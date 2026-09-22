#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>


#ifdef HAVE_VA_VA_DRM_H
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#endif
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "dxva.h"
#include "unixlib.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

/* clang-format off */
#include "winbase.h"
/* clang-format on */

WINE_DEFAULT_DEBUG_CHANNEL(dxva2);

#ifdef HAVE_VA_VA_DRM_H

enum {
    SLICE_TYPE_P,
    SLICE_TYPE_B,
    SLICE_TYPE_I,
    SLICE_TYPE_SP,
    SLICE_TYPE_SI,
};

struct va_decoder {
    VAConfigID config;
    VAContextID context;
    VASurfaceID *surfaces;
    unsigned int surface_count;
    unsigned int width;
    unsigned int height;
};

static VADisplay va_display;
static int va_fd = -1;
static unsigned int va_refcount;
static pthread_mutex_t va_mutex = PTHREAD_MUTEX_INITIALIZER;
static BOOL allow_derive = TRUE;
static BOOL have_sse41;

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("sse4.1")))
static void copy_plane_nt(unsigned char *dst, unsigned int dst_pitch, const unsigned char *src,
                          unsigned int src_pitch, unsigned int width, unsigned int height) {
    unsigned int row;

    for (row = 0; row < height; ++row) {
        const unsigned char *s = src + (size_t)row * src_pitch;
        unsigned char *d = dst + (size_t)row * dst_pitch;
        size_t left = width;
        size_t head;

        /* MOVNTDQA requires a 16-byte aligned source. */
        head = (-(uintptr_t)s) & 15;
        if (head > left) {
            head = left;
        }
        if (head) {
            memcpy(d, s, head);
            s += head;
            d += head;
            left -= head;
        }

        while (left >= 64) {
            __m128i a = _mm_stream_load_si128((__m128i *)(void *)s);
            __m128i b = _mm_stream_load_si128((__m128i *)(void *)(s + 16));
            __m128i c = _mm_stream_load_si128((__m128i *)(void *)(s + 32));
            __m128i e = _mm_stream_load_si128((__m128i *)(void *)(s + 48));

            _mm_storeu_si128((__m128i *)(void *)d, a);
            _mm_storeu_si128((__m128i *)(void *)(d + 16), b);
            _mm_storeu_si128((__m128i *)(void *)(d + 32), c);
            _mm_storeu_si128((__m128i *)(void *)(d + 48), e);
            s += 64;
            d += 64;
            left -= 64;
        }
        while (left >= 16) {
            _mm_storeu_si128((__m128i *)(void *)d, _mm_stream_load_si128((__m128i *)(void *)s));
            s += 16;
            d += 16;
            left -= 16;
        }
        if (left) {
            memcpy(d, s, left);
        }
    }

    _mm_mfence();
}
#endif

static void copy_plane(unsigned char *dst, unsigned int dst_pitch, const unsigned char *src,
                       unsigned int src_pitch, unsigned int width, unsigned int height) {
    unsigned int row;

#if defined(__x86_64__) || defined(__i386__)
    if (have_sse41) {
        copy_plane_nt(dst, dst_pitch, src, src_pitch, width, height);
        return;
    }
#endif

    for (row = 0; row < height; ++row) {
        memcpy(dst + (size_t)row * dst_pitch, src + (size_t)row * src_pitch, width);
    }
}

static void init_copy_features(void) {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;

    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        have_sse41 = !!(ecx & bit_SSE4_1);
    }
#endif
    TRACE("Using %s frame readback.\n", have_sse41 ? "streaming-load" : "plain");
}

