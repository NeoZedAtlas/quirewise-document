#ifndef QUIREWISE_CONVERT_H
#define QUIREWISE_CONVERT_H

#include "quirewise/format.h"
#include "quirewise/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! A top-down 8-bit raster page: channels is 1 (gray) or 3 (RGB). */
typedef struct QwRasterPage {
    const uint8_t *pixels;
    size_t stride;
    uint32_t width;
    uint32_t height;
    int channels;
} QwRasterPage;

/*! Page box selection for raster PDF output. */
#define QW_RASTER_PAGE_MODE_A4 0
#define QW_RASTER_PAGE_MODE_IMAGE 1

/*!
 * Build a PDF with one image page per raster entry. Images are embedded as
 * raw RGB/gray XObjects and Flate-compressed when that is smaller. On success
 * *out_pdf is malloc-owned and released with qw_buffer_free(); the page box is
 * A4 and each image is fitted into it.
 */
QW_API int qw_raster_pages_to_pdf(const QwRasterPage *pages,
                                  size_t count,
                                  uint8_t **out_pdf,
                                  size_t *out_size,
                                  QwConvertReport *report);

/*!
 * Same as qw_raster_pages_to_pdf() with an explicit page mode: A4 fits every
 * image into an A4 box, IMAGE uses the pixel dimensions as the page box.
 */
QW_API int qw_raster_pages_to_pdf_mode(const QwRasterPage *pages,
                                       size_t count,
                                       int page_mode,
                                       uint8_t **out_pdf,
                                       size_t *out_size,
                                       QwConvertReport *report);

/*!
 * Detect a known input format from its contents, including container
 * structure. Returns QW_INPUT_UNKNOWN when nothing matches; never fails.
 */
QW_API QwInputFormat qw_detect_format(const void *data, size_t size);

/*!
 * Return the stable lowercase name of a detected format, or "unknown" for
 * QW_INPUT_UNKNOWN. The returned string is static and must not be freed.
 */
QW_API const char *qw_format_name(QwInputFormat format);

/*!
 * Convert a memory buffer to a registered output format. On success, *out_data
 * is malloc-owned by the caller and must be released with qw_buffer_free().
 * The target must be reachable from the input family; otherwise the call
 * returns QW_STATUS_ERR_UNSUPPORTED and report->note names the reason.
 */
QW_API int qw_convert_to_format_memory(const void *data,
                                        size_t size,
                                        QwOutputFormat output_format,
                                        const QwConvertOptions *options,
                                        uint8_t **out_data,
                                        size_t *out_size,
                                        QwConvertReport *report);

/*!
 * Release a buffer returned by this header (output data, PDF bytes or text).
 * NULL is accepted. The same allocation is also released by qw_pdf_free().
 */
QW_API void qw_buffer_free(void *buffer);

/*!
 * Convert a memory buffer to PDF. On success, *out_pdf is malloc-owned by the
 * caller and must be released with qw_buffer_free(). Content streams and raw
 * image streams are Flate-compressed when that is smaller.
 */
QW_API int qw_convert_to_pdf_memory(const void *data,
                                     size_t size,
                                     const QwConvertOptions *options,
                                     uint8_t **out_pdf,
                                     size_t *out_size,
                                     QwConvertReport *report);

/*!
 * Convert one file to a new PDF file. The input is capped at 512 MiB and
 * existing output paths are never overwritten. A failed write may leave a
 * partial output for manual review; a successful call leaves the source file
 * untouched.
 */
QW_API int qw_convert_file(const char *input_path,
                            const char *output_path,
                            const QwConvertOptions *options,
                            QwConvertReport *report);

/*!
 * Generic target-format counterpart of qw_convert_file(). The output
 * extension must match the requested target; a mismatch is rejected with
 * QW_STATUS_ERR_INVALID_ARGUMENT before any file is written.
 */
QW_API int qw_convert_file_to_format(const char *input_path,
                                      const char *output_path,
                                      QwOutputFormat output_format,
                                      const QwConvertOptions *options,
                                      QwConvertReport *report);

/*!
 * Check whether a file can be read through its registered Quirewise route.
 * PDF inputs are opened and every page is rendered; other non-audio inputs use
 * the PDF conversion route. Audio inputs are not verified by this package (the
 * application routes them to quirewise-audio and scans packets). Any generated
 * bytes are discarded without creating an output file. Memory use can
 * therefore be similar to a normal in-memory conversion. Only input keys,
 * archive passwords, and audio_track are used from options; output processing
 * and page-selection options are ignored so the check covers all pages.
 */
QW_API int qw_check_file(const char *input_path,
                          const QwConvertOptions *options,
                          QwConvertReport *report);

#ifdef __cplusplus
}
#endif

#endif
