#ifndef ARW_U16_H
#define ARW_U16_H
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
/* Narrow, read-only subset verified against the selected ILCE-7CM2 files.
 * Classic little-endian TIFF, one uncompressed U16 strip, 14/16-bit CFA.
 * No compressed ARW, MakerNote group parsing, color transform or camera I/O.
 */
typedef struct {
    FILE *file;
    uint64_t file_bytes,offset,payload_bytes,metadata_read_bytes;
    uint32_t width,height,bits,cfa[4],crop[4],black[4],white,wb_rggb[4];
    uint32_t max_read_request;
} ArwU16;
int arw_u16_open(ArwU16 *s,const char *utf8_path);
int arw_u16_row(ArwU16 *s,uint32_t y,uint16_t *out,size_t samples);
void arw_u16_close(ArwU16 *s);
#endif
