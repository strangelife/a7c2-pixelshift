/* Match the scalar target: round the green product first, then fuse red
 * and blue in that order. Keep fused quantization in all four lanes. */
#include <arm_neon.h>
#include <stdint.h>

static inline float32x4_t render_norm(uint16x4_t values, const float table[15872]) {
    float32x4_t out = vdupq_n_f32(table[vget_lane_u16(values, 0)]);
    out = vsetq_lane_f32(table[vget_lane_u16(values, 1)], out, 1);
    out = vsetq_lane_f32(table[vget_lane_u16(values, 2)], out, 2);
    return vsetq_lane_f32(table[vget_lane_u16(values, 3)], out, 3);
}

static void render_rgb_neon(const uint16_t *pixels, uint8_t *out, uint32_t count,
                            const float norm[3][15872], const float matrix[9], const uint8_t tone[3][4096]) {
    const float32x4_t zero = vdupq_n_f32(0.0f), one = vdupq_n_f32(1.0f), half = vdupq_n_f32(0.5f);
    for (uint32_t i = 0; i < count; i += 4, pixels += 12, out += 12) {
        uint16x4x3_t src = vld3_u16(pixels);
        float32x4_t r = render_norm(src.val[0], norm[0]);
        float32x4_t g = render_norm(src.val[1], norm[1]);
        float32x4_t b = render_norm(src.val[2], norm[2]);
        for (uint32_t c = 0; c < 3; c++) {
            float32x4_t value = vmulq_n_f32(g, matrix[c*3+1]);
            value = vfmaq_n_f32(value, r, matrix[c*3]);
            value = vfmaq_n_f32(value, b, matrix[c*3+2]);
            value = vminq_f32(vmaxq_f32(value, zero), one);
            uint32x4_t index = vcvtq_u32_f32(vfmaq_n_f32(half, value, 4095.0f));
            out[c] = tone[c][vgetq_lane_u32(index, 0)];
            out[c+3] = tone[c][vgetq_lane_u32(index, 1)];
            out[c+6] = tone[c][vgetq_lane_u32(index, 2)];
            out[c+9] = tone[c][vgetq_lane_u32(index, 3)];
        }
    }
}
