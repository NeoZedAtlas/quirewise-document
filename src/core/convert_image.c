/* Image re-encoding for the document conversion entry points.
 *
 * A decoded raster can be written back with the image package's encoders:
 * PNG, BMP, TIFF, JPEG, lossless WebP, TGA, QOI, PPM, single-frame GIF,\n * Radiance HDR, SGI RGB and PCX. PDF page rasterization stays in the
 * PDF route and is not affected.
 */

#include <quirewise/convert.h>
#include <quirewise/image.h>

#include "../backends/document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int image_target(QwOutputFormat target, QwImageEncodeFormat *out)
{
    switch (target) {
    case QW_FORMAT_PNG: *out = QW_IMAGE_ENCODE_PNG; return 1;
    case QW_FORMAT_BMP: *out = QW_IMAGE_ENCODE_BMP; return 1;
    case QW_FORMAT_TIFF: *out = QW_IMAGE_ENCODE_TIFF; return 1;
    case QW_FORMAT_JPG: *out = QW_IMAGE_ENCODE_JPEG; return 1;
    case QW_FORMAT_WEBP: *out = QW_IMAGE_ENCODE_WEBP; return 1;
    case QW_FORMAT_GIF: *out = QW_IMAGE_ENCODE_GIF; return 1;
    case QW_FORMAT_HDR: *out = QW_IMAGE_ENCODE_HDR; return 1;
    case QW_FORMAT_SGI: *out = QW_IMAGE_ENCODE_SGI; return 1;
    case QW_FORMAT_PCX: *out = QW_IMAGE_ENCODE_PCX; return 1;
    case QW_FORMAT_TGA: *out = QW_IMAGE_ENCODE_TGA; return 1;
    case QW_FORMAT_QOI: *out = QW_IMAGE_ENCODE_QOI; return 1;
    case QW_FORMAT_PPM: *out = QW_IMAGE_ENCODE_PPM; return 1;
    default: return 0;
    }
}

static int image_status(int status)
{
    switch (status) {
    case QW_IMAGE_OK: return QW_STATUS_OK;
    case QW_IMAGE_ERR_INVALID_ARGUMENT: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case QW_IMAGE_ERR_OUT_OF_MEMORY: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case QW_IMAGE_ERR_IO: return QW_STATUS_ERR_IO;
    case QW_IMAGE_ERR_FORMAT: return QW_STATUS_ERR_FORMAT;
    case QW_IMAGE_ERR_UNSUPPORTED: return QW_STATUS_ERR_UNSUPPORTED;
    case QW_IMAGE_ERR_DECODE: return QW_STATUS_ERR_DECODE;
    case QW_IMAGE_ERR_RESOURCE_LIMIT: return QW_STATUS_ERR_RESOURCE_LIMIT;
    default: return QW_STATUS_ERR_FORMAT;
    }
}


static unsigned image_channels(QwImagePixelFormat format)
{
    return format == QW_IMAGE_PIXEL_GRAY8 ? 1u : 3u;
}

static int image_apply_crop(QwImage *image, const QwConvertOptions *options)
{
    uint32_t left = options->image_crop_left;
    uint32_t top = options->image_crop_top;
    uint32_t right = options->image_crop_right;
    uint32_t bottom = options->image_crop_bottom;
    uint32_t width;
    uint32_t height;
    unsigned channels;
    uint8_t *pixels;
    uint32_t row;
    if (!left && !top && !right && !bottom) return 1;
    if ((uint64_t)left + right >= image->width ||
        (uint64_t)top + bottom >= image->height)
        return 0;
    width = image->width - left - right;
    height = image->height - top - bottom;
    channels = image_channels(image->format);
    pixels = (uint8_t *)malloc((size_t)width * channels * height);
    if (!pixels) return 0;
    for (row = 0u; row < height; ++row) {
        memcpy(pixels + (size_t)row * width * channels,
               image->pixels + (size_t)(row + top) * image->stride +
                   (size_t)left * channels,
               (size_t)width * channels);
    }
    free(image->pixels);
    image->pixels = pixels;
    image->width = width;
    image->height = height;
    image->stride = (size_t)width * channels;
    image->size = image->stride * height;
    return 1;
}

