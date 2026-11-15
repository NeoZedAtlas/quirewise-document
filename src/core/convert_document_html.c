/* Layout HTML and Markdown output for the document composition layer.
 *
 * Every layout-family reader in Quirewise normalizes its content to HTML, so
 * HTML is the public intermediate representation here: this unit collects that
 * HTML through the readers' public APIs and either returns it (QW_FORMAT_HTML)
 * or converts it to Markdown with the text package's writer
 * (QW_FORMAT_MARKDOWN). Worksheet inputs get an HTML table built from the
 * public cell views; plain text goes through quirewise-text's plain-text
 * renderer.
 *
 * PDF, image and subtitle inputs have no layout route here and are refused
 * with a specific note (PDF text extraction is the TXT target's job).
 */

#include <quirewise/convert.h>
#include <quirewise/docx.h>
#include <quirewise/email.h>
#include <quirewise/epub.h>
#include <quirewise/fb2.h>
#include <quirewise/htmlz.h>
#include <quirewise/hwpx.h>
#include <quirewise/markdown.h>
#include <quirewise/mdx.h>
#include <quirewise/mobi.h>
#include <quirewise/odf_presentation.h>
#include <quirewise/odt.h>
#include <quirewise/plain_text.h>
#include <quirewise/pptx.h>
#include <quirewise/rtf.h>
#include <quirewise/spreadsheet.h>
#include <quirewise/text_archive.h>

#include "../backends/document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_DOC_HTML_LIMIT ((size_t)64u * 1024u * 1024u)
#define QW_DOC_HTML_INPUT_LIMIT ((size_t)512u * 1024u * 1024u)
#define QW_DOC_HTML_MAX_CELLS 500000u

typedef struct HtmlBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} HtmlBuffer;

typedef struct HtmlCellRef {
    size_t row;
    size_t column;
    size_t index;
} HtmlCellRef;

