// SPDX-License-Identifier: MIT
#include "annexb_split.h"
#include "v4l2_decoder.h"
#include "v4l2_probe.h"

#include <errno.h>
#include <limits.h>
#include <time.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* H.264 and HEVC reach the decoder as Annex-B byte streams; VP9 has no start
 * codes at all, so its probe takes an IVF container instead and queues exactly
 * one IVF frame per OUTPUT buffer. */
struct input_codec {
    const char *name;
    const char *display_name;
    uint32_t coded_format;
    bool ivf;
    enum venus_annexb_codec annexb;
};

static const struct input_codec input_codecs[] = {
    { "h264", "H.264", V4L2_PIX_FMT_H264, false, VENUS_ANNEXB_H264 },
    { "hevc", "HEVC", V4L2_PIX_FMT_HEVC, false, VENUS_ANNEXB_HEVC },
    /* The Annex-B splitter never sees a VP9 stream; the field is filler. */
    { "vp9", "VP9", V4L2_PIX_FMT_VP9, true, VENUS_ANNEXB_H264 },
};

struct access_unit_stats {
    size_t count;
    size_t maximum;
};

struct decode_run {
    struct venus_v4l2_decoder *decoder;
    FILE *output;
    size_t frames;
    size_t bytes;
    uint64_t next_tag;
    int error;
};

static int inspect_access_unit(const uint8_t *data, size_t size,
                               void *opaque)
{
    struct access_unit_stats *stats = opaque;

    (void)data;
    stats->count++;
    if (size > stats->maximum)
        stats->maximum = size;
    return 0;
}

static int write_frame(const struct venus_v4l2_frame *frame, void *opaque)
{
    struct decode_run *run = opaque;

    if (fwrite(frame->data, 1, frame->size, run->output) != frame->size) {
        run->error = -EIO;
        return run->error;
    }

    run->frames++;
    run->bytes += frame->size;
    return 0;
}

static int submit_access_unit(const uint8_t *data, size_t size,
                              void *opaque)
{
    struct decode_run *run = opaque;
    int status;

    status = venus_v4l2_decoder_submit(
        run->decoder, data, size, run->next_tag++,
        write_frame, run);
    if (status < 0)
        run->error = status;

    return status;
}

/* IVF: a 32-byte "DKIF" header, then per frame a 4-byte little-endian size and
 * an 8-byte timestamp ahead of the compressed frame. */
static int for_each_ivf_frame(const uint8_t *data, size_t size,
                              venus_access_unit_callback callback,
                              void *opaque, size_t *num_units)
{
    size_t offset = 32;
    size_t units = 0;

    if (size < offset || memcmp(data, "DKIF", 4) != 0)
        return -EINVAL;

    while (offset + 12 <= size) {
        uint32_t frame_size = data[offset] | (data[offset + 1] << 8) |
                              (data[offset + 2] << 16) |
                              ((uint32_t)data[offset + 3] << 24);
        int result;

        offset += 12;
        if (frame_size == 0 || frame_size > size - offset)
            return -EINVAL;

        result = callback(data + offset, frame_size, opaque);
        if (result < 0)
            return result;

        offset += frame_size;
        units++;
    }

    if (units == 0 || offset != size)
        return -EINVAL;

    if (num_units)
        *num_units = units;
    return 0;
}

static int for_each_input_unit(const struct input_codec *codec,
                               const uint8_t *data, size_t size,
                               venus_access_unit_callback callback,
                               void *opaque, size_t *num_units)
{
    if (codec->ivf)
        return for_each_ivf_frame(data, size, callback, opaque, num_units);

    return venus_annexb_for_each_access_unit(
        data, size, codec->annexb, callback, opaque, num_units);
}

static const struct input_codec *lookup_codec(const char *name)
{
    size_t index;

    for (index = 0; index < sizeof(input_codecs) / sizeof(input_codecs[0]);
         index++) {
        if (strcmp(name, input_codecs[index].name) == 0)
            return &input_codecs[index];
    }

    return NULL;
}

static int parse_positive(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !end || *end != '\0' || parsed == 0 ||
        parsed > UINT_MAX)
        return -EINVAL;

    *value = (unsigned int)parsed;
    return 0;
}