static BOOL ensure_display(void) {
    static const char *const nodes[] = {"/dev/dri/renderD128", "/dev/dri/renderD129"};
    static BOOL features_done;
    unsigned int i;
    int major, minor;

    if (!features_done) {
        const char *env = getenv("WINE_DXVA2_NO_DERIVE");

        features_done = TRUE;
        init_copy_features();
        if (env && *env != '0') {
            allow_derive = FALSE;
            TRACE("vaDeriveImage() disabled by WINE_DXVA2_NO_DERIVE.\n");
        }
    }

    if (va_display) {
        return TRUE;
    }

    for (i = 0; i < ARRAY_SIZE(nodes); ++i) {
        VADisplay display;
        int fd;

        if ((fd = open(nodes[i], O_RDWR | O_CLOEXEC)) < 0) {
            continue;
        }
        if (!(display = vaGetDisplayDRM(fd))) {
            close(fd);
            continue;
        }
        if (vaInitialize(display, &major, &minor) == VA_STATUS_SUCCESS) {
            const char *vendor = vaQueryVendorString(display);

            TRACE("Opened VA-API %d.%d on %s (%s).\n", major, minor, nodes[i],
                  vendor ? vendor : "unknown driver");
            va_display = display;
            va_fd = fd;
            return TRUE;
        }
        vaTerminate(display);
        close(fd);
    }

    WARN("Failed to open a VA-API display on any DRM render node.\n");
    return FALSE;
}

static void release_display(void) {
    if (va_refcount || !va_display) {
        return;
    }

    vaTerminate(va_display);
    close(va_fd);
    va_display = NULL;
    va_fd = -1;
}

static VAConfigID create_h264_config(void) {
    static const VAProfile profiles[] =
        {
            VAProfileH264High,
            VAProfileH264Main,
            VAProfileH264ConstrainedBaseline,
        };
    VAConfigAttrib attrib = {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420};
    VAConfigID config = VA_INVALID_ID;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(profiles); ++i) {
        if (vaCreateConfig(va_display, profiles[i], VAEntrypointVLD, &attrib, 1, &config) == VA_STATUS_SUCCESS) {
            TRACE("Created config %#x for profile %d.\n", config, profiles[i]);
            return config;
        }
    }

    WARN("No usable H.264 VLD profile.\n");
    return VA_INVALID_ID;
}

#define DXVA_DEFAULT_MIN_SIZE 16
#define DXVA_DEFAULT_MAX_SIZE 4096

static void query_decoder_caps(VAConfigID config, struct get_decoder_caps_params *caps) {
    unsigned int num_attribs = 0;
    VASurfaceAttrib *attribs;
    VAStatus status;
    unsigned int i;

    caps->min_width = caps->min_height = DXVA_DEFAULT_MIN_SIZE;
    caps->max_width = caps->max_height = DXVA_DEFAULT_MAX_SIZE;

    if ((status = vaQuerySurfaceAttributes(va_display, config, NULL, &num_attribs)) != VA_STATUS_SUCCESS) {
        WARN("Failed to query surface attribute count: %s\n", vaErrorStr(status));
        return;
    }

    if (!num_attribs) {
        WARN("The driver reports no surface attributes.\n");
        return;
    }

    if (!(attribs = calloc(num_attribs, sizeof(*attribs)))) {
        return;
    }

    if ((status = vaQuerySurfaceAttributes(va_display, config, attribs, &num_attribs)) != VA_STATUS_SUCCESS) {
        WARN("Failed to query surface attributes: %s\n", vaErrorStr(status));
        free(attribs);
        return;
    }

    for (i = 0; i < num_attribs; ++i) {
        if (!(attribs[i].flags & VA_SURFACE_ATTRIB_GETTABLE) ||
            attribs[i].value.type != VAGenericValueTypeInteger ||
            attribs[i].value.value.i <= 0) {
            continue;
        }

        switch (attribs[i].type) {
        case VASurfaceAttribMinWidth:
            caps->min_width = attribs[i].value.value.i;
            break;
        case VASurfaceAttribMinHeight:
            caps->min_height = attribs[i].value.value.i;
            break;
        case VASurfaceAttribMaxWidth:
            caps->max_width = attribs[i].value.value.i;
            break;
        case VASurfaceAttribMaxHeight:
            caps->max_height = attribs[i].value.value.i;
            break;
        default:
            break;
        }
    }

    free(attribs);

    if (caps->max_width < caps->min_width || caps->max_height < caps->min_height) {
        WARN("Driver reported a bogus size range; falling back to defaults.\n");
        caps->min_width = caps->min_height = DXVA_DEFAULT_MIN_SIZE;
        caps->max_width = caps->max_height = DXVA_DEFAULT_MAX_SIZE;
    }

    TRACE("Supported sizes are %ux%u to %ux%u.\n",
          caps->min_width, caps->min_height, caps->max_width, caps->max_height);
}

