#ifndef __WINE_DXVA2_UNIXLIB_H
#define __WINE_DXVA2_UNIXLIB_H

#include <stdarg.h>
#include <stddef.h>

#include "winbase.h"

#include "dxva.h"
#include "windef.h"

#include "wine/unixlib.h"

typedef UINT64 va_decoder_handle;

struct get_decoder_caps_params {
    UINT min_width;
    UINT min_height;
    UINT max_width;
    UINT max_height;
};

struct create_decoder_params {
    UINT width;
    UINT height;
    UINT surface_count;
    va_decoder_handle decoder;
};

struct destroy_decoder_params {
    va_decoder_handle decoder;
};

struct decode_h264_params {
    va_decoder_handle decoder;
    const DXVA_PicParams_H264 *pic_params;
    const DXVA_Qmatrix_H264 *qmatrix;
    const DXVA_Slice_H264_Long *slices;
    UINT slice_count;
    const void *bitstream;
    UINT bitstream_size;
};

struct copy_surface_params {
    va_decoder_handle decoder;
    UINT surface_index;
    void *dst;
    UINT dst_pitch;
    UINT width;
    UINT height;
};

enum dxva2_unix_call {
    unix_get_decoder_caps,
    unix_create_decoder,
    unix_decode_h264,
    unix_copy_surface,
    unix_destroy_decoder,
    unix_dxva2_funcs_count,
};

#endif /* __WINE_DXVA2_UNIXLIB_H */
