/* Fixed-phase ILCE-7CM2 16-frame reconstruction.
 * Caller supplies validated, frame-ordered, little-endian U16 RAW data.
 */
#include <stddef.h>
#include <stdint.h>
#include "stream_pipeline.h"
#include "stream_slots.h"

#if defined(__aarch64__)
#include "gather_rgb.h"
#else
enum { RAW_WIDTH = 7040, RAW_HEIGHT = 4688, RGB_WIDTH = 14016 };
#endif

_Static_assert(sizeof(StreamSlots) == 12 && _Alignof(StreamSlots) == 4, "stream slot ABI");

STREAM_EXPORT void a7c2_slots_init(StreamSlots *slots) { slots_init(slots); }
STREAM_EXPORT void a7c2_slots_stop(StreamSlots *slots) { slots_stop(slots); }
STREAM_EXPORT int a7c2_slots_can_read(const StreamSlots *slots, uint32_t seq) { return slots_status(slots, seq, 0); }
STREAM_EXPORT int a7c2_slots_can_merge(const StreamSlots *slots, uint32_t seq) { return slots_status(slots, seq, 1); }
STREAM_EXPORT int a7c2_slots_publish(StreamSlots *slots, uint32_t seq) { return slots_publish(slots, seq); }
STREAM_EXPORT int a7c2_slots_release(StreamSlots *slots, uint32_t seq) { return slots_release(slots, seq); }

static const int shifts[FRAME_COUNT][2] = {
    { 1, 0}, { 1, 2}, {-1, 2}, {-1, 0}, {0, 0}, {0, 2}, {-2, 2}, {-2, 0},
    { 1, 1}, { 1, 3}, {-1, 3}, {-1, 1}, {0, 1}, {0, 3}, {-2, 3}, {-2, 1},
};

/* For each frame, the reader seeks once to payload_offset + first * row_bytes.
 * A slot has 64 rows per frame, including when its last stripe is partial.
 * Return the actual number of RAW rows per frame, or -1 for an invalid stripe.
 */
static int plan_stripe(int y, int batch, int first[FRAME_COUNT])
{
    if (y < 0 || y >= RGB_HEIGHT || y % batch != 0) {
        return -1;
    }

    int remaining = RGB_HEIGHT - y;
    int rows = (remaining < batch ? remaining : batch) / 2;
    for (int frame = 0; frame < FRAME_COUNT; ++frame) {
        first[frame] = (16 + y - shifts[frame][1] + 1) / 2;
        if (first[frame] < 0 || first[frame] + rows > RAW_HEIGHT) {
            return -1;
        }
    }
    return rows;
}

STREAM_EXPORT int a7c2_plan_stripe(int y, int first[FRAME_COUNT]) { return plan_stripe(y, STRIPE_ROWS, first); }

/* Return RGB row count, or -1 on failure. A failed stripe must not be consumed. */
static int read_frames(int y, int batch, int frame_start, int count, const FrameSource sources[FRAME_COUNT],
                       ReadRange read, void *context, uint16_t *slot)
{
    int first[FRAME_COUNT];
    int rows = plan_stripe(y, batch, first);
    if (rows < 0) {
        return -1;
    }

    const size_t row_bytes = RAW_WIDTH * sizeof(*slot);
    const size_t request = rows * row_bytes;
    for (int frame = frame_start; frame < frame_start + count; ++frame) {
        FrameSource source = sources[frame];
        uint64_t relative = (uint64_t)first[frame] * row_bytes;
        if (source.payload_offset > source.file_bytes ||
            relative > source.file_bytes - source.payload_offset ||
            request > source.file_bytes - source.payload_offset - relative) {
            return -1;
        }
        uint16_t *destination = slot + (size_t)(frame - frame_start) * (batch / 2) * RAW_WIDTH;
        if (read(context, frame, source.payload_offset + relative, destination, request)) {
            return -1;
        }
    }
    return rows * 2;
}

STREAM_EXPORT int a7c2_read_stripe(int y, const FrameSource sources[FRAME_COUNT],
                                  ReadRange read, void *context, uint16_t *slot)
{
    return read_frames(y, STRIPE_ROWS, 0, FRAME_COUNT, sources, read, context, slot);
}

