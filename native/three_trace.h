#ifndef THREE_TRACE_H
#define THREE_TRACE_H
#include <stdint.h>
enum { T_OPEN,T_SEEK,T_READ,T_CRC,T_CLOSE,T_MEM,T_READ_CALLS,
 T_RGB_FULL_WAIT,T_JPEG_RGB_WAIT,T_RECON_CPU,T_JPEG_CPU,T_RECON_WALL,T_JPEG_WALL,
 T_RGB_PEAK,T_RECON_JOINED,T_JPEG_JOINED,T_PREVIEW_SAMPLE,T_PREVIEW_ENCODE,
 T_PREVIEW_WRITE,T_PREVIEW_CRC,T_PREVIEW_BYTES,T_PACKETS,T_RGB_PRODUCED,T_RGB_CONSUMED,
 T_PRIMARY_BYTES,T_FILE_CPU,T_RAW_EMPTY_COUNT,T_RGB_EMPTY_COUNT,T_RGB_FULL_COUNT,
 T_RAW_FULL_COUNT,T_RESERVED0,T_RESERVED1,T_COUNT };
typedef struct {
 uint64_t read_begin[4],read_end[4],merge_begin[4],merge_end[4];
 uint64_t rgb_first,rgb_last,jpeg_first,jpeg_last,write_first,write_last;
} StripeTrace;
typedef struct { uint64_t render_begin,render_end,published,consume_begin,consume_end; } BandTrace;
typedef struct { uint64_t queued,write_begin,write_end,row; } PacketTrace;
enum { X_IO_CALLS,X_SHORT_READS,X_BIG_REQUESTS,X_BIG_FULL_RETURNS,X_MAX_REQUEST,X_LOGGED,X_DROPPED,X_POOL_OPENED,X_POOL_CLOSED,X_POOL_PEAK,X_POOL_FALLBACK,X_POOL_ENABLED,X_CLOSE_ERRORS,X_POOL_MEM,X_CRC_TID,X_POOL_OPEN_NS,X_POOL_CLOSE_NS,X_COUNT };
typedef struct {uint32_t requested;int32_t returned;uint32_t window;uint16_t frame,flags;uint64_t duration;} ReadCall;
typedef struct {
 char magic[8];uint32_t version,bytes;uint64_t epoch;
 uint64_t counters[T_COUNT];StripeTrace stripes[146];BandTrace bands[584];PacketTrace packets[512];uint64_t extra[X_COUNT];ReadCall reads[16384];uint64_t compute_copy_ns,compute_copy_bytes;
} ThreeTrace;
extern ThreeTrace three_trace;
#endif