static int html_reserve(HtmlBuffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *grown;

    if (buffer->size > QW_DOC_HTML_LIMIT ||
        additional > QW_DOC_HTML_LIMIT - buffer->size)
        return 0;
    needed = buffer->size + additional;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 4096u;
    while (capacity < needed) {
        if (capacity > QW_DOC_HTML_LIMIT / 2u) {
            capacity = QW_DOC_HTML_LIMIT;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed) return 0;
    grown = (uint8_t *)realloc(buffer->data, capacity);
    if (!grown) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int html_append(HtmlBuffer *buffer, const void *data, size_t size)
{
    if (!size) return 1;
    if (!html_reserve(buffer, size)) return 0;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static int html_append_text(HtmlBuffer *buffer, const char *text)
{
    return text && text[0] ? html_append(buffer, text, strlen(text)) : 1;
}

static int html_append_escaped(HtmlBuffer *buffer, const uint8_t *text,
                               size_t size)
{
    size_t index;
    size_t start = 0u;

    for (index = 0u; index < size; ++index) {
        const char *replacement = NULL;

        switch (text[index]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        default: break;
        }
        if (!replacement) continue;
        if (!html_append(buffer, text + start, index - start) ||
            !html_append_text(buffer, replacement))
            return 0;
        start = index + 1u;
    }
    return html_append(buffer, text + start, size - start);
}

/* Hand the buffer to the caller, which releases it with qw_buffer_free. */
static int html_take(HtmlBuffer *buffer, uint8_t **out_html,
                     size_t *out_size)
{
    if (!buffer->data) {
        buffer->data = (uint8_t *)malloc(1u);
        if (!buffer->data) return QW_STATUS_ERR_OUT_OF_MEMORY;
        buffer->size = 0u;
    }
    *out_html = buffer->data;
    *out_size = buffer->size;
    buffer->data = NULL;
    buffer->size = 0u;
    buffer->capacity = 0u;
    return QW_STATUS_OK;
}

static int html_fail(QwConvertReport *report, int status, const char *what,
                     const char *detail)
{
    if (report && !report->note[0]) {
        snprintf(report->note, sizeof(report->note), "%s: %s", what,
                 detail && detail[0] ? detail : "HTML output failed");
    }
    return status;
}

static int html_copy_bytes(const uint8_t *data, size_t size, uint8_t **out,
                           size_t *out_size)
{
    uint8_t *copy = (uint8_t *)malloc(size ? size : 1u);

    if (!copy) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (size) memcpy(copy, data, size);
    *out = copy;
    *out_size = size;
    return QW_STATUS_OK;
}

static int html_extension_is(const char *extension, const char *name)
{
    return extension && extension[0] && strcmp(extension, name) == 0;
}

static int html_contains(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static int html_region_equals(const uint8_t *data, size_t size, size_t offset,
                              const char *text)
{
    size_t text_size = strlen(text);

    return offset <= size && text_size <= size - offset &&
           memcmp(data + offset, text, text_size) == 0;
}

/* Package status -> QwStatus. The positive and negative enums are handled by
 * two helpers so each reader can use its own convention. */
static int html_status_positive(int status)
{
    switch (status) {
    case 0: return QW_STATUS_OK;
    case 1: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case 2: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case 3: return QW_STATUS_ERR_FORMAT;
    case 4: return QW_STATUS_ERR_UNSUPPORTED;
    case 5: return QW_STATUS_ERR_DECODE;
    case 6: return QW_STATUS_ERR_RESOURCE_LIMIT;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

static int html_status_negative(int status)
{
    switch (status) {
    case 0: return QW_STATUS_OK;
    case -1: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case -2: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case -4: return QW_STATUS_ERR_FORMAT;
    case -5: return QW_STATUS_ERR_UNSUPPORTED;
    case -6: return QW_STATUS_ERR_DECODE;
    case -11: return QW_STATUS_ERR_RESOURCE_LIMIT;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

static int html_from_docx(const uint8_t *data, size_t size, uint8_t **out,
                          size_t *out_size, QwConvertReport *report)
{
    QwDocxDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;

    status = (int)qw_docx_parse(data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_positive(status), "DOCX parse",
                         "");
    html = qw_docx_document_html(document, &html_size);
    status = html ? html_copy_bytes(html, html_size, out, out_size)
                  : QW_STATUS_ERR_FORMAT;
    qw_docx_document_free(document);
    return status;
}

static int html_from_odt(int format, const uint8_t *data, size_t size,
                         uint8_t **out, size_t *out_size,
                         QwConvertReport *report)
{
    QwOdtDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;

    status = (int)qw_odt_parse((QwOdtFormat)format, data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_positive(status), "ODT parse",
                         "");
    html = qw_odt_document_html(document, &html_size);
    status = html ? html_copy_bytes(html, html_size, out, out_size)
                  : QW_STATUS_ERR_FORMAT;
    qw_odt_document_free(document);
    return status;
}

static int html_from_pptx(int format, const uint8_t *data, size_t size,
                          uint8_t **out, size_t *out_size,
                          QwConvertReport *report)
{
    QwPptxDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;

    status = (int)qw_pptx_parse((QwPptxFormat)format, data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_positive(status), "PPTX parse",
                         "");
    html = qw_pptx_document_html(document, &html_size);
    status = html ? html_copy_bytes(html, html_size, out, out_size)
                  : QW_STATUS_ERR_FORMAT;
    qw_pptx_document_free(document);
    return status;
}

static int html_from_odp(int format, const uint8_t *data, size_t size,
                         uint8_t **out, size_t *out_size,
                         QwConvertReport *report)
{
    QwOdpDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;

    status = (int)qw_odp_parse((QwOdpFormat)format, data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_positive(status), "ODP parse",
                         "");
    html = qw_odp_document_html(document, &html_size);
    status = html ? html_copy_bytes(html, html_size, out, out_size)
                  : QW_STATUS_ERR_FORMAT;
    qw_odp_document_free(document);
    return status;
}

static int html_from_hwpx(const uint8_t *data, size_t size, uint8_t **out,
                          size_t *out_size, QwConvertReport *report)
{
    QwHwpxDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;

    status = (int)qw_hwpx_parse(data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_positive(status), "HWPX parse",
                         "");
    html = qw_hwpx_document_html(document, &html_size);
    status = html ? html_copy_bytes(html, html_size, out, out_size)
                  : QW_STATUS_ERR_FORMAT;
    qw_hwpx_document_free(document);
    return status;
}

static int html_from_email(const uint8_t *data, size_t size, uint8_t **out,
                           size_t *out_size, QwConvertReport *report)
{
    QwEmailDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;

    status = (int)qw_email_parse(data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_negative(status), "EML parse",
                         "");
    html = qw_email_document_html(document, &html_size);
    status = html ? html_copy_bytes(html, html_size, out, out_size)
                  : QW_STATUS_ERR_FORMAT;
    qw_email_document_free(document);
    return status;
}

static int html_from_fb2(const uint8_t *data, size_t size, uint8_t **out,
                         size_t *out_size, QwConvertReport *report)
{
    QwFB2Document *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;

    status = (int)qw_fb2_parse(data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_negative(status), "FB2 parse",
                         "");
    html = qw_fb2_document_html(document, &html_size);
    status = html ? html_copy_bytes(html, html_size, out, out_size)
                  : QW_STATUS_ERR_FORMAT;
    qw_fb2_document_free(document);
    return status;
}

static int html_from_htmlz(const uint8_t *data, size_t size, uint8_t **out,
                           size_t *out_size, QwConvertReport *report)
{
    QwHtmlzDocument document;
    const char *error = NULL;
    int status;

    memset(&document, 0, sizeof(document));
    status = (int)qw_htmlz_extract_primary_document(data, size, &document,
                                                    &error);
    if (status != 0) {
        if (report) {
            snprintf(report->note, sizeof(report->note), "HTMLZ parse: %s",
                     error && error[0] ? error : "no primary document");
        }
        return html_status_negative(status);
    }
    status = html_copy_bytes(document.html_bytes, document.html_size, out,
                             out_size);
    qw_htmlz_document_free(&document);
    return status;
}

static int html_from_epub(const uint8_t *data, size_t size, uint8_t **out,
                          size_t *out_size, QwConvertReport *report)
{
    QwEpubDocument *document = NULL;
    HtmlBuffer buffer;
    size_t section_index;
    int status;

    memset(&buffer, 0, sizeof(buffer));
    status = (int)qw_epub_parse(data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_negative(status), "EPUB parse",
                         "");
    if (!html_append_text(&buffer, "<html><body>\n")) goto out_of_memory;
    for (section_index = 0u; section_index < qw_epub_section_count(document);
         ++section_index) {
        QwEpubSectionView section;
        size_t paragraph_index;

        memset(&section, 0, sizeof(section));
        if (qw_epub_section_at(document, section_index, &section) !=
            QW_EPUB_OK)
            continue;
        if (!html_append_text(&buffer, "<section>\n")) goto out_of_memory;
        if (section.title && section.title_size) {
            if (!html_append_text(&buffer, "<h1>") ||
                !html_append_escaped(&buffer,
                                     (const uint8_t *)section.title,
                                     section.title_size) ||
                !html_append_text(&buffer, "</h1>\n"))
                goto out_of_memory;
        }
        for (paragraph_index = 0u;
             paragraph_index < section.paragraph_count; ++paragraph_index) {
            QwEpubParagraphView paragraph;
            char open_tag[8];
            char close_tag[8];
            size_t run_index;
            int heading;

            memset(&paragraph, 0, sizeof(paragraph));
            if (qw_epub_paragraph_at(document, section_index,
                                     paragraph_index, &paragraph) != QW_EPUB_OK)
                continue;
            if (paragraph.run_count) {
                QwEpubRunView first;
                memset(&first, 0, sizeof(first));
                if (qw_epub_run_at(document, section_index, paragraph_index,
                                   0u, &first) == QW_EPUB_OK &&
                    (first.style_flags & QW_EPUB_STYLE_TABLE_ROW)) {
                    int cell_open = 0;
                    if (!html_append_text(&buffer, "<table><tr>"))
                        goto out_of_memory;
                    for (run_index = 0u; run_index < paragraph.run_count;
                         ++run_index) {
                        QwEpubRunView run;
                        memset(&run, 0, sizeof(run));
                        if (qw_epub_run_at(document, section_index,
                                           paragraph_index, run_index,
                                           &run) != QW_EPUB_OK)
                            continue;
                        if (run.style_flags & QW_EPUB_STYLE_TABLE_CELL) {
                            if (cell_open &&
                                !html_append_text(&buffer, "</td>"))
                                goto out_of_memory;
                            if (!html_append_text(&buffer, "<td>"))
                                goto out_of_memory;
                            cell_open = 1;
                        }
                        if (run.utf8_text && run.text_size &&
                            !html_append_escaped(&buffer, run.utf8_text,
                                                 run.text_size))
                            goto out_of_memory;
                    }
                    if (cell_open &&
                        !html_append_text(&buffer, "</td>"))
                        goto out_of_memory;
                    if (!html_append_text(&buffer, "</tr></table>"))
                        goto out_of_memory;
                    continue;
                }
                if (qw_epub_run_at(document, section_index, paragraph_index,
                                   0u, &first) == QW_EPUB_OK &&
                    (first.style_flags &
                     (QW_EPUB_STYLE_LIST_ITEM | QW_EPUB_STYLE_PRE))) {
                    int list_item = (first.style_flags &
                                     QW_EPUB_STYLE_LIST_ITEM) != 0;
                    if (!html_append_text(&buffer,
                                          list_item ? "<ul><li>" : "<pre>"))
                        goto out_of_memory;
                    for (run_index = 0u; run_index < paragraph.run_count;
                         ++run_index) {
                        QwEpubRunView run;
                        memset(&run, 0, sizeof(run));
                        if (qw_epub_run_at(document, section_index,
                                           paragraph_index, run_index,
                                           &run) != QW_EPUB_OK)
                            continue;
                        if (run.utf8_text && run.text_size &&
                            !html_append_escaped(&buffer, run.utf8_text,
                                                 run.text_size))
                            goto out_of_memory;
                    }
                    if (!html_append_text(&buffer,
                                          list_item ? "</li></ul>" : "</pre>"))
                        goto out_of_memory;
                    continue;
                }
            }
            heading = paragraph.heading_level >= 1u &&
                      paragraph.heading_level <= 6u
                          ? (int)paragraph.heading_level : 0;
            if (heading) {
                snprintf(open_tag, sizeof(open_tag), "<h%d>", heading);
                snprintf(close_tag, sizeof(close_tag), "</h%d>", heading);
            } else {
                snprintf(open_tag, sizeof(open_tag), "%s", "<p>");
                snprintf(close_tag, sizeof(close_tag), "%s", "</p>");
            }
            if (!html_append_text(&buffer, open_tag)) goto out_of_memory;
            for (run_index = 0u; run_index < paragraph.run_count;
                 ++run_index) {
                QwEpubRunView run;

                memset(&run, 0, sizeof(run));
                if (qw_epub_run_at(document, section_index, paragraph_index,
                                   run_index, &run) != QW_EPUB_OK)
                    continue;
                if (run.style_flags & QW_EPUB_STYLE_BOLD) {
                    if (!html_append_text(&buffer, "<strong>"))
                        goto out_of_memory;
                }
                if (run.style_flags & QW_EPUB_STYLE_ITALIC) {
                    if (!html_append_text(&buffer, "<em>"))
                        goto out_of_memory;
                }
                if (run.style_flags & QW_EPUB_STYLE_CODE) {
                    if (!html_append_text(&buffer, "<code>"))
                        goto out_of_memory;
                }
                if (run.link_target && run.link_target_size) {
                    if (!html_append_text(&buffer, "<a href=\"") ||
                        !html_append_escaped(
                            &buffer, (const uint8_t *)run.link_target,
                            run.link_target_size) ||
                        !html_append_text(&buffer, "\">"))
                        goto out_of_memory;
                }
                if (run.utf8_text && run.text_size &&
                    !html_append_escaped(&buffer, run.utf8_text,
                                           run.text_size))
                    goto out_of_memory;
                if (run.link_target && run.link_target_size) {
                    if (!html_append_text(&buffer, "</a>")) goto out_of_memory;
                }
                if (run.style_flags & QW_EPUB_STYLE_CODE) {
                    if (!html_append_text(&buffer, "</code>"))
                        goto out_of_memory;
                }
                if (run.style_flags & QW_EPUB_STYLE_ITALIC) {
                    if (!html_append_text(&buffer, "</em>"))
                        goto out_of_memory;
                }
                if (run.style_flags & QW_EPUB_STYLE_BOLD) {
                    if (!html_append_text(&buffer, "</strong>"))
                        goto out_of_memory;
                }
            }
            if (!html_append_text(&buffer, close_tag) ||
                !html_append_text(&buffer, "\n"))
                goto out_of_memory;
        }
        if (!html_append_text(&buffer, "</section>\n")) goto out_of_memory;
    }
    if (!html_append_text(&buffer, "</body></html>\n")) goto out_of_memory;
    qw_epub_document_free(document);
    return html_take(&buffer, out, out_size);

out_of_memory:
    free(buffer.data);
    qw_epub_document_free(document);
    return QW_STATUS_ERR_OUT_OF_MEMORY;
}

static int html_from_rtf(const uint8_t *data, size_t size, uint8_t **out,
                         size_t *out_size, QwConvertReport *report)
{
    QwRtfDocument document;
    int status;

    memset(&document, 0, sizeof(document));
    status = (int)qw_rtf_parse_html(data, size, &document);
    if (status != 0 || !document.html)
        return html_fail(report, html_status_positive(status), "RTF parse",
                         "");
    status = html_copy_bytes((const uint8_t *)document.html,
                             document.html_size, out, out_size);
    qw_rtf_document_free(&document);
    return status;
}

static int html_from_markdown(const uint8_t *data, size_t size, uint8_t **out,
                              size_t *out_size, QwConvertReport *report)
{
    QwMarkdownDocument document;
    int status;

    memset(&document, 0, sizeof(document));
    status = (int)qw_markdown_render_html(data, size, &document);
    if (status != 0 || !document.html)
        return html_fail(report, html_status_positive(status),
                         "Markdown parse", "");
    status = html_copy_bytes((const uint8_t *)document.html, document.html_size,
                             out, out_size);
    qw_markdown_document_free(&document);
    return status;
}

static int html_from_plain_text(const uint8_t *data, size_t size,
                                uint8_t **out, size_t *out_size,
                                QwConvertReport *report)
{
    static const char empty_html[] = "<html><body></body></html>";
    QwPlainTextDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;

    if (!size)
        return html_copy_bytes((const uint8_t *)empty_html,
                               strlen(empty_html), out, out_size);

    status = (int)qw_plain_text_parse(data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_positive(status),
                         "plain text parse", "");
    html = qw_plain_text_document_html(document, &html_size);
    status = html ? html_copy_bytes(html, html_size, out, out_size)
                  : QW_STATUS_ERR_FORMAT;
    qw_plain_text_document_free(document);
    return status;
}

static int html_from_text_archive(QwTextArchiveKind kind, const uint8_t *data,
                                  size_t size, uint8_t **out,
                                  size_t *out_size, QwConvertReport *report)
{
    QwTextArchiveDocument document;
    const char *error = NULL;
    int status;

    memset(&document, 0, sizeof(document));
    status = (int)qw_text_archive_extract_primary(data, size, kind, &document,
                                                  &error);
    if (status != 0) {
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "text archive extraction failed: %s",
                     error && error[0] ? error : "no primary member");
        }
        return html_status_negative(status);
    }
    status = html_from_plain_text(document.text_bytes, document.text_size, out,
                                  out_size, report);
    qw_text_archive_document_free(&document);
    return status;
}

static int html_from_mobi(const uint8_t *data, size_t size, uint8_t **out,
                          size_t *out_size, QwConvertReport *report)
{
    QwMobiDocument *document = NULL;
    const uint8_t *text;
    size_t text_size = 0u;
    int status;

    status = (int)qw_mobi_parse(data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_negative(status), "MOBI parse",
                         "");
    text = qw_mobi_document_text(document, &text_size);
    status = text ? html_from_plain_text(text, text_size, out, out_size,
                                         report)
                  : QW_STATUS_ERR_FORMAT;
    qw_mobi_document_free(document);
    return status;
}

static int html_from_mdx(const uint8_t *data, size_t size, uint8_t **out,
                         size_t *out_size, QwConvertReport *report)
{
    QwMdxDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    char error[256];
    int status;

    memset(error, 0, sizeof(error));
    status = (int)qw_mdx_parse(data, size, &document, error, sizeof(error));
    if (status != 0 || !document)
        return html_fail(report, html_status_negative(status), "MDX parse",
                         error);
    status = (int)qw_mdx_document_html(document, &html, &html_size);
    if (status != 0 || !html) {
        qw_mdx_document_free(document);
        return html_fail(report, html_status_negative(status), "MDX render",
                         "");
    }
    status = html_copy_bytes(html, html_size, out, out_size);
    qw_mdx_document_free(document);
    return status;
}

static int html_cell_ref_compare(const void *left, const void *right)
{
    const HtmlCellRef *a = (const HtmlCellRef *)left;
    const HtmlCellRef *b = (const HtmlCellRef *)right;

    if (a->row != b->row) return a->row < b->row ? -1 : 1;
    if (a->column != b->column) return a->column < b->column ? -1 : 1;
    return 0;
}

/* PDF has no layout HTML, but its text layer is a legitimate reverse source:
 * the TXT route extracts the text, the text package wraps it as HTML, and the
 * office writers can then emit DOCX/EPUB. This is text-only by design. */
static int html_from_pdf_text(const uint8_t *data, size_t size,
                              uint8_t **out_html, size_t *out_size,
                              QwConvertReport *report)
{
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int status;

    status = qw_convert_to_format_memory(data, size, QW_FORMAT_TXT, NULL,
                                         &text, &text_size, report);
    if (status != QW_STATUS_OK) return status;
    status = html_from_plain_text(text, text_size, out_html, out_size, report);
    free(text);
    if (status == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "PDF text layer only: layout, images, tables and styles are "
                 "not reconstructed");
    }
    return status;
}

static int html_from_spreadsheet(QwSpreadsheetFormat format,
                                 const uint8_t *data, size_t size,
                                 uint8_t **out, size_t *out_size,
                                 QwConvertReport *report)
{
    QwSpreadsheetDocument *document = NULL;
    HtmlBuffer buffer;
    size_t sheet_index;
    int status;

    memset(&buffer, 0, sizeof(buffer));
    status = (int)qw_spreadsheet_parse(format, data, size, &document);
    if (status != 0 || !document)
        return html_fail(report, html_status_negative(status),
                         "spreadsheet parse", "");
    if (!html_append_text(&buffer, "<html><body>\n")) goto out_of_memory;
    for (sheet_index = 0u;
         sheet_index < qw_spreadsheet_sheet_count(document);
         ++sheet_index) {
        const QwSpreadsheetSheet *sheet =
            qw_spreadsheet_sheet_at(document, sheet_index);
        size_t name_size = 0u;
        const char *name;
        size_t cell_count;
        HtmlCellRef *refs = NULL;
        size_t ref_index;
        size_t current_row = 0u;
        size_t current_column = 0u;
        int row_open = 0;

        if (!sheet) continue;
        name = qw_spreadsheet_sheet_name(sheet, &name_size);
        if (!html_append_text(&buffer, "<h2>") ||
            (name && !html_append_escaped(&buffer,
                                          (const uint8_t *)name, name_size)) ||
            !html_append_text(&buffer, "</h2>\n<table>\n"))
            goto out_of_memory;
        cell_count = qw_spreadsheet_cell_count(sheet);
        if (cell_count > QW_DOC_HTML_MAX_CELLS) {
            if (!html_append_text(&buffer,
                                  "<tr><td>(sheet too large for HTML)</td></tr>\n"))
                goto out_of_memory;
            if (!html_append_text(&buffer, "</table>\n")) goto out_of_memory;
            continue;
        }
        if (cell_count) {
            refs = (HtmlCellRef *)malloc(cell_count * sizeof(*refs));
            if (!refs) goto out_of_memory;
            for (ref_index = 0u; ref_index < cell_count; ++ref_index) {
                QwSpreadsheetCellView cell;

                memset(&cell, 0, sizeof(cell));
                if (qw_spreadsheet_cell_at(sheet, ref_index, &cell) !=
                    QW_SPREADSHEET_OK) {
                    refs[ref_index].row = (size_t)-1;
                    refs[ref_index].column = (size_t)-1;
                    refs[ref_index].index = ref_index;
                    continue;
                }
                refs[ref_index].row = cell.row;
                refs[ref_index].column = cell.column;
                refs[ref_index].index = ref_index;
            }
            /* Drop unreadable cells by sorting them to the end. */
            qsort(refs, cell_count, sizeof(*refs), html_cell_ref_compare);
            for (ref_index = 0u; ref_index < cell_count; ++ref_index) {
                QwSpreadsheetCellView cell;

                if (refs[ref_index].row == (size_t)-1) continue;
                memset(&cell, 0, sizeof(cell));
                if (qw_spreadsheet_cell_at(sheet, refs[ref_index].index,
                                           &cell) != QW_SPREADSHEET_OK)
                    continue;
                while (current_row < cell.row) {
                    if (row_open) {
                        if (!html_append_text(&buffer, "</tr>\n"))
                            goto out_of_memory;
                        row_open = 0;
                    }
                    ++current_row;
                    current_column = 0u;
                }
                if (!row_open) {
                    if (!html_append_text(&buffer, "<tr>"))
                        goto out_of_memory;
                    row_open = 1;
                }
                while (current_column < cell.column) {
                    if (!html_append_text(&buffer, "<td></td>"))
                        goto out_of_memory;
                    ++current_column;
                }
                current_column = cell.column + 1u;
                if (!html_append_text(&buffer, "<td>") ||
                    (cell.value && cell.value_size &&
                     !html_append_escaped(&buffer, cell.value,
                                            cell.value_size)) ||
                    !html_append_text(&buffer, "</td>"))
                    goto out_of_memory;
            }
            if (row_open && !html_append_text(&buffer, "</tr>\n"))
                goto out_of_memory;
            free(refs);
        }
        if (!html_append_text(&buffer, "</table>\n")) goto out_of_memory;
    }
    if (!html_append_text(&buffer, "</body></html>\n")) goto out_of_memory;
    qw_spreadsheet_document_free(document);
    return html_take(&buffer, out, out_size);

out_of_memory:
    free(buffer.data);
    qw_spreadsheet_document_free(document);
    return QW_STATUS_ERR_OUT_OF_MEMORY;
}

static int html_spreadsheet_format(const char *extension,
                                   QwSpreadsheetFormat *out_format)
{
    if (html_extension_is(extension, "csv")) *out_format = QW_SPREADSHEET_FORMAT_CSV;
    else if (html_extension_is(extension, "tsv"))
        *out_format = QW_SPREADSHEET_FORMAT_TSV;
    else if (html_extension_is(extension, "xls")) *out_format = QW_SPREADSHEET_FORMAT_XLS;
    else if (html_extension_is(extension, "xlt")) *out_format = QW_SPREADSHEET_FORMAT_XLT;
    else if (html_extension_is(extension, "xlsx"))
        *out_format = QW_SPREADSHEET_FORMAT_XLSX;
    else if (html_extension_is(extension, "xlsm"))
        *out_format = QW_SPREADSHEET_FORMAT_XLSM;
    else if (html_extension_is(extension, "xltx"))
        *out_format = QW_SPREADSHEET_FORMAT_XLTX;
    else if (html_extension_is(extension, "ods")) *out_format = QW_SPREADSHEET_FORMAT_ODS;
    else if (html_extension_is(extension, "ots")) *out_format = QW_SPREADSHEET_FORMAT_OTS;
    else if (html_extension_is(extension, "fods"))
        *out_format = QW_SPREADSHEET_FORMAT_FODS;
    else return 0;
    return 1;
}

static int html_from_zip(const uint8_t *data, size_t size,
                         const char *extension, uint8_t **out,
                         size_t *out_size, QwConvertReport *report)
{
    if (html_contains(data, size, "application/epub+zip"))
        return html_from_epub(data, size, out, out_size, report);
    if (html_extension_is(extension, "pptx") ||
        html_extension_is(extension, "pptm") ||
        html_extension_is(extension, "potx") ||
        html_extension_is(extension, "potm") ||
        html_extension_is(extension, "ppsx") ||
        html_extension_is(extension, "ppsm") ||
        html_contains(data, size, "ppt/presentation.xml")) {
        int format = QW_PPTX_FORMAT_PPTX;
        if (html_extension_is(extension, "pptm")) format = QW_PPTX_FORMAT_PPTM;
        else if (html_extension_is(extension, "potx"))
            format = QW_PPTX_FORMAT_POTX;
        else if (html_extension_is(extension, "potm"))
            format = QW_PPTX_FORMAT_POTM;
        else if (html_extension_is(extension, "ppsx"))
            format = QW_PPTX_FORMAT_PPSX;
        else if (html_extension_is(extension, "ppsm"))
            format = QW_PPTX_FORMAT_PPSM;
        return html_from_pptx(format, data, size, out, out_size, report);
    }
    if (html_extension_is(extension, "odp") ||
        html_extension_is(extension, "otp") ||
        html_extension_is(extension, "fodp") ||
        html_contains(data, size,
                      "application/vnd.oasis.opendocument.presentation")) {
        int format = QW_ODP_FORMAT_ODP;
        if (html_extension_is(extension, "otp")) format = QW_ODP_FORMAT_OTP;
        else if (html_extension_is(extension, "fodp"))
            format = QW_ODP_FORMAT_FODP;
        return html_from_odp(format, data, size, out, out_size, report);
    }
    if (html_extension_is(extension, "hwpx") ||
        html_contains(data, size, "Contents/section"))
        return html_from_hwpx(data, size, out, out_size, report);
    if (html_extension_is(extension, "htmlz") ||
        html_contains(data, size, "index.html") ||
        html_contains(data, size, "index.htm"))
        return html_from_htmlz(data, size, out, out_size, report);
    if (html_contains(data, size, "xl/workbook.xml")) {
        return html_from_spreadsheet(QW_SPREADSHEET_FORMAT_XLSX, data, size, out,
                                     out_size, report);
    }
    if (html_extension_is(extension, "xlsx") ||
        html_extension_is(extension, "xlsm") ||
        html_extension_is(extension, "xltx"))
        return html_from_spreadsheet(QW_SPREADSHEET_FORMAT_XLSX, data, size, out,
                                     out_size, report);
    if (html_extension_is(extension, "ods") ||
        html_extension_is(extension, "ots") ||
        html_contains(data, size,
                      "application/vnd.oasis.opendocument.spreadsheet"))
        return html_from_spreadsheet(QW_SPREADSHEET_FORMAT_ODS, data, size, out,
                                     out_size, report);
    if (html_extension_is(extension, "docx") ||
        html_extension_is(extension, "docm") ||
        html_extension_is(extension, "dotx") ||
        html_extension_is(extension, "dotm") ||
        qw_docx_probe(data, size) != 0)
        return html_from_docx(data, size, out, out_size, report);
    if (html_extension_is(extension, "odt") ||
        html_extension_is(extension, "ott") ||
        html_extension_is(extension, "fodt") ||
        html_contains(data, size, "application/vnd.oasis.opendocument.text") ||
        html_contains(data, size, "content.xml")) {
        int format = QW_ODT_FORMAT_ODT;
        if (html_extension_is(extension, "ott")) format = QW_ODT_FORMAT_OTT;
        else if (html_extension_is(extension, "fodt"))
            format = QW_ODT_FORMAT_FODT;
        return html_from_odt(format, data, size, out, out_size, report);
    }
    return html_fail(report, QW_STATUS_ERR_UNSUPPORTED, "layout HTML",
                     "unrecognized ZIP package (not DOCX/ODT/PPTX/ODP/HWPX/"
                     "HTMLZ/EPUB/spreadsheet)");
}

int qw_doc_convert_html_memory(const uint8_t *data, size_t size,
                               const char *input_extension, uint8_t **out_html,
                               size_t *out_size, QwConvertReport *report)
{
    const char *extension = input_extension ? input_extension : "";

    if (!out_html || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_html = NULL;
    *out_size = 0u;
    if (!data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (size > QW_DOC_HTML_INPUT_LIMIT)
        return html_fail(report, QW_STATUS_ERR_RESOURCE_LIMIT, "layout HTML",
                         "input exceeds the 512 MiB document limit");

    if (html_extension_is(extension, "docx") ||
        html_extension_is(extension, "docm") ||
        html_extension_is(extension, "dotx") ||
        html_extension_is(extension, "dotm"))
        return html_from_docx(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "odt") ||
        html_extension_is(extension, "ott") ||
        html_extension_is(extension, "fodt")) {
        int format = QW_ODT_FORMAT_ODT;
        if (html_extension_is(extension, "ott")) format = QW_ODT_FORMAT_OTT;
        else if (html_extension_is(extension, "fodt"))
            format = QW_ODT_FORMAT_FODT;
        return html_from_odt(format, data, size, out_html, out_size, report);
    }
    if (html_extension_is(extension, "pptx") ||
        html_extension_is(extension, "pptm") ||
        html_extension_is(extension, "potx") ||
        html_extension_is(extension, "potm") ||
        html_extension_is(extension, "ppsx") ||
        html_extension_is(extension, "ppsm")) {
        int format = QW_PPTX_FORMAT_PPTX;
        if (html_extension_is(extension, "pptm")) format = QW_PPTX_FORMAT_PPTM;
        else if (html_extension_is(extension, "potx"))
            format = QW_PPTX_FORMAT_POTX;
        else if (html_extension_is(extension, "potm"))
            format = QW_PPTX_FORMAT_POTM;
        else if (html_extension_is(extension, "ppsx"))
            format = QW_PPTX_FORMAT_PPSX;
        else if (html_extension_is(extension, "ppsm"))
            format = QW_PPTX_FORMAT_PPSM;
        return html_from_pptx(format, data, size, out_html, out_size, report);
    }
    if (html_extension_is(extension, "odp") ||
        html_extension_is(extension, "otp") ||
        html_extension_is(extension, "fodp")) {
        int format = QW_ODP_FORMAT_ODP;
        if (html_extension_is(extension, "otp")) format = QW_ODP_FORMAT_OTP;
        else if (html_extension_is(extension, "fodp"))
            format = QW_ODP_FORMAT_FODP;
        return html_from_odp(format, data, size, out_html, out_size, report);
    }
    if (html_extension_is(extension, "hwpx"))
        return html_from_hwpx(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "eml"))
        return html_from_email(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "fb2"))
        return html_from_fb2(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "htmlz"))
        return html_from_htmlz(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "epub") ||
        html_extension_is(extension, "kepub"))
        return html_from_epub(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "mobi") ||
        html_extension_is(extension, "azw3") ||
        html_extension_is(extension, "azw"))
        return html_from_mobi(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "rtf"))
        return html_from_rtf(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "md") ||
        html_extension_is(extension, "markdown") ||
        html_extension_is(extension, "mdown"))
        return html_from_markdown(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "mdx"))
        return html_from_mdx(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "mdd"))
        return html_fail(report, QW_STATUS_ERR_UNSUPPORTED, "layout HTML",
                         "MDD is a resource container; convert the companion "
                         "MDX");
    if (html_extension_is(extension, "csv") ||
        html_extension_is(extension, "tsv") ||
        html_extension_is(extension, "xls") ||
        html_extension_is(extension, "xlt") ||
        html_extension_is(extension, "xlsx") ||
        html_extension_is(extension, "xlsm") ||
        html_extension_is(extension, "xltx") ||
        html_extension_is(extension, "ods") ||
        html_extension_is(extension, "ots") ||
        html_extension_is(extension, "fods")) {
        QwSpreadsheetFormat format;
        if (!html_spreadsheet_format(extension, &format))
            return html_fail(report, QW_STATUS_ERR_UNSUPPORTED, "layout HTML",
                             "unsupported worksheet format");
        return html_from_spreadsheet(format, data, size, out_html, out_size,
                                     report);
    }
    if (html_extension_is(extension, "txtz"))
        return html_from_text_archive(QW_TEXT_ARCHIVE_TXTZ, data, size,
                                      out_html, out_size, report);
    if (html_extension_is(extension, "pmlz"))
        return html_from_text_archive(QW_TEXT_ARCHIVE_PMLZ, data, size,
                                      out_html, out_size, report);
    if (html_extension_is(extension, "html") ||
        html_extension_is(extension, "htm") ||
        html_extension_is(extension, "xhtml"))
        return html_copy_bytes(data, size, out_html, out_size);
    if (html_extension_is(extension, "txt") ||
        html_extension_is(extension, "text"))
        return html_from_plain_text(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "pdf"))
        return html_from_pdf_text(data, size, out_html, out_size, report);
    if (html_extension_is(extension, "xps"))
        return html_fail(report, QW_STATUS_ERR_UNSUPPORTED, "layout HTML",
                         "XPS is reader-only; no HTML route");

    /* No extension: detect from content. */
    if (html_region_equals(data, size, 0u, "%PDF-"))
        return html_from_pdf_text(data, size, out_html, out_size, report);
    if (html_region_equals(data, size, 0u, "PK\x03\x04"))
        return html_from_zip(data, size, extension, out_html, out_size, report);
    if (html_region_equals(data, size, 0u, "{\\rtf"))
        return html_from_rtf(data, size, out_html, out_size, report);
    if (html_contains(data, 1024u, "<FictionBook"))
        return html_from_fb2(data, size, out_html, out_size, report);
    if (html_region_equals(data, size, 0u, "<!DOCTYPE") ||
        html_region_equals(data, size, 0u, "<!doctype") ||
        html_contains(data, 1024u, "<html"))
        return html_copy_bytes(data, size, out_html, out_size);
    if (qw_mdx_probe(data, size))
        return html_from_mdx(data, size, out_html, out_size, report);
    if (qw_mobi_probe(data, size))
        return html_from_mobi(data, size, out_html, out_size, report);

    return html_fail(report, QW_STATUS_ERR_UNSUPPORTED, "layout HTML",
                     "no HTML route for this input; pass a file with a known "
                     "extension");
}