static NTSTATUS va_get_decoder_caps(void *args) {
    struct get_decoder_caps_params *params = args;
    VAConfigID config;

    pthread_mutex_lock(&va_mutex);

    if (!ensure_display()) {
        pthread_mutex_unlock(&va_mutex);
        return STATUS_NOT_SUPPORTED;
    }

    if ((config = create_h264_config()) == VA_INVALID_ID) {
        release_display();
        pthread_mutex_unlock(&va_mutex);
        return STATUS_NOT_SUPPORTED;
    }

    query_decoder_caps(config, params);
    vaDestroyConfig(va_display, config);
    release_display();

    pthread_mutex_unlock(&va_mutex);
    return STATUS_SUCCESS;
}

static void fill_va_reference(const struct va_decoder *dec, VAPictureH264 *out,
                              DXVA_PicEntry_H264 entry, INT top_poc, INT bottom_poc, UINT frame_num) {
    if (entry.bPicEntry == 0xff || entry.Index7Bits >= dec->surface_count) {
        out->picture_id = VA_INVALID_SURFACE;
        out->frame_idx = 0;
        out->flags = VA_PICTURE_H264_INVALID;
        out->TopFieldOrderCnt = 0;
        out->BottomFieldOrderCnt = 0;
        return;
    }

    out->picture_id = dec->surfaces[entry.Index7Bits];
    out->frame_idx = frame_num;
    out->flags = entry.AssociatedFlag ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                                      : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    out->TopFieldOrderCnt = top_poc;
    out->BottomFieldOrderCnt = bottom_poc;
}

static void fill_va_slice_reference(const struct va_decoder *dec, const DXVA_PicParams_H264 *pp,
                                    VAPictureH264 *out, DXVA_PicEntry_H264 entry) {
    unsigned int i;

    if (entry.bPicEntry == 0xff || entry.Index7Bits >= dec->surface_count) {
        out->picture_id = VA_INVALID_SURFACE;
        out->frame_idx = 0;
        out->flags = VA_PICTURE_H264_INVALID;
        out->TopFieldOrderCnt = 0;
        out->BottomFieldOrderCnt = 0;
        return;
    }

    out->picture_id = dec->surfaces[entry.Index7Bits];
    out->flags = entry.AssociatedFlag ? VA_PICTURE_H264_LONG_TERM_REFERENCE
                                      : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    out->frame_idx = 0;
    out->TopFieldOrderCnt = 0;
    out->BottomFieldOrderCnt = 0;

    for (i = 0; i < 16; ++i) {
        if (pp->RefFrameList[i].bPicEntry != 0xff && pp->RefFrameList[i].Index7Bits == entry.Index7Bits) {
            out->frame_idx = pp->FrameNumList[i];
            out->TopFieldOrderCnt = pp->FieldOrderCntList[i][0];
            out->BottomFieldOrderCnt = pp->FieldOrderCntList[i][1];
            break;
        }
    }
}

