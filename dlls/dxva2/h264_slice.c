#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "dxva.h"

#include "wine/debug.h"
#include "h264_slice.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxva2);

enum { SLICE_P,
       SLICE_B,
       SLICE_I,
       SLICE_SP,
       SLICE_SI };

struct bitreader {
    const unsigned char *data;
    unsigned int size;
    unsigned int pos;
    unsigned int cur;
    int bits_left;
    unsigned int zero_run;
    unsigned int rbsp_bits;
    unsigned int epb_count;
    BOOL error;
};

static void br_init(struct bitreader *br, const unsigned char *data, unsigned int size) {
    memset(br, 0, sizeof(*br));
    br->data = data;
    br->size = size;
}

static BOOL br_fill(struct bitreader *br) {
    unsigned char b;

    if (br->pos >= br->size) {
        br->error = TRUE;
        return FALSE;
    }
    b = br->data[br->pos++];
    if (br->zero_run >= 2 && b == 0x03) {
        ++br->epb_count;
        br->zero_run = 0;
        if (br->pos >= br->size) {
            br->error = TRUE;
            return FALSE;
        }
        b = br->data[br->pos++];
    }
    if (b) {
        br->zero_run = 0;
    } else {
        ++br->zero_run;
    }
    br->cur = b;
    br->bits_left = 8;
    return TRUE;
}

static unsigned int br_u1(struct bitreader *br) {
    if (br->error) {
        return 0;
    }
    if (!br->bits_left && !br_fill(br)) {
        return 0;
    }
    --br->bits_left;
    ++br->rbsp_bits;
    return (br->cur >> br->bits_left) & 1;
}

static unsigned int br_u(struct bitreader *br, int n) {
    unsigned int v = 0;

    while (n-- > 0) {
        v = (v << 1) | br_u1(br);
    }
    return v;
}

static unsigned int br_ue(struct bitreader *br) {
    int leading = 0;

    while (!br->error && !br_u1(br)) {
        if (++leading > 31) {
            br->error = TRUE;
            return 0;
        }
    }
    if (br->error) {
        return 0;
    }
    return ((1u << leading) - 1) + br_u(br, leading);
}

static int br_se(struct bitreader *br) {
    unsigned int ue = br_ue(br);

    if (!ue) {
        return 0;
    }
    return (ue & 1) ? (int)((ue + 1) >> 1) : -(int)(ue >> 1);
}

struct ref_pic {
    DXVA_PicEntry_H264 entry;
    BOOL long_term;
    int pic_num;
    unsigned int lt_idx;
    int poc;
};

struct reorder_cmd {
    unsigned int idc;
    unsigned int val;
};

static void sort_by_pic_num_desc(struct ref_pic *list, unsigned int count) {
    unsigned int i, j;

    for (i = 1; i < count; ++i) {
        struct ref_pic tmp = list[i];
        for (j = i; j && list[j - 1].pic_num < tmp.pic_num; --j) {
            list[j] = list[j - 1];
        }
        list[j] = tmp;
    }
}

static void sort_by_lt_idx_asc(struct ref_pic *list, unsigned int count) {
    unsigned int i, j;

    for (i = 1; i < count; ++i) {
        struct ref_pic tmp = list[i];
        for (j = i; j && list[j - 1].lt_idx > tmp.lt_idx; --j) {
            list[j] = list[j - 1];
        }
        list[j] = tmp;
    }
}

static void sort_by_poc(struct ref_pic *list, unsigned int count, BOOL ascending) {
    unsigned int i, j;

    for (i = 1; i < count; ++i) {
        struct ref_pic tmp = list[i];
        for (j = i; j && (ascending ? list[j - 1].poc > tmp.poc : list[j - 1].poc < tmp.poc); --j) {
            list[j] = list[j - 1];
        }
        list[j] = tmp;
    }
}

static BOOL same_pic(const struct ref_pic *a, const struct ref_pic *b) {
    return a->entry.Index7Bits == b->entry.Index7Bits && a->long_term == b->long_term;
}

