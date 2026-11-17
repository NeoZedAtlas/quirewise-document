#ifndef QUIREWISE_DOCUMENT_BACKEND_H
#define QUIREWISE_DOCUMENT_BACKEND_H

#include "quirewise/convert.h"
#include "quirewise/document.h"
#include "quirewise/image.h"
#include "quirewise/text_archive.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

typedef struct QwPdfReader QwPdfReader;

typedef struct QwPdfOutlineItem {
    const char *title;
    size_t page_index;
    size_t depth;
} QwPdfOutlineItem;

int qw_doc_convert_memory_with_options(const uint8_t *data,
                                        size_t size,
                                        const char *input_extension,
                                        int reflow_epub,
                                        const QwConvertOptions *options,
                                        uint8_t **out_pdf,
                                        size_t *out_size,
                                        QwConvertReport *report);

/*
 * Non-PDF target dispatch for the data, subtitle and spreadsheet families.
 * The owning package's public parser and writer do the work; the input
 * extension (or, when absent, an unambiguous content sniff) selects the
 * parser. PDF targets belong to qw_doc_convert_memory_with_options().
 */
int qw_doc_convert_family_to_format(const uint8_t *data, size_t size,
                                    const char *input_extension,
                                    QwOutputFormat output_format,
                                    const QwConvertOptions *options,
                                    uint8_t **out_data, size_t *out_size,
                                    QwConvertReport *report);

/*
 * Layout HTML output. The readers' public HTML representation is the shared
 * intermediate for layout inputs; PDF, image and subtitle inputs are refused
 * because they have no layout route. The extension selects the reader, and a
 * content sniff is used when no extension is available.
 */
/* Outlook MSG (compound file) text/attachment extraction. */
int qw_msg_probe(const uint8_t *data, size_t size);
int qw_msg_to_html(const uint8_t *data, size_t size, uint8_t **out_html,
                   size_t *out_size, QwConvertReport *report);

/* Rewrites relative <img src> references in an HTMLZ member to data URIs
 * using resources from the same archive. */
int qw_doc_htmlz_inline_images(const uint8_t *archive, size_t archive_size,
                               const uint8_t *html, size_t html_size,
                               const uint8_t *member_name,
                               size_t member_name_size, uint8_t **out_html,
                               size_t *out_size, size_t *inlined);

int qw_doc_convert_html_memory_with_options(
    const uint8_t *data, size_t size, const char *input_extension,
    const QwConvertOptions *options, uint8_t **out_html, size_t *out_size,
    QwConvertReport *report);
int qw_doc_convert_markdown_memory_with_options(
    const uint8_t *data, size_t size, const char *input_extension,
    const QwConvertOptions *options, uint8_t **out_markdown,
    size_t *out_size, QwConvertReport *report);
int qw_doc_convert_html_memory(const uint8_t *data, size_t size,
                               const char *input_extension, uint8_t **out_html,
                               size_t *out_size, QwConvertReport *report);

/* Same input handling as qw_doc_convert_html_memory(), then the text package's
 * HTML-to-Markdown writer (quirewise/text.h). */
int qw_doc_convert_markdown_memory(const uint8_t *data, size_t size,
                                   const char *input_extension,
                                   uint8_t **out_markdown, size_t *out_size,
                                   QwConvertReport *report);

/* Minimal binary office writers built on the readers' HTML intermediate and
 * the internal ZIP writer: DOCX (WordprocessingML) and EPUB 2. */
int qw_doc_convert_docx_memory(const uint8_t *data, size_t size,
                               const char *input_extension, uint8_t **out_data,
                               size_t *out_size, QwConvertReport *report);
int qw_doc_convert_epub_memory(const uint8_t *data, size_t size,
                               const char *input_extension, uint8_t **out_data,
                               size_t *out_size, QwConvertReport *report);

/* Container/text writers: HTMLZ (index.html), TXTZ (index.txt) and a minimal
 * HWPX package, all built from the HTML intermediate. */
/* Layout-preserving PDF -> DOCX/EPUB: rasterize pages and embed them. */
/* Extract one page's text layer; used by the editable PDF -> Office writers. */
int qw_doc_pdf_extract_page_text(QwPdfReader *reader, size_t page_index,
                                 uint8_t **out_text, size_t *out_size);

int qw_doc_office_docx_text_from_pdf(const uint8_t *data, size_t size,
                                     const char *password,
                                     uint8_t **out_data, size_t *out_size,
                                     QwConvertReport *report);