static void fill_va_picture_parameters(const struct va_decoder *dec, const DXVA_PicParams_H264 *pp,
                                       VAPictureParameterBufferH264 *vp) {
    unsigned int i;

    memset(vp, 0, sizeof(*vp));

    vp->CurrPic.picture_id = pp->CurrPic.Index7Bits < dec->surface_count
        ? dec->surfaces[pp->CurrPic.Index7Bits]
        : VA_INVALID_SURFACE;
    vp->CurrPic.frame_idx = pp->frame_num;
    vp->CurrPic.flags = 0;
    if (pp->field_pic_flag) {
        vp->CurrPic.flags |= pp->CurrPic.AssociatedFlag ? VA_PICTURE_H264_BOTTOM_FIELD
                                                        : VA_PICTURE_H264_TOP_FIELD;
    }
    vp->CurrPic.TopFieldOrderCnt = pp->CurrFieldOrderCnt[0];
    vp->CurrPic.BottomFieldOrderCnt = pp->CurrFieldOrderCnt[1];

    for (i = 0; i < 16; ++i) {
        fill_va_reference(dec, &vp->ReferenceFrames[i], pp->RefFrameList[i],
                          pp->FieldOrderCntList[i][0], pp->FieldOrderCntList[i][1], pp->FrameNumList[i]);
    }

    vp->picture_width_in_mbs_minus1 = pp->wFrameWidthInMbsMinus1;
    vp->picture_height_in_mbs_minus1 = pp->wFrameHeightInMbsMinus1;
    vp->bit_depth_luma_minus8 = pp->bit_depth_luma_minus8;
    vp->bit_depth_chroma_minus8 = pp->bit_depth_chroma_minus8;
    vp->num_ref_frames = pp->num_ref_frames;

    vp->seq_fields.bits.chroma_format_idc = pp->chroma_format_idc;
    vp->seq_fields.bits.residual_colour_transform_flag = pp->residual_colour_transform_flag;
    vp->seq_fields.bits.frame_mbs_only_flag = pp->frame_mbs_only_flag;
    vp->seq_fields.bits.mb_adaptive_frame_field_flag = pp->MbaffFrameFlag;
    vp->seq_fields.bits.direct_8x8_inference_flag = pp->direct_8x8_inference_flag;
    vp->seq_fields.bits.MinLumaBiPredSize8x8 = pp->MinLumaBipredSize8x8Flag;
    vp->seq_fields.bits.log2_max_frame_num_minus4 = pp->log2_max_frame_num_minus4;
    vp->seq_fields.bits.pic_order_cnt_type = pp->pic_order_cnt_type;
    vp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = pp->log2_max_pic_order_cnt_lsb_minus4;
    vp->seq_fields.bits.delta_pic_order_always_zero_flag = pp->delta_pic_order_always_zero_flag;

    vp->pic_init_qp_minus26 = pp->pic_init_qp_minus26;
    vp->pic_init_qs_minus26 = pp->pic_init_qs_minus26;
    vp->chroma_qp_index_offset = pp->chroma_qp_index_offset;
    vp->second_chroma_qp_index_offset = pp->second_chroma_qp_index_offset;

    vp->pic_fields.bits.entropy_coding_mode_flag = pp->entropy_coding_mode_flag;
    vp->pic_fields.bits.weighted_pred_flag = pp->weighted_pred_flag;
    vp->pic_fields.bits.weighted_bipred_idc = pp->weighted_bipred_idc;
    vp->pic_fields.bits.transform_8x8_mode_flag = pp->transform_8x8_mode_flag;
    vp->pic_fields.bits.field_pic_flag = pp->field_pic_flag;
    vp->pic_fields.bits.constrained_intra_pred_flag = pp->constrained_intra_pred_flag;
    vp->pic_fields.bits.pic_order_present_flag = pp->pic_order_present_flag;
    vp->pic_fields.bits.deblocking_filter_control_present_flag = pp->deblocking_filter_control_present_flag;
    vp->pic_fields.bits.redundant_pic_cnt_present_flag = pp->redundant_pic_cnt_present_flag;
    vp->pic_fields.bits.reference_pic_flag = pp->RefPicFlag;

    vp->frame_num = pp->frame_num;
}