int qw_doc_convert_markdown_memory(const uint8_t *data, size_t size,
                                   const char *input_extension,
                                   uint8_t **out_markdown, size_t *out_size,
                                   QwConvertReport *report)
{
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwMarkdownText converted;
    int status;

    if (!out_markdown || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_markdown = NULL;
    *out_size = 0u;
    status = qw_doc_convert_html_memory(data, size, input_extension, &html,
                                        &html_size, report);
    if (status != QW_STATUS_OK) return status;
    memset(&converted, 0, sizeof(converted));
    status = (int)qw_markdown_from_html(html, html_size, &converted);
    free(html);
    if (status != 0 || !converted.markdown) {
        return html_fail(report, html_status_positive(status),
                         "Markdown write", "");
    }
    {
        size_t removed_images = converted.removed_images;

        *out_markdown = (uint8_t *)converted.markdown;
        *out_size = converted.markdown_size;
        converted.markdown = NULL;
        qw_markdown_text_free(&converted);
        if (report) {
            report->format = QW_FORMAT_MARKDOWN;
            report->output_format = QW_FORMAT_MARKDOWN;
            report->output_size = *out_size;
            snprintf(report->note, sizeof(report->note),
                     "layout HTML converted to the supported Markdown subset; "
                     "%zu image(s) reduced to alt text", removed_images);
        }
    }
    return QW_STATUS_OK;
}
