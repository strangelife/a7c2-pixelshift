/* Shared renderer and reference 4:2:2 JPEG encoder for bounded RGB strips. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "stream_pipeline.h"
#include "srgb16.h"
#if defined(__aarch64__)
#include "render_neon.h"
#endif

struct RenderState {
    float normalization[3][15872];
    uint8_t tone[3][4096];
};

STREAM_EXPORT size_t a7c2_renderer_bytes(void) { return sizeof(RenderState); }

STREAM_EXPORT void a7c2_renderer_init(RenderState *state, const uint8_t curves[3][256]) {
    static const float wb[] = {2552.0f / 1024, 1.0f, 2053.0f / 1024};
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < 15872; ++i) state->normalization[c][i] = (float)i * wb[c] / 15871.0f;
        for (int i = 0; i < 4096; ++i) {
            unsigned a = srgb16[i] / 257, frac = srgb16[i] % 257;
            unsigned b = a < 255 ? a + 1 : a;
            state->tone[c][i] = (uint8_t)((curves[c][a] * (257-frac) + curves[c][b] * frac + 128) / 257);
        }
    }
}

STREAM_EXPORT void a7c2_render_band(const RenderState *state, const uint16_t *data, uint8_t *out) {
    static const float matrix[] = {1.63606179f, -.44861874f, -.18744300f, -.11516476f, 1.48897076f,
                                   -.37380603f, -.00474023f, -.32360125f, 1.32834148f};
    enum { pixels_per_band = 14016 * 16 };
#if defined(__aarch64__)
    render_rgb_neon(data, out, pixels_per_band, state->normalization, matrix, state->tone);
#else
    for (int n = 0; n < pixels_per_band; ++n, data += 3, out += 3) {
        float r = state->normalization[0][data[0]], g = state->normalization[1][data[1]], b = state->normalization[2][data[2]];
        for (int c = 0; c < 3; ++c) {
            const float *m = matrix + c * 3;
            float value = fmaf(b, m[2], fmaf(r, m[0], g * m[1]));
            unsigned index = value <= 0 ? 0 : value >= 1 ? 4095 : (unsigned)fmaf(value, 4095.0f, 0.5f);
            out[c] = state->tone[c][index];
        }
    }
#endif
}

typedef struct { GetRows rows; WriteBytes write; void *context; int failed; } JpegSink;

static const uint8_t *next_rows(JpegSink *sink, int y, int count) {
    return sink->failed ? NULL : sink->rows(sink->context, y, count);
}

static void output_bytes(void *context, void *data, int size) {
    JpegSink *sink = context;
    if (!sink->failed && (size < 0 || sink->write(sink->context, data, (size_t)size))) sink->failed = 1;
}

#define PS_BATCH_ENTROPY 1
#define PS_ENTROPY_BUFFER 4096
#define PS_FAST_JPEG_BITS 1
#define PS_JPEG_ROW_SOURCE(y, n) next_rows(s->context, y, n)
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_422_batch.h"

static int encode_jpeg(GetRows rows, WriteBytes write, void *context, int height) {
    static const unsigned char placeholder[3] = {0};
    JpegSink sink = {rows, write, context, 0};
    int ok = stbi_write_jpg_to_func(output_bytes, &sink, 14016, height, 3, placeholder, 90);
    return ok && !sink.failed ? 0 : -1;
}

STREAM_EXPORT int a7c2_encode_jpeg(GetRows rows, WriteBytes write, void *context) {
    return encode_jpeg(rows, write, context, 9344);
}

STREAM_EXPORT int a7c2_encode_jpeg_sample(GetRows rows, WriteBytes write, void *context) {
    return encode_jpeg(rows, write, context, 128);
}