static void fill_va_slice_parameters(const struct va_decoder *dec, const DXVA_PicParams_H264 *pp,
                                     const DXVA_Slice_H264_Long *slice, VASliceParameterBufferH264 *vs) {
    unsigned int i, j, stype = slice->slice_type % 5;
    BOOL has_weights, has_chroma;

    memset(vs, 0, sizeof(*vs));

    vs->slice_data_size = slice->SliceBytesInBuffer;
    vs->slice_data_offset = 0;
    vs->slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    vs->slice_data_bit_offset = slice->BitOffsetToSliceData;
    vs->first_mb_in_slice = slice->first_mb_in_slice;
    vs->slice_type = stype;
    vs->direct_spatial_mv_pred_flag = slice->direct_spatial_mv_pred_flag;
    vs->num_ref_idx_l0_active_minus1 = slice->num_ref_idx_l0_active_minus1;
    vs->num_ref_idx_l1_active_minus1 = slice->num_ref_idx_l1_active_minus1;
    vs->cabac_init_idc = slice->cabac_init_idc;
    vs->slice_qp_delta = slice->slice_qp_delta;
    vs->disable_deblocking_filter_idc = slice->disable_deblocking_filter_idc;
    vs->slice_alpha_c0_offset_div2 = slice->slice_alpha_c0_offset_div2;
    vs->slice_beta_offset_div2 = slice->slice_beta_offset_div2;
    vs->luma_log2_weight_denom = slice->luma_log2_weight_denom;
    vs->chroma_log2_weight_denom = slice->chroma_log2_weight_denom;

    for (i = 0; i < 32; ++i) {
        fill_va_slice_reference(dec, pp, &vs->RefPicList0[i], slice->RefPicList[0][i]);
        fill_va_slice_reference(dec, pp, &vs->RefPicList1[i], slice->RefPicList[1][i]);
    }

    has_weights = (pp->weighted_pred_flag && (stype == SLICE_TYPE_P || stype == SLICE_TYPE_SP))
                || (pp->weighted_bipred_idc == 1 && stype == SLICE_TYPE_B);
    has_chroma = !pp->residual_colour_transform_flag && pp->chroma_format_idc;

    vs->luma_weight_l0_flag = has_weights;
    vs->chroma_weight_l0_flag = has_weights && has_chroma;
    vs->luma_weight_l1_flag = has_weights && stype == SLICE_TYPE_B;
    vs->chroma_weight_l1_flag = vs->luma_weight_l1_flag && has_chroma;

    for (i = 0; i < 32; ++i) {
        if (vs->luma_weight_l0_flag) {
            vs->luma_weight_l0[i] = slice->Weights[0][i][0][0];
            vs->luma_offset_l0[i] = slice->Weights[0][i][0][1];
        } else {
            vs->luma_weight_l0[i] = 1 << vs->luma_log2_weight_denom;
            vs->luma_offset_l0[i] = 0;
        }
        if (vs->luma_weight_l1_flag) {
            vs->luma_weight_l1[i] = slice->Weights[1][i][0][0];
            vs->luma_offset_l1[i] = slice->Weights[1][i][0][1];
        } else {
            vs->luma_weight_l1[i] = 1 << vs->luma_log2_weight_denom;
            vs->luma_offset_l1[i] = 0;
        }
        for (j = 0; j < 2; ++j) {
            if (vs->chroma_weight_l0_flag) {
                vs->chroma_weight_l0[i][j] = slice->Weights[0][i][1 + j][0];
                vs->chroma_offset_l0[i][j] = slice->Weights[0][i][1 + j][1];
            } else {
                vs->chroma_weight_l0[i][j] = 1 << vs->chroma_log2_weight_denom;
                vs->chroma_offset_l0[i][j] = 0;
            }
            if (vs->chroma_weight_l1_flag) {
                vs->chroma_weight_l1[i][j] = slice->Weights[1][i][1 + j][0];
                vs->chroma_offset_l1[i][j] = slice->Weights[1][i][1 + j][1];
            } else {
                vs->chroma_weight_l1[i][j] = 1 << vs->chroma_log2_weight_denom;
                vs->chroma_offset_l1[i][j] = 0;
            }
        }
    }
}