static int read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file;
    long length;
    uint8_t *contents;

    file = fopen(path, "rb");
    if (!file)
        return -errno;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -EIO;
    }

    length = ftell(file);
    if (length <= 0 || length > 64 * 1024 * 1024) {
        fclose(file);
        return -EFBIG;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -EIO;
    }

    contents = malloc((size_t)length);
    if (!contents) {
        fclose(file);
        return -ENOMEM;
    }

    if (fread(contents, 1, (size_t)length, file) != (size_t)length) {
        free(contents);
        fclose(file);
        return -EIO;
    }

    fclose(file);
    *data = contents;
    *size = (size_t)length;
    return 0;
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        return -1;

    return (int64_t)timestamp.tv_sec * 1000 +
           timestamp.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    struct venus_capabilities capabilities;
    struct venus_v4l2_decoder_config config;
    struct venus_v4l2_decoder *decoder = NULL;
    struct venus_v4l2_error open_error = { 0 };
    struct access_unit_stats stats = { 0 };
    struct decode_run run = { 0 };
    const struct input_codec *codec = &input_codecs[0];
    const char *device;
    const char *input_path;
    const char *output_path;
    uint8_t *input = NULL;
    size_t input_size = 0;
    size_t units = 0;
    unsigned int width;
    unsigned int height;
    unsigned int expected_frames;
    bool eos = false;
    int64_t deadline;
    int status;
    int first = 1;

    if (argc > 1 && strncmp(argv[1], "--codec=", 8) == 0) {
        codec = lookup_codec(argv[1] + 8);
        if (!codec) {
            fprintf(stderr,
                    "--codec must be one of h264, hevc, vp9\n");
            return 2;
        }
        first = 2;
    }

    if (argc - first != 5 && argc - first != 6) {
        fprintf(stderr,
                "usage: %s [--codec=h264|hevc|vp9] WIDTH HEIGHT FRAMES "
                "INPUT OUTPUT [DEVICE]\n",
                argv[0]);
        return 2;
    }

    if (parse_positive(argv[first], &width) < 0 ||
        parse_positive(argv[first + 1], &height) < 0 ||
        parse_positive(argv[first + 2], &expected_frames) < 0) {
        fprintf(stderr, "width, height and frames must be positive\n");
        return 2;
    }

    input_path = argv[first + 3];
    output_path = argv[first + 4];

    if (argc - first == 6) {
        device = argv[first + 5];
    } else {
        status = venus_v4l2_probe(&capabilities);
        if (status < 0 || capabilities.decoder_path[0] == '\0') {
            fprintf(stderr, "qcom-venus decoder was not found\n");
            return 1;
        }
        device = capabilities.decoder_path;
    }

    status = read_file(input_path, &input, &input_size);
    if (status < 0) {
        fprintf(stderr, "read %s: %s\n", input_path,
                strerror(-status));
        return 1;
    }

    status = for_each_input_unit(
        codec, input, input_size, inspect_access_unit, &stats, &units);
    if (status < 0) {
        fprintf(stderr, "split %s input: %s\n", codec->name,
                strerror(-status));
        free(input);
        return 1;
    }

    config = (struct venus_v4l2_decoder_config) {
        .device = device,
        .coded_format = codec->coded_format,
        .width = width,
        .height = height,
        .output_buffer_size =
            stats.maximum < 1024 * 1024 ? 1024 * 1024 : stats.maximum,
        .output_buffers = 4,
        .capture_buffers = 16,
    };

    status = venus_v4l2_decoder_open(
        &config, &decoder, &open_error);
    if (status < 0) {
        fprintf(stderr, "open decoder: operation=%s error=%s (%d)\n",
                open_error.operation[0] ? open_error.operation : "none",
                strerror(-status), -status);
        free(input);
        return 1;
    }

    run.decoder = decoder;
    run.output = fopen(output_path, "wb");
    if (!run.output) {
        fprintf(stderr, "open %s: %s\n", output_path, strerror(errno));
        venus_v4l2_decoder_close(decoder);
        free(input);
        return 1;
    }

    printf("device=%s\n", device);
    printf("codec=%s\n", codec->name);
    printf("input_bytes=%zu\n", input_size);
    printf("access_units=%zu\n", units);
    printf("output_buffers=%u\n",
           venus_v4l2_decoder_output_count(decoder));

    status = for_each_input_unit(
        codec, input, input_size, submit_access_unit, &run, NULL);
    if (status < 0)
        goto finish;

    printf("capture_buffers=%u\n",
           venus_v4l2_decoder_capture_count(decoder));
    printf("capture_format=%ux%u size=%u\n",
           venus_v4l2_decoder_capture_width(decoder),
           venus_v4l2_decoder_capture_height(decoder),
           venus_v4l2_decoder_capture_size(decoder));

    status = venus_v4l2_decoder_stop(decoder);
    if (status < 0)
        goto finish;

    deadline = monotonic_milliseconds() + 30000;
    while (run.frames < expected_frames && !eos &&
           monotonic_milliseconds() < deadline) {
        status = venus_v4l2_decoder_pump(
            decoder, 1000, write_frame, &run, &eos);
        if (status == -ETIMEDOUT || status == -EAGAIN)
            continue;
        if (status < 0)
            goto finish;
    }

    if (run.frames != expected_frames)
        status = -ETIMEDOUT;

finish:
    if (fclose(run.output) != 0 && status == 0)
        status = -EIO;

    printf("decoded_frames=%zu\n", run.frames);
    printf("decoded_bytes=%zu\n", run.bytes);
    printf("source_changes=%u\n",
           venus_v4l2_decoder_source_changes(decoder));
    printf("eos=%s\n", eos ? "yes" : "no");

    if (status < 0) {
        const char *operation =
            venus_v4l2_decoder_last_operation(decoder);

        if (status == -ETIMEDOUT && strcmp(operation, "none") == 0)
            operation = "wait(decoded frames)";
        fprintf(stderr, "FAIL operation=%s error=%s (%d)\n",
                operation, strerror(-status), -status);
    } else {
        printf("PASS: V4L2 stateful %s decode completed\n",
               codec->display_name);
    }

    venus_v4l2_decoder_close(decoder);
    free(input);
    return status < 0 ? 1 : 0;
}