static unsigned int init_list_p(const struct ref_pic *dpb, unsigned int dpb_count, struct ref_pic *out) {
    struct ref_pic st[16], lt[16];
    unsigned int i, n_st = 0, n_lt = 0, n = 0;

    for (i = 0; i < dpb_count; ++i) {
        if (dpb[i].long_term) {
            lt[n_lt++] = dpb[i];
        } else {
            st[n_st++] = dpb[i];
        }
    }
    sort_by_pic_num_desc(st, n_st);
    sort_by_lt_idx_asc(lt, n_lt);
    for (i = 0; i < n_st; ++i) {
        out[n++] = st[i];
    }
    for (i = 0; i < n_lt; ++i) {
        out[n++] = lt[i];
    }
    return n;
}

static void init_lists_b(const struct ref_pic *dpb, unsigned int dpb_count, int curr_poc,
                         struct ref_pic *l0, unsigned int *out_n0, struct ref_pic *l1, unsigned int *out_n1) {
    struct ref_pic less[16], more[16], lt[16];
    unsigned int i, n_less = 0, n_more = 0, n_lt = 0, n0 = 0, n1 = 0;

    for (i = 0; i < dpb_count; ++i) {
        if (dpb[i].long_term) {
            lt[n_lt++] = dpb[i];
        } else if (dpb[i].poc < curr_poc) {
            less[n_less++] = dpb[i];
        } else {
            more[n_more++] = dpb[i];
        }
    }
    sort_by_lt_idx_asc(lt, n_lt);

    sort_by_poc(less, n_less, FALSE);
    sort_by_poc(more, n_more, TRUE);
    for (i = 0; i < n_less; ++i) {
        l0[n0++] = less[i];
    }
    for (i = 0; i < n_more; ++i) {
        l0[n0++] = more[i];
    }
    for (i = 0; i < n_lt; ++i) {
        l0[n0++] = lt[i];
    }

    for (i = 0; i < n_more; ++i) {
        l1[n1++] = more[i];
    }
    for (i = 0; i < n_less; ++i) {
        l1[n1++] = less[i];
    }
    for (i = 0; i < n_lt; ++i) {
        l1[n1++] = lt[i];
    }

    if (n1 > 1 && n0 == n1) {
        for (i = 0; i < n1; ++i) {
            if (!same_pic(&l0[i], &l1[i])) {
                break;
            }
        }
        if (i == n1) {
            struct ref_pic tmp = l1[0];
            l1[0] = l1[1];
            l1[1] = tmp;
        }
    }

    *out_n0 = n0;
    *out_n1 = n1;
}

static void reorder_list(struct ref_pic *list, unsigned int active, const struct ref_pic *dpb,
                         unsigned int dpb_count, const struct reorder_cmd *cmds, unsigned int cmd_count,
                         int curr_pic_num, int max_pic_num) {
    int pred = curr_pic_num;
    unsigned int ref_idx = 0, c, i, n;

    for (c = 0; c < cmd_count && ref_idx <= active; ++c) {
        struct ref_pic target;
        BOOL found = FALSE;

        if (cmds[c].idc == 0 || cmds[c].idc == 1) {
            int no_wrap, pic_num;

            if (cmds[c].idc == 0) {
                no_wrap = pred - (int)(cmds[c].val + 1);
                if (no_wrap < 0) {
                    no_wrap += max_pic_num;
                }
            } else {
                no_wrap = pred + (int)(cmds[c].val + 1);
                if (no_wrap >= max_pic_num) {
                    no_wrap -= max_pic_num;
                }
            }
            pred = no_wrap;
            pic_num = no_wrap > curr_pic_num ? no_wrap - max_pic_num : no_wrap;
            for (i = 0; i < dpb_count; ++i) {
                if (!dpb[i].long_term && dpb[i].pic_num == pic_num) {
                    target = dpb[i];
                    found = TRUE;
                    break;
                }
            }
        } else {
            for (i = 0; i < dpb_count; ++i) {
                if (dpb[i].long_term && dpb[i].lt_idx == cmds[c].val) {
                    target = dpb[i];
                    found = TRUE;
                    break;
                }
            }
        }
        if (!found) {
            continue;
        }

        for (i = active; i > ref_idx; --i) {
            list[i] = list[i - 1];
        }
        list[ref_idx++] = target;
        n = ref_idx;
        for (i = ref_idx; i <= active; ++i) {
            if (!same_pic(&list[i], &target)) {
                list[n++] = list[i];
            }
        }
    }
}