static NTSTATUS va_create_decoder(void *args) {
    struct create_decoder_params *params = args;
    struct get_decoder_caps_params caps;
    VASurfaceAttrib attrib;
    struct va_decoder *dec;
    VAStatus status;

    pthread_mutex_lock(&va_mutex);

    if (!ensure_display()) {
        pthread_mutex_unlock(&va_mutex);
        return STATUS_NOT_SUPPORTED;
    }

    if (!(dec = calloc(1, sizeof(*dec)))) {
        release_display();
        pthread_mutex_unlock(&va_mutex);
        return STATUS_NO_MEMORY;
    }
    dec->config = VA_INVALID_ID;
    dec->width = params->width;
    dec->height = params->height;
    dec->surface_count = params->surface_count;

    if (params->surface_count && !(dec->surfaces = calloc(params->surface_count, sizeof(*dec->surfaces)))) {
        goto fail;
    }

    if ((dec->config = create_h264_config()) == VA_INVALID_ID) {
        goto fail;
    }

    query_decoder_caps(dec->config, &caps);
    if (params->width < caps.min_width || params->width > caps.max_width ||
        params->height < caps.min_height || params->height > caps.max_height) {
        WARN("Size %ux%u is outside of the supported range %ux%u to %ux%u.\n", params->width, params->height,
             caps.min_width, caps.min_height, caps.max_width, caps.max_height);
        goto fail;
    }

    attrib.type = VASurfaceAttribPixelFormat;
    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.value.type = VAGenericValueTypeInteger;
    attrib.value.value.i = VA_FOURCC_NV12;
    if ((status = vaCreateSurfaces(va_display, VA_RT_FORMAT_YUV420, params->width, params->height,
                                   dec->surfaces, params->surface_count, &attrib, 1)) != VA_STATUS_SUCCESS) {
        ERR("vaCreateSurfaces failed: %s\n", vaErrorStr(status));
        goto fail;
    }

    if ((status = vaCreateContext(va_display, dec->config, params->width, params->height,
                                  VA_PROGRESSIVE, dec->surfaces, params->surface_count, &dec->context)) != VA_STATUS_SUCCESS) {
        ERR("vaCreateContext failed: %s\n", vaErrorStr(status));
        vaDestroySurfaces(va_display, dec->surfaces, params->surface_count);
        goto fail;
    }

    ++va_refcount;
    pthread_mutex_unlock(&va_mutex);

    params->decoder = (va_decoder_handle)(UINT_PTR)dec;
    TRACE("Created decoder %p (%ux%u, %u surfaces).\n", dec, dec->width, dec->height, dec->surface_count);
    return STATUS_SUCCESS;

fail:
    if (dec->config != VA_INVALID_ID) {
        vaDestroyConfig(va_display, dec->config);
    }
    release_display();
    free(dec->surfaces);
    free(dec);
    pthread_mutex_unlock(&va_mutex);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS va_decode_h264(void *args) {
    struct decode_h264_params *params = args;
    struct va_decoder *dec = (struct va_decoder *)(UINT_PTR)params->decoder;
    VAPictureParameterBufferH264 pic_param;
    VASliceParameterBufferH264 *slice_params;
    NTSTATUS ret = STATUS_UNSUCCESSFUL;
    VAIQMatrixBufferH264 iq_matrix;
    unsigned int i, buf_count = 0;
    VABufferID *bufs = NULL;
    VASurfaceID target;
    VAStatus status;

    if (!dec || params->pic_params->CurrPic.Index7Bits >= dec->surface_count) {
        return STATUS_INVALID_PARAMETER;
    }

    if (params->pic_params->num_slice_groups_minus1) {
        static BOOL once;
        if (!once++) {
            WARN("FMO slice groups are not supported.\n");
        }
        return STATUS_NOT_SUPPORTED;
    }

    if (!params->slice_count || !(slice_params = calloc(params->slice_count, sizeof(*slice_params)))) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!(bufs = calloc(2 + 2 * (size_t)params->slice_count, sizeof(*bufs)))) {
        free(slice_params);
        return STATUS_NO_MEMORY;
    }

    target = dec->surfaces[params->pic_params->CurrPic.Index7Bits];

    fill_va_picture_parameters(dec, params->pic_params, &pic_param);
    for (i = 0; i < params->slice_count; ++i) {
        fill_va_slice_parameters(dec, params->pic_params, &params->slices[i], &slice_params[i]);
    }

    pthread_mutex_lock(&va_mutex);

    if ((status = vaBeginPicture(va_display, dec->context, target)) != VA_STATUS_SUCCESS) {
        ERR("vaBeginPicture failed: %s\n", vaErrorStr(status));
        goto done;
    }

    if (vaCreateBuffer(va_display, dec->context, VAPictureParameterBufferType,
                       sizeof(pic_param), 1, &pic_param, &bufs[buf_count]) == VA_STATUS_SUCCESS) {
        ++buf_count;
    }

    if (params->qmatrix) {
        memcpy(iq_matrix.ScalingList4x4, params->qmatrix->bScalingLists4x4, sizeof(iq_matrix.ScalingList4x4));
        memcpy(iq_matrix.ScalingList8x8, params->qmatrix->bScalingLists8x8, sizeof(iq_matrix.ScalingList8x8));
        if (vaCreateBuffer(va_display, dec->context, VAIQMatrixBufferType,
                           sizeof(iq_matrix), 1, &iq_matrix, &bufs[buf_count]) == VA_STATUS_SUCCESS) {
            ++buf_count;
        }
    }

    for (i = 0; i < params->slice_count; ++i) {
        unsigned int offset = params->slices[i].BSNALunitDataLocation;
        unsigned int size = slice_params[i].slice_data_size;

        if (offset >= params->bitstream_size) {
            WARN("Slice %u starts past the end of the bitstream, skipping.\n", i);
            continue;
        }
        if (size > params->bitstream_size - offset) {
            size = params->bitstream_size - offset;
            slice_params[i].slice_data_size = size;
        }
        if (!size) {
            WARN("Slice %u is empty, skipping.\n", i);
            continue;
        }

        if (vaCreateBuffer(va_display, dec->context, VASliceParameterBufferType,
                           sizeof(*slice_params), 1, &slice_params[i],
                           &bufs[buf_count]) != VA_STATUS_SUCCESS) {
            continue;
        }
        ++buf_count;

        if (vaCreateBuffer(va_display, dec->context, VASliceDataBufferType, size, 1,
                           (void *)((const char *)params->bitstream + offset),
                           &bufs[buf_count]) != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(va_display, bufs[--buf_count]);
            continue;
        }
        ++buf_count;
    }

    if (buf_count
        && (status = vaRenderPicture(va_display, dec->context, bufs, buf_count)) != VA_STATUS_SUCCESS) {
        ERR("vaRenderPicture failed: %s\n", vaErrorStr(status));
        vaEndPicture(va_display, dec->context);
        goto done;
    }

    if ((status = vaEndPicture(va_display, dec->context)) != VA_STATUS_SUCCESS) {
        ERR("vaEndPicture failed: %s\n", vaErrorStr(status));
        goto done;
    }

    ret = STATUS_SUCCESS;

done:
    for (i = 0; i < buf_count; ++i) {
        vaDestroyBuffer(va_display, bufs[i]);
    }
    pthread_mutex_unlock(&va_mutex);
    free(bufs);
    free(slice_params);
    return ret;
}

