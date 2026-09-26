#ifndef QPRO_AREA_RESIZE_H
#define QPRO_AREA_RESIZE_H

#include <stdint.h>

/* Downscale an 8-bit single-channel image like OpenCV's
   cv2.resize(src, (dst_w, dst_h), interpolation=cv2.INTER_AREA). Verified
   bit-identical to OpenCV 5.0.0 for 400x400 -> 224x224 (the tongue input);
   other scales, including integer ones, can differ by 1. Only
   downscaling (dst_w <= src_w, dst_h <= src_h) is supported; returns 0 on
   success and -1 on invalid arguments. src_stride is the byte distance
   between source rows, so a panel can be read straight out of a wider strip. */
int area_resize_u8(const uint8_t *src, int src_w, int src_h, int src_stride,
                   uint8_t *dst, int dst_w, int dst_h);

#endif
