#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>
#include <jerror.h>

#include "stream_pipeline.h"

enum { JPEG_ROWS = 16, JPEG_CHUNK = 64 * 1024 };

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf recovery;
} JpegError;

typedef struct {
    struct jpeg_destination_mgr base;
    WriteBytes write;
    void *context;
    JOCTET buffer[JPEG_CHUNK];
} JpegDestination;

typedef struct {
    struct jpeg_compress_struct compressor;
    JpegError error;
    JpegDestination destination;
} TurboJob;

static void jpeg_failed(j_common_ptr jpeg)
{
    JpegError *error = (JpegError *)jpeg->err;
    longjmp(error->recovery, 1);
}

static void destination_init(j_compress_ptr jpeg)
{
    JpegDestination *destination = (JpegDestination *)jpeg->dest;
    destination->base.next_output_byte = destination->buffer;
    destination->base.free_in_buffer = sizeof(destination->buffer);
}

static void destination_write(j_compress_ptr jpeg, size_t count)
{
    JpegDestination *destination = (JpegDestination *)jpeg->dest;
    if (count && destination->write(destination->context, destination->buffer, count)) {
        ERREXIT(jpeg, JERR_FILE_WRITE);
    }
}

static boolean destination_full(j_compress_ptr jpeg)
{
    destination_write(jpeg, JPEG_CHUNK);
    destination_init(jpeg);
    return TRUE;
}

static void destination_finish(j_compress_ptr jpeg)
{
    destination_write(jpeg, JPEG_CHUNK - jpeg->dest->free_in_buffer);
}

static int encode_turbo(GetRows rows, WriteBytes write, void *context, int width, int height)
{
    TurboJob *job = calloc(1, sizeof(*job));
    if (!job) {
        return -1;
    }

    j_compress_ptr jpeg = &job->compressor;
    jpeg->err = jpeg_std_error(&job->error.base);
    job->error.base.error_exit = jpeg_failed;
    if (setjmp(job->error.recovery)) {
        jpeg_destroy_compress(jpeg);
        free(job);
        return -1;
    }
    jpeg_create_compress(jpeg);
    job->destination.write = write;
    job->destination.context = context;
    job->destination.base.init_destination = destination_init;
    job->destination.base.empty_output_buffer = destination_full;
    job->destination.base.term_destination = destination_finish;
    jpeg->dest = &job->destination.base;

    jpeg->image_width = width;
    jpeg->image_height = height;
    jpeg->input_components = 3;
    jpeg->in_color_space = JCS_RGB;
    jpeg_set_defaults(jpeg);
    jpeg->comp_info[0].h_samp_factor = 2;
    jpeg->comp_info[0].v_samp_factor = 1;
    jpeg->comp_info[1].h_samp_factor = jpeg->comp_info[2].h_samp_factor = 1;
    jpeg->comp_info[1].v_samp_factor = jpeg->comp_info[2].v_samp_factor = 1;
    jpeg->dct_method = JDCT_ISLOW;
    /* The existing stb path uses the standard JPEG tables with Q90 scaling. */
    jpeg_set_quality(jpeg, 90, TRUE);
    jpeg_start_compress(jpeg, TRUE);

    const JDIMENSION submit_rows=width==OUTPUT_WIDTH?16:8;
    while (jpeg->next_scanline < jpeg->image_height) {
        const uint8_t *block = rows(context, (int)jpeg->next_scanline, submit_rows);
        if (!block) {
            jpeg_destroy_compress(jpeg);
            free(job);
            return -1;
        }
        JSAMPROW scanlines[JPEG_ROWS];
        for (JDIMENSION y = 0; y < submit_rows; ++y) {
            scanlines[y] = (JSAMPROW)(block + (size_t)y * width * 3);
        }
        if (jpeg_write_scanlines(jpeg, scanlines, submit_rows) != submit_rows) {
            ERREXIT(jpeg, JERR_CANT_SUSPEND);
        }
    }

    jpeg_finish_compress(jpeg);
    jpeg_destroy_compress(jpeg);
    free(job);
    return 0;
}

STREAM_EXPORT int a7c2_encode_jpeg_turbo(GetRows rows, WriteBytes write, void *context)
{
    return encode_turbo(rows, write, context, OUTPUT_WIDTH, RGB_HEIGHT);
}

STREAM_EXPORT int a7c2_encode_jpeg_turbo_sample(GetRows rows, WriteBytes write, void *context)
{
    return encode_turbo(rows, write, context, OUTPUT_WIDTH, STRIPE_ROWS);
}

STREAM_EXPORT int a7c2_encode_replay(GetRows rows,WriteBytes write,void *context){return encode_turbo(rows,write,context,1616,1080);}