static NTSTATUS va_copy_surface(void *args) {
    struct copy_surface_params *params = args;
    struct va_decoder *dec = (struct va_decoder *)(UINT_PTR)params->decoder;
    unsigned int w = params->width, h = params->height;
    unsigned char *src, *dst = params->dst;
    NTSTATUS ret = STATUS_UNSUCCESSFUL;
    VASurfaceID surface;
    VAStatus status;
    BOOL derived;
    VAImage image;

    if (!dec || params->surface_index >= dec->surface_count) {
        return STATUS_INVALID_PARAMETER;
    }
    surface = dec->surfaces[params->surface_index];

    pthread_mutex_lock(&va_mutex);

    vaSyncSurface(va_display, surface);

    derived = FALSE;
    if (allow_derive && vaDeriveImage(va_display, surface, &image) == VA_STATUS_SUCCESS) {
        if (image.format.fourcc == VA_FOURCC_NV12 && image.num_planes >= 2
            && image.width >= w && image.height >= h) {
            derived = TRUE;
        } else {
            TRACE("Derived image is %ux%u fourcc %#x with %u plane(s), converting instead.\n",
                  image.width, image.height, image.format.fourcc, image.num_planes);
            vaDestroyImage(va_display, image.image_id);
        }
    }

    if (!derived) {
        VAImageFormat format = {.fourcc = VA_FOURCC_NV12, .byte_order = VA_LSB_FIRST, .bits_per_pixel = 12};

        if ((status = vaCreateImage(va_display, &format, w, h, &image)) != VA_STATUS_SUCCESS) {
            ERR("vaCreateImage failed: %s\n", vaErrorStr(status));
            goto unlock;
        }
        if ((status = vaGetImage(va_display, surface, 0, 0, w, h, image.image_id)) != VA_STATUS_SUCCESS) {
            ERR("vaGetImage failed: %s\n", vaErrorStr(status));
            vaDestroyImage(va_display, image.image_id);
            goto unlock;
        }
    }

    if ((status = vaMapBuffer(va_display, image.buf, (void **)&src)) != VA_STATUS_SUCCESS) {
        ERR("vaMapBuffer failed: %s\n", vaErrorStr(status));
        vaDestroyImage(va_display, image.image_id);
        goto unlock;
    }

    copy_plane(dst, params->dst_pitch, src + image.offsets[0], image.pitches[0], w, h);
    copy_plane(dst + (size_t)h * params->dst_pitch, params->dst_pitch,
               src + image.offsets[1], image.pitches[1], w, h / 2);

    vaUnmapBuffer(va_display, image.buf);
    vaDestroyImage(va_display, image.image_id);
    ret = STATUS_SUCCESS;

unlock:
    pthread_mutex_unlock(&va_mutex);
    return ret;
}

