#ifndef A7C2_STREAM_PIPELINE_H
#define A7C2_STREAM_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define STREAM_EXPORT __declspec(dllexport)
#else
#define STREAM_EXPORT
#endif

enum {
    FRAME_COUNT = 16, OUTPUT_WIDTH = 14016, RGB_HEIGHT = 9344,
    STRIPE_ROWS = 128, BAND_ROWS = 16, RAW_ROW_SAMPLES = 7040
};

typedef struct {
    uint64_t payload_offset;
    uint64_t file_bytes;
} FrameSource;

typedef int (*ReadRange)(void *, int, uint64_t, void *, size_t);
typedef const uint8_t *(*GetRows)(void *, int, int);
typedef int (*WriteBytes)(void *, const void *, size_t);
typedef int (*ObserveBand)(void *, const uint16_t *, size_t);
typedef int (*ObservePreview)(void *, int, const uint8_t *);
typedef struct RenderState RenderState;
typedef int (*EncodeJpeg)(GetRows, WriteBytes, void *);

STREAM_EXPORT int a7c2_read_stripe(int y, const FrameSource sources[FRAME_COUNT],
                                  ReadRange read, void *context, uint16_t *slot);
STREAM_EXPORT int a7c2_merge_band(int y, const uint16_t *raw_slot, uint16_t *rgb);
STREAM_EXPORT int a7c2_read_group(int y, int batch, int group, const FrameSource sources[FRAME_COUNT],
                                 ReadRange read, void *context, uint16_t *slot);
STREAM_EXPORT int a7c2_merge_group(int y, int batch, int group, const uint16_t *slot, uint16_t *stripe);
STREAM_EXPORT size_t a7c2_renderer_bytes(void);
STREAM_EXPORT void a7c2_renderer_init(RenderState *state, const uint8_t curves[3][256]);
STREAM_EXPORT void a7c2_render_band(const RenderState *state, const uint16_t *data, uint8_t *out);
STREAM_EXPORT int a7c2_encode_jpeg(GetRows rows, WriteBytes write, void *context);
STREAM_EXPORT int a7c2_encode_jpeg_turbo(GetRows rows, WriteBytes write, void *context);

typedef enum { A7C2_JPEG_STB, A7C2_JPEG_TURBO } A7c2Jpeg;

/* Prevalidate and order all 16 RAW descriptors. The caller retains storage
 * for inputs and callbacks until return. read/write both run on the caller
 * thread. observe (optional) runs on the single encoder thread in parallel
 * mode and receives consecutive RGB16 bands, in bytes.
 *
 * If a callback fails, the pipeline stops and joins its worker before
 * returning -1. The caller owns the temporary destination and must discard
 * it on failure or validate it before publishing. I/O callbacks must return
 * on their own; the pipeline cannot cancel an in-progress blocking callback.
 */
STREAM_EXPORT int a7c2_run_pipeline(const FrameSource sources[FRAME_COUNT],
                                  const uint8_t curves[3][256], ReadRange read,
                                  WriteBytes write, ObserveBand observe, void *context,
                                  A7c2Jpeg codec, int parallel);

/* Four consecutive frames per input slot. Batch is 256 or 512 output rows. */
STREAM_EXPORT int a7c2_run_grouped(const FrameSource sources[FRAME_COUNT],
                                 const uint8_t curves[3][256], ReadRange read,
                                 WriteBytes write, ObserveBand observe, void *context,
                                 A7c2Jpeg codec, int parallel, int batch);

typedef struct {
    uint64_t raw_wait_ns, output_wait_ns, file_wait_ns, read_ns, write_ns;
    uint64_t group_merge_ns, linear_check_ns, render_ns, encoder_total_ns;
    uint32_t output_high_water, worker_joined;
    uint64_t file_thread_cpu_ns, compute_thread_cpu_ns, pipeline_shutdown_ns, fetch_total_ns, emit_packet_ns;
} A7c2PipelineStats;

STREAM_EXPORT int a7c2_run_grouped_profiled(const FrameSource sources[FRAME_COUNT],
                                          const uint8_t curves[3][256], ReadRange read,
                                          WriteBytes write, ObserveBand observe, ObservePreview preview,
                                          void *context, A7c2Jpeg codec, int parallel, int batch,
                                          A7c2PipelineStats *stats);

#endif
