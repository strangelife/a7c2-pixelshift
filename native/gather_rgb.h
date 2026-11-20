#ifndef A7C2_GATHER_RGB_H
#define A7C2_GATHER_RGB_H

/* Assemble four horizontally-spaced pixels at a time from the exact 4x4
 * pixel-shift lattice. The caller supplies complete RAW rows. */
#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>

enum { RAW_WIDTH = 7040, RAW_HEIGHT = 4688, RGB_WIDTH = 14016 };

typedef struct {
    uint8_t frame, row, column;
} GatherSample;

typedef struct {
    GatherSample sample[4][4][4];
} GatherMap;

static int make_gather_map(GatherMap *map, const int shifts[16][2]) {
    uint8_t seen[4][4][4] = {{{0}}};
    for (int phase_y = 0; phase_y < 4; phase_y++) {
        for (int phase_x = 0; phase_x < 4; phase_x++) {
            for (int frame = 0; frame < 16; frame++) {
                int y = 16 + phase_y - shifts[frame][1];
                int x = 24 + phase_x - shifts[frame][0];
                if ((y | x) & 1) continue;
                y /= 2;
                x /= 2;
                if (x < 0 || x + RGB_WIDTH / 2 > RAW_WIDTH || y < 0 || y > 10) return -1;
                int component = y & 1 ? (x & 1 ? 3 : 1) : (x & 1 ? 2 : 0);
                if (seen[phase_y][phase_x][component]++) return -1;
                map->sample[phase_y][phase_x][component] =
                    (GatherSample){(uint8_t)frame, (uint8_t)y, (uint8_t)x};
            }
            for (int c = 0; c < 4; c++) if (seen[phase_y][phase_x][c] != 1) return -1;
        }
    }
    return 0;
}

/* Each phase contributes output x values p, p+4, p+8, p+12. Transpose
 * four phases to four contiguous groups before storing packed RGB. */
static inline void gather_zip(uint16x4_t a, uint16x4_t b, uint16x4_t c, uint16x4_t d,
                              uint16x8_t *lo, uint16x8_t *hi) {
    uint16x4x2_t ab = vzip_u16(a, b), cd = vzip_u16(c, d);
    uint32x2x2_t first = vzip_u32(vreinterpret_u32_u16(ab.val[0]), vreinterpret_u32_u16(cd.val[0]));
    uint32x2x2_t second = vzip_u32(vreinterpret_u32_u16(ab.val[1]), vreinterpret_u32_u16(cd.val[1]));
    *lo = vcombine_u16(vreinterpret_u16_u32(first.val[0]), vreinterpret_u16_u32(first.val[1]));
    *hi = vcombine_u16(vreinterpret_u16_u32(second.val[0]), vreinterpret_u16_u32(second.val[1]));
}

static int gather_rgb_row(const GatherMap *map, int output_y, int cache_rows,
                          const int cache_first[16], const uint16_t *cache, uint16_t *dst) {
    if (output_y < 0 || output_y >= 9344 || cache_rows <= 0) return -1;
    const uint16_t *rows[4][4];
    const GatherSample (*phase)[4] = map->sample[output_y & 3];
    for (int p = 0; p < 4; p++) {
        for (int c = 0; c < 4; c++) {
            GatherSample s = phase[p][c];
            int y = s.row + (output_y / 4) * 2;
            int local_y = y - cache_first[s.frame];
            if (y >= RAW_HEIGHT || local_y < 0 || local_y >= cache_rows) return -1;
            rows[p][c] = cache + ((size_t)s.frame * cache_rows + local_y) * RAW_WIDTH + s.column;
        }
    }
    uint16x4_t maximum = vdup_n_u16(0), black = vdup_n_u16(512);
    for (int x = 0; x < RGB_WIDTH; x += 16, dst += 48) {
        uint16x4_t r[4], g[4], b[4];
        for (int p = 0; p < 4; p++) {
            /* LD2 supplies the even lanes we need. The final discarded lane
             * stays within the padded RAW row. */
            uint16x4_t vr = vld2_u16(rows[p][0] + x / 2).val[0];
            uint16x4_t vg1 = vld2_u16(rows[p][1] + x / 2).val[0];
            uint16x4_t vg2 = vld2_u16(rows[p][2] + x / 2).val[0];
            uint16x4_t vb = vld2_u16(rows[p][3] + x / 2).val[0];
            maximum = vmax_u16(maximum, vmax_u16(vmax_u16(vr, vg1), vmax_u16(vg2, vb)));
            r[p] = vqsub_u16(vr, black);
            g[p] = vrhadd_u16(vqsub_u16(vg1, black), vqsub_u16(vg2, black));
            b[p] = vqsub_u16(vb, black);
        }
        uint16x8x3_t lo, hi;
        gather_zip(r[0], r[1], r[2], r[3], &lo.val[0], &hi.val[0]);
        gather_zip(g[0], g[1], g[2], g[3], &lo.val[1], &hi.val[1]);
        gather_zip(b[0], b[1], b[2], b[3], &lo.val[2], &hi.val[2]);
        vst3q_u16(dst, lo);
        vst3q_u16(dst + 24, hi);
    }
    return vmaxv_u16(maximum) > 16383 ? -1 : 0;
}

#endif
