/* Public PDF entry points.
 *
 * The implementation lives in the package-local reader/rasteriser/merger in
 * src/backends; this file only adapts it to the installed header so consumers
 * no longer need a private include path.
 */

#include <quirewise/pdf.h>

#include "../backends/document_backend.h"

#include <stdlib.h>
#include <string.h>

int qw_pdf_open(const uint8_t *data, size_t size,
                QwPdfDocument **out_document, size_t *out_page_count,
                QwConvertReport *report)
{
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    int status;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (out_page_count) *out_page_count = 0u;
    status = qw_pdf_reader_open(data, size, &reader, &pages, report);
    if (status != QW_STATUS_OK) return status;
    *out_document = (QwPdfDocument *)reader;
    if (out_page_count) *out_page_count = pages;
    return QW_STATUS_OK;
}

int qw_pdf_open_with_password(const uint8_t *data, size_t size,
                              const char *password,
                              QwPdfDocument **out_document,
                              size_t *out_page_count,
                              QwConvertReport *report)
{
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    int status;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (out_page_count) *out_page_count = 0u;
    status = qw_pdf_reader_open_password(data, size, password, &reader, &pages,
                                         report);
    if (status != QW_STATUS_OK) return status;
    *out_document = (QwPdfDocument *)reader;
    if (out_page_count) *out_page_count = pages;
    return QW_STATUS_OK;
}

static int pdf_info_string(QwPdfDocument *document, char *buffer,
                            size_t capacity, int author)
{
    QwPdfReader *reader = (QwPdfReader *)document;
    char empty;
    if (!document || !buffer || capacity == 0u)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    buffer[0] = '\0';
    if (!qw_pdf_reader_info(reader, author ? &empty : buffer,
                            author ? 0u : capacity,
                            author ? buffer : &empty,
                            author ? capacity : 0u))
        return QW_STATUS_OK;
    return QW_STATUS_OK;
}

size_t qw_pdf_bookmark_count(QwPdfDocument *document)
{
    QwPdfReader *reader = (QwPdfReader *)document;
    QwPdfOutlineEntry entries[256];
    size_t count = 0u;
    if (!reader) return 0u;
    if (!qw_pdf_reader_outline(reader, entries, sizeof(entries) / sizeof(entries[0]),
                               &count))
        return 0u;
    if (count > sizeof(entries) / sizeof(entries[0])) {
        size_t total = 0u;
        QwPdfOutlineEntry *all = (QwPdfOutlineEntry *)malloc(count * sizeof(*all));
        if (!all) return 0u;
        if (!qw_pdf_reader_outline(reader, all, count, &total)) {
            free(all);
            return 0u;
        }
        free(all);
        return total;
    }
    return count;
}

int qw_pdf_bookmark_at(QwPdfDocument *document, size_t index, char *buffer,
                       size_t capacity, size_t *out_page_index, int *out_depth)
{
    QwPdfReader *reader = (QwPdfReader *)document;
    QwPdfOutlineEntry *entries = NULL;
    size_t count = 0u;
    int status = QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!reader || !buffer || capacity == 0u) return status;
    buffer[0] = '\0';
    if (!qw_pdf_reader_outline(reader, NULL, 0u, &count) || index >= count)
        return status;
    if (count == 0u) return status;
    entries = (QwPdfOutlineEntry *)malloc(count * sizeof(*entries));
    if (!entries) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (!qw_pdf_reader_outline(reader, entries, count, &count)) {
        free(entries);
        return status;
    }
    if (index >= count) goto done;
    {
        size_t copy = strlen(entries[index].title);
        if (copy >= capacity) copy = capacity - 1u;
        memcpy(buffer, entries[index].title, copy);
        buffer[copy] = '\0';
        if (out_page_index) *out_page_index = entries[index].page_index;
        if (out_depth) *out_depth = entries[index].depth;
        status = QW_STATUS_OK;
    }
done:
    free(entries);
    return status;
}

int qw_pdf_document_title(QwPdfDocument *document, char *buffer,
                          size_t capacity)
{
    return pdf_info_string(document, buffer, capacity, 0);
}

int qw_pdf_document_author(QwPdfDocument *document, char *buffer,
                           size_t capacity)
{
    return pdf_info_string(document, buffer, capacity, 1);
}

int qw_pdf_open_structure(const uint8_t *data, size_t size,
                          QwPdfDocument **out_document,
                          size_t *out_page_count, QwConvertReport *report)
{
    QwPdfReader *reader = NULL;
    size_t pages = 0u;
    int status;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (out_page_count) *out_page_count = 0u;
    status = qw_pdf_reader_open_structure(data, size, &reader, &pages, report);
    if (status != QW_STATUS_OK) return status;
    *out_document = (QwPdfDocument *)reader;
    if (out_page_count) *out_page_count = pages;
    return QW_STATUS_OK;
}

int qw_pdf_page_geometry(QwPdfDocument *document, size_t page_index,
                         double media_box[4], double crop_box[4],
                         int *rotation)
{
    if (!document || !media_box || !crop_box || !rotation) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    return qw_pdf_reader_page_geometry((QwPdfReader *)document, page_index,
                                       media_box, crop_box, rotation);
}

int qw_pdf_render_page(QwPdfDocument *document, size_t page_index,
                       unsigned int dpi, QwPageImage *out_page,
                       QwConvertReport *report)
{
    if (!out_page) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_page, 0, sizeof(*out_page));
    if (!document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    return qw_pdf_reader_render_page((QwPdfReader *)document, page_index, dpi,
                                     out_page, report);
}

int qw_pdf_extract_text(QwPdfDocument *document, uint8_t **out_text,
                        size_t *out_size, QwConvertReport *report)
{
    if (!out_text || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_size = 0u;
    if (!document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    return qw_pdf_reader_extract_text((QwPdfReader *)document, out_text,
                                      out_size, report);
}

int qw_pdf_merge(const uint8_t *const *pdfs, const size_t *sizes, size_t count,
                 uint8_t **out_pdf, size_t *out_size, size_t *out_pages,
                 QwConvertReport *report)
{
    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (out_pages) *out_pages = 0u;
    return qw_pdf_merge_documents(pdfs, sizes, count, out_pdf, out_size,
                                  out_pages, report);
}

void qw_pdf_close(QwPdfDocument *document)
{
    qw_pdf_reader_close((QwPdfReader *)document);
}

void qw_pdf_free(void *buffer)
{
    free(buffer);
}

void qw_pdf_page_free(QwPageImage *page)
{
    if (!page) return;
    free((void *)page->data);
    memset(page, 0, sizeof(*page));
}
