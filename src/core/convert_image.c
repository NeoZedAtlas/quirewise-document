/* Image re-encoding for the document conversion entry points.
 *
 * A decoded raster can be written back with the image package's encoders:
 * PNG, BMP, TIFF, JPEG and lossless WebP. PDF page rasterization stays in the
 * PDF route and is not affected.
 */

#include <quirewise/convert.h>
#include <quirewise/image.h>

#include "../backends/document_backend.h"

#include <stdio.h>
#include <string.h>

static int image_target(QwOutputFormat target, QwImageEncodeFormat *out)
{
    switch (target) {
    case QW_FORMAT_PNG: *out = QW_IMAGE_ENCODE_PNG; return 1;
    case QW_FORMAT_BMP: *out = QW_IMAGE_ENCODE_BMP; return 1;
    case QW_FORMAT_TIFF: *out = QW_IMAGE_ENCODE_TIFF; return 1;
    case QW_FORMAT_JPG: *out = QW_IMAGE_ENCODE_JPEG; return 1;
    case QW_FORMAT_WEBP: *out = QW_IMAGE_ENCODE_WEBP; return 1;
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

int qw_doc_convert_image_memory(const uint8_t *data, size_t size,
                                QwOutputFormat output_format,
                                uint8_t **out_data, size_t *out_size,
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
                 "raster re-encoded to %s; no resampling or color conversion",
                 qw_format_name(output_format));
    }
    return QW_STATUS_OK;
}