int qw_doc_office_epub_text_from_pdf(const uint8_t *data, size_t size,
                                     const char *password,
                                     uint8_t **out_data, size_t *out_size,
                                     QwConvertReport *report);
int qw_doc_office_docx_from_pdf(const uint8_t *data, size_t size,
                                const char *password, uint8_t **out_data,
                                size_t *out_size, QwConvertReport *report);
int qw_doc_office_epub_from_pdf(const uint8_t *data, size_t size,
                                const char *password, uint8_t **out_data,
                                size_t *out_size, QwConvertReport *report);

int qw_doc_convert_htmlz_memory(const uint8_t *data, size_t size,
                                const char *input_extension,
                                uint8_t **out_data, size_t *out_size,
                                QwConvertReport *report);
int qw_doc_convert_text_archive_memory(QwTextArchiveKind kind,
                                       const uint8_t *data, size_t size,
                                       const char *input_extension,
                                       uint8_t **out_data, size_t *out_size,
                                       QwConvertReport *report);
int qw_doc_convert_pmlz_memory(const uint8_t *data, size_t size,
                               const char *input_extension,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report);
int qw_doc_convert_txtz_memory(const uint8_t *data, size_t size,
                               const char *input_extension,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report);
int qw_doc_convert_hwpx_memory(const uint8_t *data, size_t size,
                               const char *input_extension,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report);

/* Raster re-encoding: decoded PNG/BMP/TIFF/JPEG/WebP input to an encodable
 * target. The image package currently writes PNG/BMP/TIFF/JPEG/WebP. */
int qw_doc_convert_image_memory_with_options(
    const uint8_t *data, size_t size, QwOutputFormat output_format,
    const QwConvertOptions *options, uint8_t **out_data, size_t *out_size,
    QwConvertReport *report);
int qw_doc_convert_image_memory(const uint8_t *data, size_t size,
                                QwOutputFormat output_format,
                                uint8_t **out_data, size_t *out_size,
                                QwConvertReport *report);

int qw_doc_convert_memory(const uint8_t *data,
                            size_t size,
                            const char *input_extension,
                            int reflow_epub,
                            uint8_t **out_pdf,
                            size_t *out_size,
                            QwConvertReport *report);
int qw_doc_image_to_pdf(const QwImage *image,
                        uint8_t **out_pdf,
                        size_t *out_size,
                        QwConvertReport *report);
int qw_doc_image_to_pdf_mode(const QwImage *image,
                             int page_mode,
                             uint8_t **out_pdf,
                             size_t *out_size,
                             QwConvertReport *report);
int qw_doc_convert_memory_with_outline(const uint8_t *data,
                                         size_t size,
                                         const char *input_extension,
                                         const QwPdfOutlineItem *outline,
                                         size_t outline_count,
                                         uint8_t **out_pdf,
                                         size_t *out_size,
                                         QwConvertReport *report);

int qw_doc_convert_html(const char *html,
                          size_t html_size,
                          uint8_t **out_pdf,
                          size_t *out_size,
                          QwConvertReport *report);
/* Same renderer with a caller-supplied TrueType font embedded as /F3
 * (CIDFontType2 + Identity-H + ToUnicode). NULL font_path keeps the built-in
 * Helvetica/STSong-Light pair. */
/* Same renderer, emitting the PDF/A-1b structure (XMP metadata, sRGB output
 * intent and an embedded font is required). */
int qw_doc_convert_html_pdfa(const char *html,
                             size_t html_size,
                             float page_width_points,
                             float page_height_points,
                             const char *font_path,
                             uint8_t **out_pdf,
                             size_t *out_size,
                             QwConvertReport *report);
int qw_doc_convert_html_with_font(const char *html,
                                  size_t html_size,
                                  float page_width_points,
                                  float page_height_points,
                                  const char *font_path,
                                  uint8_t **out_pdf,
                                  size_t *out_size,
                                  QwConvertReport *report);
int qw_doc_convert_html_to_stream(const char *html,
                                   size_t html_size,
                                   FILE *stream,
                                   size_t max_output_bytes,
                                   size_t *out_size,
                                   QwConvertReport *report);
int qw_doc_convert_html_with_page_size(const char *html,
                                        size_t html_size,
                                        float page_width_points,
                                        float page_height_points,
                                        uint8_t **out_pdf,
                                        size_t *out_size,
                                        QwConvertReport *report);