static NTSTATUS va_destroy_decoder(void *args) {
    struct destroy_decoder_params *params = args;
    struct va_decoder *dec = (struct va_decoder *)(UINT_PTR)params->decoder;

    if (!dec) {
        return STATUS_INVALID_PARAMETER;
    }

    pthread_mutex_lock(&va_mutex);

    if (dec->context) {
        vaDestroyContext(va_display, dec->context);
    }
    if (dec->surface_count) {
        vaDestroySurfaces(va_display, dec->surfaces, dec->surface_count);
    }
    if (dec->config != VA_INVALID_ID) {
        vaDestroyConfig(va_display, dec->config);
    }

    if (va_refcount) {
        --va_refcount;
    }
    release_display();

    pthread_mutex_unlock(&va_mutex);

    free(dec->surfaces);
    free(dec);
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
    {
        va_get_decoder_caps,
        va_create_decoder,
        va_decode_h264,
        va_copy_surface,
        va_destroy_decoder,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_dxva2_funcs_count);

#else /* HAVE_VA_VA_DRM_H */

static NTSTATUS va_not_supported(void *args) {
    static BOOL once;
    if (!once++) {
        WARN("dxva2 was built without VA-API support.\n");
    }
    return STATUS_NOT_SUPPORTED;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
    {
        va_not_supported,
        va_not_supported,
        va_not_supported,
        va_not_supported,
        va_not_supported,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_dxva2_funcs_count);

#endif /* HAVE_VA_VA_DRM_H */
