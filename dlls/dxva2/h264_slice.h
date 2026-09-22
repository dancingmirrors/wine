#ifndef __WINE_DXVA2_H264_SLICE_H
#define __WINE_DXVA2_H264_SLICE_H

#include "dxva.h"

BOOL h264_parse_short_slice(const DXVA_PicParams_H264 *pic,
                            const void *bitstream, unsigned int bitstream_size,
                            const DXVA_Slice_H264_Short *in, DXVA_Slice_H264_Long *out);

#endif /* __WINE_DXVA2_H264_SLICE_H */