int qw_doc_convert_html_with_page_size_to_stream(
    const char *html,
    size_t html_size,
    float page_width_points,
    float page_height_points,
    FILE *stream,
    size_t max_output_bytes,
    size_t *out_size,
    QwConvertReport *report);
int qw_doc_convert_html_file(const char *path,
                               uint8_t **out_pdf,
                               size_t *out_size,
                               QwConvertReport *report);
int qw_doc_convert_html_file_to_stream(const char *path,
                                         FILE *stream,
                                         size_t max_output_bytes,
                                         size_t *out_size,
                                         QwConvertReport *report);
int qw_doc_decode_caj_jp2(const uint8_t *data,
                            size_t size,
                            QwPageImage *out_page,
                            QwConvertReport *report);

int qw_pdf_render_page_memory(const uint8_t *data,
                             size_t size,
                             size_t page_index,
                             unsigned int dpi,
                             QwPageImage *out_page,
                             QwConvertReport *report);

typedef struct QwPdfOutlineEntry {
    char title[256];
    size_t page_index;
    int depth;
} QwPdfOutlineEntry;

int qw_pdf_reader_outline(QwPdfReader *reader, QwPdfOutlineEntry *entries,
                          size_t capacity, size_t *out_count);

int qw_pdf_reader_info(QwPdfReader *reader, char *title, size_t title_capacity,
                       char *author, size_t author_capacity);

int qw_pdf_reader_open_password(const uint8_t *data,
                                     size_t size,
                                     const char *password,
                                     QwPdfReader **out_reader,
                                     size_t *out_page_count,
                                     QwConvertReport *report);
int qw_pdf_reader_open(const uint8_t *data,
                             size_t size,
                             QwPdfReader **out_reader,
                             size_t *out_page_count,
                             QwConvertReport *report);
/* Open without requiring a supported security handler: page dictionaries are
 * unencrypted, so page count and boxes remain available. */
int qw_pdf_reader_open_structure(const uint8_t *data,
                                       size_t size,
                                       QwPdfReader **out_reader,
                                       size_t *out_page_count,
                                       QwConvertReport *report);
/* Media/crop boxes (points) and rotation for one page. */
int qw_pdf_reader_page_geometry(const QwPdfReader *reader,
                                      size_t page_index,
                                      double media[4],
                                      double crop[4],
                                      int *rotation);
int qw_pdf_reader_render_page(QwPdfReader *reader,
                                    size_t page_index,
                                    unsigned int dpi,
                                    QwPageImage *out_page,
                                    QwConvertReport *report);
int qw_pdf_reader_extract_text(QwPdfReader *reader,
                                     uint8_t **out_text,
                                     size_t *out_size,
                                     QwConvertReport *report);
int qw_pdf_reader_extract_text_to_stream(
    QwPdfReader *reader,
    FILE *stream,
    size_t max_output_bytes,
    size_t *out_size,
    QwConvertReport *report);
/* Object number of a page in document order; 0 when the index is invalid. */
size_t qw_pdf_reader_page_object(const QwPdfReader *reader, size_t index);
void qw_pdf_reader_close(QwPdfReader *reader);

int qw_pdf_merge_documents(const uint8_t *const *pdfs,
                                 const size_t *sizes,
                                 size_t count,
                                 uint8_t **out_pdf,
                                 size_t *out_size,
                                 size_t *out_pages,
                                 QwConvertReport *report);


/* Decode an ITU-T T.6 (CCITT Group 4) stream into GRAY8 samples (0 = black,
 * 255 = white) with the polarity PDF's /BlackIs1 flag asks for. Returns 1 on
 * success and hands ownership of *out_gray8 to the caller. */
int pdf_ccitt_decode_image(const uint8_t *data,
                           size_t size,
                           uint32_t width,
                           uint32_t height,
                           int black_is1,
                           uint8_t **out_gray8);

/* Decode a PDF JBIG2 image stream (optionally preceded by the /JBIG2Globals
 * segments) into GRAY8 samples (0 = black, 255 = white). Returns 1 on success,
 * QW_STATUS_ERR_UNSUPPORTED when the stream uses segment types this decoder
 * does not implement, or 0 for malformed input. */
int pdf_jbig2_decode_image(const uint8_t *globals,
                           size_t globals_size,
                           const uint8_t *data,
                           size_t size,
                           uint32_t width_hint,
                           uint32_t height_hint,
                           uint8_t **out_gray8);
#endif