STREAM_EXPORT int a7c2_read_group(int y, int batch, int group, const FrameSource sources[FRAME_COUNT],
                                 ReadRange read, void *context, uint16_t *slot)
{
    if ((batch != 256 && batch != 320 && batch != 512) || group < 0 || group >= 4) return -1;
    return read_frames(y, batch, group * 4, 4, sources, read, context, slot);
}

#if !defined(__aarch64__)
typedef struct {
    int frame;
    int row;
    int column;
} Sample;

/* Independent scalar reference for desktop validation. The ARM build uses the
 * established NEON kernel. Crop origin (12,8) is expressed in doubled pixels.
 */
static int scalar_map(Sample map[4][4][4])
{
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            unsigned occupied = 0;
            for (int frame = 0; frame < FRAME_COUNT; ++frame) {
                int raw_x = 24 + x - shifts[frame][0];
                int raw_y = 16 + y - shifts[frame][1];
                if ((raw_x | raw_y) & 1) {
                    continue;
                }
                raw_x /= 2;
                raw_y /= 2;
                int c = ((raw_x & 1) << 1) | (raw_y & 1);
                if (occupied & (1u << c)) {
                    return -1;
                }
                occupied |= 1u << c;
                map[y][x][c] = (Sample){frame, raw_y, raw_x};
            }
            if (occupied != 15) {
                return -1;
            }
        }
    }
    return 0;
}

static int scalar_row(const Sample phase[4][4], int y, const int *first,
                      const uint16_t *raw, uint16_t *rgb)
{
    const uint16_t *start[4][4];
    for (int x = 0; x < 4; ++x) {
        for (int c = 0; c < 4; ++c) {
            Sample sample = phase[x][c];
            int row = sample.row + 2 * (y / 4) - first[sample.frame];
            start[x][c] = raw + ((size_t)sample.frame * (STRIPE_ROWS / 2) + row)
                * RAW_WIDTH + sample.column;
        }
    }

    for (int x = 0; x < RGB_WIDTH; ++x) {
        unsigned channels[4];
        for (int c = 0; c < 4; ++c) {
            unsigned value = start[x % 4][c][2 * (x / 4)];
            if (value > 16383) {
                return -1;
            }
            channels[c] = value > 512 ? value - 512 : 0;
        }
        *rgb++ = (uint16_t)channels[0];
        *rgb++ = (uint16_t)((channels[1] + channels[2] + 1) / 2);
        *rgb++ = (uint16_t)channels[3];
    }
    return 0;
}
#endif

/* raw_slot is one complete 16-frame stripe. rgb holds one 16-row band.
 * Return 0 on success, or -1 on invalid geometry/samples. Discard the band on error.
 */