static int image_apply_resize(QwImage *image, const QwConvertOptions *options)
{
    uint32_t width = options->image_resize_width;
    uint32_t height = options->image_resize_height;
    unsigned channels;
    uint8_t *pixels;
    uint32_t row;
    if (!width && !height) return 1;
    if (!width)
        width = (uint32_t)(((uint64_t)height * image->width / image->height) ?
                           ((uint64_t)height * image->width / image->height) : 1u);
    else if (!height)
        height = (uint32_t)(((uint64_t)width * image->height / image->width) ?
                            ((uint64_t)width * image->height / image->width) : 1u);
    if (!width || !height || width > 8192u || height > 8192u) return 0;
    if (width == image->width && height == image->height) return 1;
    channels = image_channels(image->format);
    pixels = (uint8_t *)malloc((size_t)width * channels * height);
    if (!pixels) return 0;
    for (row = 0u; row < height; ++row) {
        uint32_t source_row =
            (uint32_t)((uint64_t)row * image->height / height);
        uint32_t column;
        for (column = 0u; column < width; ++column) {
            uint32_t source_column =
                (uint32_t)((uint64_t)column * image->width / width);
            memcpy(pixels + ((size_t)row * width + column) * channels,
                   image->pixels + (size_t)source_row * image->stride +
                       (size_t)source_column * channels,
                   channels);
        }
    }
    free(image->pixels);
    image->pixels = pixels;
    image->width = width;
    image->height = height;
    image->stride = (size_t)width * channels;
    image->size = image->stride * height;
    return 1;
}

int qw_doc_convert_image_memory_with_options(
    const uint8_t *data, size_t size, QwOutputFormat output_format,
    const QwConvertOptions *options, uint8_t **out_data, size_t *out_size,
    QwConvertReport *report)
{
    QwImageEncodeFormat encode_format;
    QwImage image;
    int status;
    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (!data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!image_target(output_format, &encode_format)) {
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "no image output target for %s",
                     qw_format_name(output_format));
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    memset(&image, 0, sizeof(image));
    status = qw_image_decode(data, size, &image);
    if (status != QW_IMAGE_OK) {
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "image decode failed (%d)", status);
        return image_status(status);
    }
    if (options &&
        (!image_apply_crop(&image, options) ||
         !image_apply_resize(&image, options))) {
        qw_image_free(&image);
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "image crop/resize request is outside the decoded raster");
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    status = qw_image_encode(&image, encode_format, out_data, out_size);
    qw_image_free(&image);
    if (status != QW_IMAGE_OK) {
        if (report) {
            if (status == QW_IMAGE_ERR_UNSUPPORTED)
                snprintf(report->note, sizeof(report->note),
                         "the image package has no encoder for %s yet",
                         qw_format_name(output_format));
            else
                snprintf(report->note, sizeof(report->note),
                         "image encode failed (%d)", status);
        }
        return image_status(status);
    }
    if (report) {
        report->format = qw_detect_format(data, size);
        report->output_format = output_format;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "raster re-encoded to %s%s", qw_format_name(output_format),
                 options && (options->image_crop_left || options->image_crop_top ||
                             options->image_crop_right ||
                             options->image_crop_bottom ||
                             options->image_resize_width ||
                             options->image_resize_height)
                     ? " with the requested crop/resize"
                     : "; no resampling or color conversion");
    }
    return QW_STATUS_OK;
}

int qw_doc_convert_image_memory(const uint8_t *data, size_t size,
                                QwOutputFormat output_format,
                                uint8_t **out_data, size_t *out_size,
                                QwConvertReport *report)
{
    return qw_doc_convert_image_memory_with_options(
        data, size, output_format, NULL, out_data, out_size, report);
}
