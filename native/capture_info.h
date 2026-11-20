#ifndef PS_CAPTURE_INFO_H
#define PS_CAPTURE_INFO_H
#include "arw_u16.h"
typedef struct {
    uint32_t group,frame,total,orientation,iso;
    uint32_t shutter_num,shutter_den,aperture_num,aperture_den;
    char datetime[20];
} CaptureInfo;
int ps_capture_info(ArwU16 *source,CaptureInfo *info);
#endif