static void emit_list(const struct ref_pic *list, unsigned int active, DXVA_PicEntry_H264 out[32]) {
    unsigned int i;

    for (i = 0; i < 32; ++i) {
        if (i < active && list[i].entry.bPicEntry != 0xff) {
            out[i] = list[i].entry;
        } else {
            out[i].bPicEntry = 0xff;
        }
    }
}

BOOL h264_parse_short_slice(const DXVA_PicParams_H264 *pic,
                            const void *bitstream, unsigned int bitstream_size,
                            const DXVA_Slice_H264_Short *in, DXVA_Slice_H264_Long *out) {
    const unsigned char *nal = (const unsigned char *)bitstream + in->BSNALunitDataLocation;
    unsigned int avail, sc, nal_ref_idc, nal_unit_type, slice_type, stype;
    unsigned int num_l0, num_l1, first_mb_bits, i;
    struct reorder_cmd cmd_l0[32], cmd_l1[32];
    unsigned int n_cmd_l0 = 0, n_cmd_l1 = 0;
    struct ref_pic dpb[16];
    unsigned int dpb_count = 0;
    int max_frame_num, curr_poc, idr;
    struct bitreader br;

    memset(out, 0, sizeof(*out));
    out->BSNALunitDataLocation = in->BSNALunitDataLocation;
    out->SliceBytesInBuffer = in->SliceBytesInBuffer;
    out->wBadSliceChopping = in->wBadSliceChopping;

    if (in->BSNALunitDataLocation >= bitstream_size) {
        return FALSE;
    }
    avail = bitstream_size - in->BSNALunitDataLocation;
    if (in->SliceBytesInBuffer && in->SliceBytesInBuffer < avail) {
        avail = in->SliceBytesInBuffer;
    }

    if (avail >= 3 && !nal[0] && !nal[1] && nal[2] == 1) {
        sc = 3;
    } else if (avail >= 4 && !nal[0] && !nal[1] && !nal[2] && nal[3] == 1) {
        sc = 4;
    } else {
        WARN("No start code at slice offset %u.\n", in->BSNALunitDataLocation);
        return FALSE;
    }

    br_init(&br, nal + sc, avail - sc);

    br_u1(&br);
    nal_ref_idc = br_u(&br, 2);
    nal_unit_type = br_u(&br, 5);
    idr = nal_unit_type == 5;

    max_frame_num = 1 << (pic->log2_max_frame_num_minus4 + 4);

    out->first_mb_in_slice = br_ue(&br);
    slice_type = br_ue(&br);
    out->slice_type = slice_type;
    stype = slice_type % 5;
    br_ue(&br);

    if (pic->residual_colour_transform_flag) {
        br_u(&br, 2);
    }

    br_u(&br, pic->log2_max_frame_num_minus4 + 4);

    if (!pic->frame_mbs_only_flag) {
        if (br_u1(&br)) {
            WARN("Field-coded slice is not supported.\n");
            return FALSE;
        }
    }

    if (idr) {
        br_ue(&br);
    }

    if (pic->pic_order_cnt_type == 0) {
        br_u(&br, pic->log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (pic->pic_order_present_flag) {
            br_se(&br);
        }
    } else if (pic->pic_order_cnt_type == 1 && !pic->delta_pic_order_always_zero_flag) {
        br_se(&br);
        if (pic->pic_order_present_flag) {
            br_se(&br);
        }
    }

    if (pic->redundant_pic_cnt_present_flag) {
        br_ue(&br);
    }

    if (stype == SLICE_B) {
        out->direct_spatial_mv_pred_flag = br_u1(&br);
    }

    num_l0 = num_l1 = 0;
    if (stype != SLICE_I && stype != SLICE_SI) {
        num_l0 = pic->num_ref_idx_l0_active_minus1;
        if (stype == SLICE_B) {
            num_l1 = pic->num_ref_idx_l1_active_minus1;
        }
        if (br_u1(&br)) {
            num_l0 = br_ue(&br);
            if (stype == SLICE_B) {
                num_l1 = br_ue(&br);
            }
        }
    }
    if (num_l0 > 31) {
        num_l0 = 31;
    }
    if (num_l1 > 31) {
        num_l1 = 31;
    }
    out->num_ref_idx_l0_active_minus1 = num_l0;
    out->num_ref_idx_l1_active_minus1 = num_l1;

    if (stype != SLICE_I && stype != SLICE_SI) {
        if (br_u1(&br)) {
            unsigned int idc;
            while (!br.error && (idc = br_ue(&br)) != 3) {
                if (idc > 3) {
                    br.error = TRUE;
                    break;
                }
                if (n_cmd_l0 < ARRAY_SIZE(cmd_l0)) {
                    cmd_l0[n_cmd_l0].idc = idc;
                    cmd_l0[n_cmd_l0].val = br_ue(&br);
                    ++n_cmd_l0;
                } else {
                    br_ue(&br);
                }
            }
        }
    }
    if (stype == SLICE_B) {
        if (br_u1(&br)) {
            unsigned int idc;
            while (!br.error && (idc = br_ue(&br)) != 3) {
                if (idc > 3) {
                    br.error = TRUE;
                    break;
                }
                if (n_cmd_l1 < ARRAY_SIZE(cmd_l1)) {
                    cmd_l1[n_cmd_l1].idc = idc;
                    cmd_l1[n_cmd_l1].val = br_ue(&br);
                    ++n_cmd_l1;
                } else {
                    br_ue(&br);
                }
            }
        }
    }

    if ((pic->weighted_pred_flag && (stype == SLICE_P || stype == SLICE_SP)) || (pic->weighted_bipred_idc == 1 && stype == SLICE_B)) {
        unsigned int chroma = !pic->residual_colour_transform_flag && pic->chroma_format_idc;
        unsigned int list, count;

        out->luma_log2_weight_denom = br_ue(&br);
        if (chroma) {
            out->chroma_log2_weight_denom = br_ue(&br);
        }

        for (list = 0; list < (unsigned int)(stype == SLICE_B ? 2 : 1); ++list) {
            count = (list ? num_l1 : num_l0) + 1;
            for (i = 0; i < count && i < 32; ++i) {
                out->Weights[list][i][0][0] = 1 << out->luma_log2_weight_denom;
                out->Weights[list][i][0][1] = 0;
                out->Weights[list][i][1][0] = out->Weights[list][i][2][0] = 1 << out->chroma_log2_weight_denom;
                out->Weights[list][i][1][1] = out->Weights[list][i][2][1] = 0;

                if (br_u1(&br)) {
                    out->Weights[list][i][0][0] = br_se(&br);
                    out->Weights[list][i][0][1] = br_se(&br);
                }
                if (chroma && br_u1(&br)) {
                    unsigned int j;
                    for (j = 1; j <= 2; ++j) {
                        out->Weights[list][i][j][0] = br_se(&br);
                        out->Weights[list][i][j][1] = br_se(&br);
                    }
                }
            }
        }
    }

    if (nal_ref_idc) {
        if (idr) {
            br_u1(&br);
            br_u1(&br);
        } else if (br_u1(&br)) {
            unsigned int op;
            while (!br.error && (op = br_ue(&br))) {
                if (op > 6) {
                    br.error = TRUE;
                    break;
                }
                if (op == 1 || op == 3) {
                    br_ue(&br);
                }
                if (op == 2) {
                    br_ue(&br);
                }
                if (op == 3 || op == 6) {
                    br_ue(&br);
                }
                if (op == 4) {
                    br_ue(&br);
                }
            }
        }
    }

    if (pic->entropy_coding_mode_flag && stype != SLICE_I && stype != SLICE_SI) {
        out->cabac_init_idc = br_ue(&br);
    }

    out->slice_qp_delta = br_se(&br);

    if (stype == SLICE_SP) {
        br_u1(&br);
    }
    if (stype == SLICE_SP || stype == SLICE_SI) {
        br_se(&br);
    }

    if (pic->deblocking_filter_control_present_flag) {
        out->disable_deblocking_filter_idc = br_ue(&br);
        if (out->disable_deblocking_filter_idc != 1) {
            out->slice_alpha_c0_offset_div2 = br_se(&br);
            out->slice_beta_offset_div2 = br_se(&br);
        }
    }

    if (pic->num_slice_groups_minus1 > 0 && pic->slice_group_map_type >= 3 && pic->slice_group_map_type <= 5) {
        WARN("FMO slice groups are not supported.\n");
        return FALSE;
    }

    if (br.error) {
        WARN("Ran out of slice header data.\n");
        return FALSE;
    }

    first_mb_bits = br.rbsp_bits;
    if (pic->entropy_coding_mode_flag) {
        first_mb_bits += (8 - (first_mb_bits & 7)) & 7;
    }
    if (!br.error && !br.bits_left && br.zero_run >= 2 && br.pos < br.size && br.data[br.pos] == 0x03) {
        ++br.epb_count;
    }
    out->BitOffsetToSliceData = sc * 8 + first_mb_bits + br.epb_count * 8;
    if (br.epb_count) {
        WARN("%u emulation-prevention byte(s) inside the slice header.\n", br.epb_count);
    }

    if (stype == SLICE_I || stype == SLICE_SI) {
        for (i = 0; i < 32; ++i) {
            out->RefPicList[0][i].bPicEntry = out->RefPicList[1][i].bPicEntry = 0xff;
        }
        return TRUE;
    }

    curr_poc = min(pic->CurrFieldOrderCnt[0], pic->CurrFieldOrderCnt[1]);
    for (i = 0; i < 16; ++i) {
        unsigned int used = (pic->UsedForReferenceFlags >> (2 * i)) & 3;

        if (pic->RefFrameList[i].bPicEntry == 0xff) {
            continue;
        }
        if (!used) {
            continue;
        }

        dpb[dpb_count].entry = pic->RefFrameList[i];
        dpb[dpb_count].long_term = pic->RefFrameList[i].AssociatedFlag;
        dpb[dpb_count].poc = min(pic->FieldOrderCntList[i][0], pic->FieldOrderCntList[i][1]);
        if (dpb[dpb_count].long_term) {
            dpb[dpb_count].lt_idx = pic->FrameNumList[i];
        } else {
            int fn = pic->FrameNumList[i];
            dpb[dpb_count].pic_num = fn > (int)pic->frame_num ? fn - max_frame_num : fn;
        }
        ++dpb_count;
    }

    {
        struct ref_pic l0[40], l1[40];
        unsigned int n0 = 0, n1 = 0;

        for (i = 0; i < ARRAY_SIZE(l0); ++i) {
            l0[i].entry.bPicEntry = l1[i].entry.bPicEntry = 0xff;
        }

        if (stype == SLICE_B) {
            init_lists_b(dpb, dpb_count, curr_poc, l0, &n0, l1, &n1);
        } else {
            n0 = init_list_p(dpb, dpb_count, l0);
        }

        reorder_list(l0, num_l0 + 1, dpb, dpb_count, cmd_l0, n_cmd_l0, pic->frame_num, max_frame_num);
        emit_list(l0, num_l0 + 1, out->RefPicList[0]);

        if (stype == SLICE_B) {
            reorder_list(l1, num_l1 + 1, dpb, dpb_count, cmd_l1, n_cmd_l1, pic->frame_num, max_frame_num);
            emit_list(l1, num_l1 + 1, out->RefPicList[1]);
        } else {
            emit_list(l1, 0, out->RefPicList[1]);
        }

        (void)n0;
        (void)n1;
    }

    return TRUE;
}
