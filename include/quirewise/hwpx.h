#ifndef QUIREWISE_HWPX_H
#define QUIREWISE_HWPX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwHwpxStatus {
    QW_HWPX_OK = 0,
    QW_HWPX_ERR_INVALID_ARGUMENT = -1,
    QW_HWPX_ERR_OUT_OF_MEMORY = -2,
    QW_HWPX_ERR_FORMAT = -3,
    QW_HWPX_ERR_UNSUPPORTED = -4,
    QW_HWPX_ERR_RESOURCE_LIMIT = -5,
    QW_HWPX_ERR_DECODE = -6
} QwHwpxStatus;

typedef struct QwHwpxDocument QwHwpxDocument;

/* Parse HWPX package bytes and map its text and simple tables to HTML. */
QwHwpxStatus qw_hwpx_parse(const void *data, size_t size,
                           QwHwpxDocument **out_document);
/* HTML bytes are borrowed and remain valid until the document is freed. */
const uint8_t *qw_hwpx_document_html(const QwHwpxDocument *document,
                                     size_t *out_size);
/*! Return the number of sections. */
size_t qw_hwpx_document_section_count(const QwHwpxDocument *document);
/*! Return the number of unsupported objects skipped by the reader. */
size_t qw_hwpx_document_omitted_object_count(const QwHwpxDocument *document);
/*! Release a document returned by the parser; NULL is accepted. */
void qw_hwpx_document_free(QwHwpxDocument *document);

/* Build a minimal HWPX package from an HTML fragment. Paragraphs become OWPML
 * <p>/<t> nodes; tables are flattened. The result is malloc-owned and released
 * with qw_hwpx_buffer_free(). */
QwHwpxStatus qw_hwpx_create(const void *html, size_t html_size,
                            uint8_t **out_package, size_t *out_size);
/*! Release a buffer returned by qw_hwpx_create(); NULL is accepted. */
void qw_hwpx_buffer_free(void *buffer);

#ifdef __cplusplus
}
#endif

#endif
