/* Image route applies crop/resize options before re-encoding. */

#include <quirewise/convert.h>
#include <quirewise/image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_IMAGE_EDIT_SRC
#error "QUIREWISE_IMAGE_EDIT_SRC must be defined"
#endif

static int read_file(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(file); return 0; }
    if (fread(data, 1u, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = data;
    *out_size = (size_t)size;
    return 1;
}

static int convert_with_options(const uint8_t *data, size_t size,
                                const QwConvertOptions *options,
                                uint32_t expected_width,
                                uint32_t expected_height)
{
    uint8_t *output = NULL;
    size_t output_size = 0u;
    QwImage decoded;
    QwConvertReport report;
    int status;
    memset(&report, 0, sizeof(report));
    status = qw_convert_to_format_memory(data, size, QW_FORMAT_PNG, options,
                                         &output, &output_size, &report);
    if (status != QW_STATUS_OK || !output || !output_size) {
        fprintf(stderr, "FAIL: conversion (%s)\n", report.note);
        return 0;
    }
    memset(&decoded, 0, sizeof(decoded));
    status = qw_image_decode(output, output_size, &decoded);
    free(output);
    if (status != QW_IMAGE_OK || decoded.width != expected_width ||
        decoded.height != expected_height) {
        fprintf(stderr, "FAIL: expected %ux%u, got %ux%u\n",
                expected_width, expected_height, decoded.width, decoded.height);
        qw_image_free(&decoded);
        return 0;
    }
    qw_image_free(&decoded);
    return 1;
}

int main(void)
{
    uint8_t *source = NULL;
    size_t source_size = 0u;
    QwConvertOptions options;

    if (!read_file(QUIREWISE_IMAGE_EDIT_SRC, &source, &source_size) ||
        !source || !source_size) {
        fputs("FAIL: cannot read the image fixture\n", stderr);
        return 1;
    }
    options = (QwConvertOptions)QW_CONVERT_OPTIONS_INIT;
    if (!convert_with_options(source, source_size, &options, 48u, 32u)) {
        free(source);
        return 1;
    }
    options = (QwConvertOptions)QW_CONVERT_OPTIONS_INIT;
    options.image_crop_left = 8u;
    options.image_crop_top = 8u;
    options.image_crop_right = 8u;
    options.image_crop_bottom = 8u;
    if (!convert_with_options(source, source_size, &options, 32u, 16u)) {
        free(source);
        return 1;
    }
    options = (QwConvertOptions)QW_CONVERT_OPTIONS_INIT;
    options.image_resize_width = 24u;
    options.image_resize_height = 12u;
    if (!convert_with_options(source, source_size, &options, 24u, 12u)) {
        free(source);
        return 1;
    }
    options = (QwConvertOptions)QW_CONVERT_OPTIONS_INIT;
    options.image_crop_left = 4u;
    options.image_crop_top = 4u;
    options.image_crop_right = 4u;
    options.image_crop_bottom = 4u;
    options.image_resize_width = 20u;
    options.image_resize_height = 8u;
    if (!convert_with_options(source, source_size, &options, 20u, 8u)) {
        free(source);
        return 1;
    }
    options = (QwConvertOptions)QW_CONVERT_OPTIONS_INIT;
    options.image_crop_left = 40u;
    options.image_crop_right = 40u;
    {
        uint8_t *output = NULL;
        size_t output_size = 0u;
        QwConvertReport report;
        memset(&report, 0, sizeof(report));
        if (qw_convert_to_format_memory(source, source_size, QW_FORMAT_PNG,
                                        &options, &output, &output_size,
                                        &report) !=
            QW_STATUS_ERR_INVALID_ARGUMENT) {
            fputs("FAIL: out-of-range crop was accepted\n", stderr);
            free(output);
            free(source);
            return 1;
        }
    }
    free(source);
    puts("Quirewise image crop/resize tests passed");
    return 0;
}