STREAM_EXPORT int a7c2_merge_band(int y, const uint16_t *raw_slot, uint16_t *rgb)
{
    if (y < 0 || y > RGB_HEIGHT - BAND_ROWS || y % BAND_ROWS != 0) {
        return -1;
    }
    int first[FRAME_COUNT];
    if (a7c2_plan_stripe(y - y % STRIPE_ROWS, first) < 0) {
        return -1;
    }

#if defined(__aarch64__)
    GatherMap map;
    if (make_gather_map(&map, shifts)) {
        return -1;
    }
#else
    Sample map[4][4][4];
    if (scalar_map(map)) {
        return -1;
    }
#endif

    for (int dy = 0; dy < BAND_ROWS; ++dy, rgb += RGB_WIDTH * 3) {
#if defined(__aarch64__)
        if (gather_rgb_row(&map, y + dy, STRIPE_ROWS / 2, first, raw_slot, rgb)) {
#else
        if (scalar_row(map[(y + dy) % 4], y + dy, first, raw_slot, rgb)) {
#endif
            return -1;
        }
    }
    return 0;
}

static int write_group_phase(const uint16_t *src[4], uint16_t *rgb)
{
#if defined(__aarch64__)
    uint16x4_t high = vdup_n_u16(0), black = vdup_n_u16(512);
    for (int x = 0; x < RGB_WIDTH / 4; x += 4, rgb += 48) {
        uint16x4_t r = vld2_u16(src[0] + 2*x).val[0];
        uint16x4_t g1 = vld2_u16(src[1] + 2*x).val[0];
        uint16x4_t g2 = vld2_u16(src[2] + 2*x).val[0];
        uint16x4_t b = vld2_u16(src[3] + 2*x).val[0];
        high = vmax_u16(high, vmax_u16(vmax_u16(r, g1), vmax_u16(g2, b)));
        uint16x4x3_t dest = {{vqsub_u16(r, black), vrhadd_u16(vqsub_u16(g1, black), vqsub_u16(g2, black)), vqsub_u16(b, black)}};
        vst3_lane_u16(rgb, dest, 0);
        vst3_lane_u16(rgb + 12, dest, 1);
        vst3_lane_u16(rgb + 24, dest, 2);
        vst3_lane_u16(rgb + 36, dest, 3);
    }
    return vmaxv_u16(high) <= 16383 ? 0 : -1;
#else
    for (int x = 0; x < RGB_WIDTH / 4; ++x, rgb += 12) {
        uint16_t values[4];
        for (int c = 0; c < 4; ++c) {
            uint16_t v = src[c][2*x];
            if (v > 16383) return -1;
            values[c] = v >= 512 ? v - 512 : 0;
        }
        rgb[0] = values[0];
        rgb[1] = (uint16_t)((values[1] + values[2] + 1) / 2);
        rgb[2] = values[3];
    }
    return 0;
#endif
}

STREAM_EXPORT int a7c2_merge_group(int y, int batch, int group, const uint16_t *slot, uint16_t *stripe)
{
    if ((batch != 256 && batch != 320 && batch != 512) || group < 0 || group >= 4 || !slot || !stripe) return -1;
    int first[FRAME_COUNT];
    int rows = plan_stripe(y, batch, first);
    if (rows < 0) return -1;
    const int h_phase = 1 - (group & 1);
    const int v_phase = group / 2;
#if defined(__aarch64__)
    GatherMap map;
    if (make_gather_map(&map, shifts)) return -1;
    const GatherSample (*phases)[4][4] = map.sample;
#else
    Sample map[4][4][4];
    if (scalar_map(map)) return -1;
    const Sample (*phases)[4][4] = map;
#endif

    for (int dy = v_phase; dy < 2*rows; dy += 2) {
        int out_y = y + dy;
        for (int p = h_phase; p < 4; p += 2) {
            const uint16_t *src[4];
            for (int c = 0; c < 4; ++c) {
                int frame = phases[out_y & 3][p][c].frame;
                int row = phases[out_y & 3][p][c].row + (out_y / 4) * 2 - first[frame];
                int slot_frame = frame - group*4;
                if (slot_frame < 0 || slot_frame >= 4 || row < 0 || row >= rows) return -1;
                src[c] = slot + ((size_t)slot_frame * (batch / 2) + row) * RAW_WIDTH + phases[out_y & 3][p][c].column;
            }
            if (write_group_phase(src, stripe + ((size_t)dy * RGB_WIDTH + p) * 3)) return -1;
        }
    }
    return 0;
}

/* Four independent frame views; all origins are absolute RAW row numbers. */
STREAM_EXPORT int a7c2_merge_view(int y,int rows,int group,const uint16_t *const input[4],const int origin[4],const int available[4],uint16_t *stripe,int begin,int end){
 if(y<0||y+rows>RGB_HEIGHT||y%4||rows%4||group<0||group>=4||begin<0||end>rows||begin>end)return -1;
 const int hp=1-(group&1),vp=group/2;
#if defined(__aarch64__)
 GatherMap map;if(make_gather_map(&map,shifts))return -1;const GatherSample (*phases)[4][4]=map.sample;
#else
 Sample map[4][4][4];if(scalar_map(map))return -1;const Sample (*phases)[4][4]=map;
#endif
 for(int dy=vp;dy<rows;dy+=2){if(dy<begin||dy>=end)continue;int out_y=y+dy;
  for(int p=hp;p<4;p+=2){const uint16_t *src[4];
   for(int c=0;c<4;c++){int f=phases[out_y&3][p][c].frame,local=f-group*4;if(local<0||local>=4)return -1;
    int row=phases[out_y&3][p][c].row+(out_y/4)*2-origin[local];if(!input[local]||row<0||row>=available[local])return -1;
    src[c]=input[local]+(size_t)row*RAW_WIDTH+phases[out_y&3][p][c].column;
   }if(write_group_phase(src,stripe+((size_t)dy*RGB_WIDTH+p)*3))return -1;
  }
 }return 0;
}
