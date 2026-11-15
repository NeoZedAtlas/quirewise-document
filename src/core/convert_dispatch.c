/* Public conversion dispatch for the document package.
 *
 * `qw_convert_to_pdf_memory` / `qw_convert_to_format_memory` route inputs to
 * the internal renderers. Every format named in app/docs/formats that is owned
 * by this package has a branch here; otherwise the entry point returns
 * `QW_STATUS_ERR_UNSUPPORTED` with an explanatory note instead of letting a
 * parser guess.
 *
 *  - `%PDF-` input is passed through unchanged (valid PDF stays valid);
 *  - HTML input is rendered by the internal layout/PDF writer;
 *  - flat ODF (`<?xml ... office:document`) is split into FODT / FODP;
 *  - ZIP containers are identified by their member names (member names are
 *    stored verbatim in the central directory) before any parser runs:
 *    DOCX/DOCM/DOTX/DOTM, ODT/OTT, PPTX family, ODP/OTP, HWPX, HTMLZ, TXTZ,
 *    PMLZ, XLSX family and ODS, plus EPUB (reflow);
 *  - EML, MOBI/AZW3 and plain/markup text use the ebook readers or the text
 *    layout path.
 *
 * Unknown ZIP packages report `QW_STATUS_ERR_UNSUPPORTED`; they are no longer
 * reported as a broken OpenDocument package.
 * No MuPDF code is involved. */

#include <quirewise/convert.h>
#include <quirewise/pdf.h>
#include <quirewise/email.h>
#include <quirewise/hwpx.h>
#include <quirewise/htmlz.h>
#include <quirewise/mdd.h>
#include <quirewise/mdx.h>
#include <quirewise/mobi.h>
#include <quirewise/odf_presentation.h>
#include <quirewise/pml.h>
#include <quirewise/pptx.h>
#include <quirewise/rtf.h>
#include <quirewise/text_archive.h>

#include "format_registry.h"
#include "png_encoder.h"
#include "../backends/document_backend.h"
#include "../formats/odt/odt_format.h"
#include "../formats/docx/docx_format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int convert_region_equals(const uint8_t *data, size_t size, size_t offset,
                                 const char *text)
{
    size_t length = strlen(text);
    if (!data || offset + length > size) return 0;
    return memcmp(data + offset, text, length) == 0;
}

static int contains_text(const uint8_t *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!length || size < length) return 0;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return 1;
    }
    return 0;
}

/* Skip a UTF-8 BOM and leading ASCII whitespace before sniffing text. */
static const uint8_t *convert_skip_preamble(const uint8_t *data, size_t size)
{
    size_t index = 0u;
    if (size >= 3u && data[0] == 0xEFu && data[1] == 0xBBu &&
        data[2] == 0xBFu) {
        index = 3u;
    }
    while (index < size && (data[index] == ' ' || data[index] == '\t' ||
                            data[index] == '\r' || data[index] == '\n')) {
        ++index;
    }
    return data + index;
}

static int pdf_passthrough(const uint8_t *data, size_t size, uint8_t **out_pdf,
                           size_t *out_size, QwConvertReport *report)
{
    uint8_t *copy = (uint8_t *)malloc(size ? size : 1u);
    if (!copy) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (size) memcpy(copy, data, size);
    *out_pdf = copy;
    *out_size = size;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "Input already is a PDF document; passed through unchanged");
    }
    return QW_STATUS_OK;
}

/* One dispatcher for memory and file conversions. `qw_doc_convert_memory`
 * covers every input family this package owns, so the public entry point no
 * longer keeps a second, narrower ZIP/XML sniffer that reported unsupported
 * packages as broken OpenDocument files. */
int qw_doc_convert_memory(const uint8_t *data, size_t size,
                          const char *input_extension, int reflow_epub,
                          uint8_t **out_pdf, size_t *out_size,
                          QwConvertReport *report);

int qw_convert_to_pdf_memory(const void *data, size_t size,
                             const QwConvertOptions *options,
                             uint8_t **out_pdf, size_t *out_size,
                             QwConvertReport *report)
{
    const uint8_t *bytes = (const uint8_t *)data;
    const char *extension = NULL;
    int reflow = 0;

    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (!bytes || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;

    if (convert_region_equals(bytes, size, 0u, "%PDF-")) {
        return pdf_passthrough(bytes, size, out_pdf, out_size, report);
    }
    /* The memory entry point has no file extension; detect the EPUB container
     * so the reflow route is reachable here as it is through the file API. */
    if (convert_region_equals(bytes, size, 0u, "PK\x03\x04") &&
        contains_text(bytes, size, "application/epub+zip")) {
        extension = "epub";
        reflow = 1;
    }
    return qw_doc_convert_memory_with_options(bytes, size, extension, reflow,
                                              options, out_pdf, out_size,
                                              report);
}

/* Map the historical MuPDF "convert memory" entry onto the internal writers. */

#include <quirewise/epub.h>
#include <quirewise/fb2.h>
#include <quirewise/spreadsheet.h>

#define QW_EPUB_HTML_LIMIT (8u * 1024u * 1024u)

static int epub_html_append(char **buffer, size_t *size, size_t *capacity,
                            const char *text, size_t length)
{
    char *grown;
    size_t needed = *size + length;
    if (needed < *size || needed > QW_EPUB_HTML_LIMIT) return 0;
    if (needed + 1u > *capacity) {
        size_t next = *capacity ? *capacity : 1024u;
        while (next < needed + 1u) {
            if (next > QW_EPUB_HTML_LIMIT / 2u) { next = QW_EPUB_HTML_LIMIT; break; }
            next *= 2u;
        }
        if (next < needed + 1u) return 0;
        grown = (char *)realloc(*buffer, next);
        if (!grown) return 0;
        *buffer = grown;
        *capacity = next;
    }
    if (length) memcpy(*buffer + *size, text, length);
    *size = needed;
    (*buffer)[*size] = '\0';
    return 1;
}

static int epub_html_escape(char **buffer, size_t *size, size_t *capacity,
                            const char *text, size_t length)
{
    size_t i;
    for (i = 0u; i < length; ++i) {
        const char *replacement = NULL;
        switch (text[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        default: break;
        }
        if (replacement) {
            if (!epub_html_append(buffer, size, capacity, replacement,
                                  strlen(replacement))) return 0;
        } else if (!epub_html_append(buffer, size, capacity, text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

static int epub_to_html_internal(const uint8_t *data, size_t size,
                                 char **out_html, size_t *out_size,
                                 QwConvertReport *report)
{
    QwEpubDocument *document = NULL;
    QwEpubStatus status;
    char *html = NULL;
    size_t length = 0u;
    size_t capacity = 0u;
    size_t section_index;
    size_t section_count;
    int ok = 0;
    status = qw_epub_parse(data, size, &document);
    if (status != QW_EPUB_OK || !document) {
        int mapped;
        const char *reason;
        switch (status) {
        case QW_EPUB_ERR_UNSUPPORTED:
            mapped = QW_STATUS_ERR_UNSUPPORTED;
            reason = "EPUB contains layout features the internal renderer does not support";
            break;
        case QW_EPUB_ERR_RESOURCE_LIMIT:
            mapped = QW_STATUS_ERR_RESOURCE_LIMIT;
            reason = "EPUB exceeds the structured layout limits";
            break;
        case QW_EPUB_ERR_OUT_OF_MEMORY:
            mapped = QW_STATUS_ERR_OUT_OF_MEMORY;
            reason = "out of memory while parsing the EPUB package";
            break;
        case QW_EPUB_ERR_DECODE:
            mapped = QW_STATUS_ERR_DECODE;
            reason = "EPUB content could not be decoded";
            break;
        case QW_EPUB_ERR_INVALID_ARGUMENT:
            mapped = QW_STATUS_ERR_INVALID_ARGUMENT;
            reason = "invalid EPUB input";
            break;
        default:
            mapped = QW_STATUS_ERR_FORMAT;
            reason = "EPUB package is invalid";
            break;
        }
        if (report) snprintf(report->note, sizeof(report->note),
                             "EPUB parse failed: %s", reason);
        return mapped;
    }
    section_count = qw_epub_section_count(document);
    ok = epub_html_append(&html, &length, &capacity,
                          "<html><body>", strlen("<html><body>"));
    for (section_index = 0u; ok && section_index < section_count;
         ++section_index) {
        QwEpubSectionView section;
        size_t paragraph_index;
        memset(&section, 0, sizeof(section));
        if (qw_epub_section_at(document, section_index, &section) !=
            QW_EPUB_OK) {
            ok = 0;
            break;
        }
        if (section.title && section.title_size) {
            ok = epub_html_append(&html, &length, &capacity, "<h1>", 4u) &&
                 epub_html_escape(&html, &length, &capacity, section.title,
                                  section.title_size) &&
                 epub_html_append(&html, &length, &capacity, "</h1>", 5u);
        }
        for (paragraph_index = 0u;
             ok && paragraph_index < section.paragraph_count;
             ++paragraph_index) {
            QwEpubParagraphView paragraph;
            size_t run_index;
            char tag[8];
            memset(&paragraph, 0, sizeof(paragraph));
            if (qw_epub_paragraph_at(document, section_index, paragraph_index,
                                     &paragraph) != QW_EPUB_OK) {
                ok = 0;
                break;
            }
            if (paragraph.page_break_before) {
                ok = epub_html_append(&html, &length, &capacity,
                                      "<div style=\"page-break-before:always\"></div>",
                                      strlen("<div style=\"page-break-before:always\"></div>"));
            }
            if (!ok) break;
            if (paragraph.heading_level) {
                snprintf(tag, sizeof(tag), "<h%u>",
                         paragraph.heading_level > 6u ? 6u
                                                       : paragraph.heading_level);
                ok = epub_html_append(&html, &length, &capacity, tag,
                                      strlen(tag));
            } else {
                ok = epub_html_append(&html, &length, &capacity, "<p>", 3u);
            }
            for (run_index = 0u; ok && run_index < paragraph.run_count;
                 ++run_index) {
                QwEpubRunView run;
                memset(&run, 0, sizeof(run));
                if (qw_epub_run_at(document, section_index, paragraph_index,
                                   run_index, &run) != QW_EPUB_OK) {
                    ok = 0;
                    break;
                }
                if (run.link_target && run.link_target_size)
                    ok = epub_html_append(&html, &length, &capacity,
                                          "<a href=\"", strlen("<a href=\"")) &&
                         epub_html_escape(&html, &length, &capacity,
                                          run.link_target,
                                          run.link_target_size) &&
                         epub_html_append(&html, &length, &capacity, "\">", 2u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_BOLD))
                    ok = epub_html_append(&html, &length, &capacity, "<b>", 3u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_ITALIC))
                    ok = epub_html_append(&html, &length, &capacity, "<i>", 3u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_CODE))
                    ok = epub_html_append(&html, &length, &capacity, "<code>", 6u);
                if (ok && run.utf8_text && run.text_size)
                    ok = epub_html_escape(&html, &length, &capacity,
                                          (const char *)run.utf8_text,
                                          run.text_size);
                if (ok && (run.style_flags & QW_EPUB_STYLE_CODE))
                    ok = epub_html_append(&html, &length, &capacity, "</code>", 7u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_ITALIC))
                    ok = epub_html_append(&html, &length, &capacity, "</i>", 4u);
                if (ok && (run.style_flags & QW_EPUB_STYLE_BOLD))
                    ok = epub_html_append(&html, &length, &capacity, "</b>", 4u);
                if (ok && run.link_target && run.link_target_size)
                    ok = epub_html_append(&html, &length, &capacity, "</a>", 4u);
            }
            if (ok && paragraph.heading_level) {
                snprintf(tag, sizeof(tag), "</h%u>",
                         paragraph.heading_level > 6u ? 6u
                                                       : paragraph.heading_level);
                ok = epub_html_append(&html, &length, &capacity, tag,
                                      strlen(tag));
            } else if (ok) {
                ok = epub_html_append(&html, &length, &capacity, "</p>", 4u);
            }
        }
    }
    if (ok)
        ok = epub_html_append(&html, &length, &capacity, "</body></html>",
                              strlen("</body></html>"));
    qw_epub_document_free(document);
    if (!ok) {
        free(html);
        if (report) snprintf(report->note, sizeof(report->note),
                             "EPUB reflow hit the HTML size limit");
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    *out_html = html;
    *out_size = length;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "EPUB reflow: text/styles rendered; images and CSS approximated "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT|QW_ROUTE_LOSS_FORMAT_DEPENDENT)");
    }
    return QW_STATUS_OK;
}


#define CONVERT_HTML_LIMIT (64u * 1024u * 1024u)

static int csv_html_append(char **buffer, size_t *size, size_t *capacity,
                           const char *text, size_t length)
{
    char *grown;
    size_t needed = *size + length;
    if (needed < *size || needed >= CONVERT_HTML_LIMIT) return 0;
    if (needed + 1u > *capacity) {
        size_t next = *capacity ? *capacity : 1024u;
        while (next < needed + 1u) {
            if (next > CONVERT_HTML_LIMIT / 2u) {
                next = CONVERT_HTML_LIMIT;
                break;
            }
            next *= 2u;
        }
        if (next < needed + 1u) return 0;
        grown = (char *)realloc(*buffer, next);
        if (!grown) return 0;
        *buffer = grown;
        *capacity = next;
    }
    if (length) memcpy(*buffer + *size, text, length);
    *size = needed;
    (*buffer)[*size] = '\0';
    return 1;
}

static int csv_html_escape(char **buffer, size_t *size, size_t *capacity,
                           const uint8_t *text, size_t length)
{
    size_t i;
    for (i = 0u; i < length; ++i) {
        const char *replacement = NULL;
        switch (text[i]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '"': replacement = "&quot;"; break;
        default: break;
        }
        if (replacement) {
            if (!csv_html_append(buffer, size, capacity, replacement,
                                 strlen(replacement))) return 0;
        } else if (!csv_html_append(buffer, size, capacity, (const char *)text + i,
                                    1u)) {
            return 0;
        }
    }
    return 1;
}

/* Status-family mappers are defined after the generic HTML helpers; declare
 * them here because the FB2/CSV adapters below need them first. */
static int convert_map_standard_status(int status);
static int convert_map_compact_status(int status);
static int convert_map_rtf_status(int status);
static int convert_map_archive_status(int status);
static int convert_map_pml_status(int status);

static int fb2_to_pdf_internal(const uint8_t *data, size_t size,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    QwFB2Document *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    int status;
    status = qw_fb2_parse(data, size, &document);
    if (status != QW_FB2_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "FB2 parse failed with status %d", status);
        return convert_map_standard_status((int)status);
    }
    html = qw_fb2_document_html(document, &html_size);
    if (!html || !html_size) {
        qw_fb2_document_free(document);
        return QW_STATUS_ERR_FORMAT;
    }
    status = qw_doc_convert_html((const char *)html, html_size, out_pdf,
                                   out_size, report);
    qw_fb2_document_free(document);
    if (status == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "FB2 layout: text rendered; images and CSS approximated "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT|QW_ROUTE_LOSS_FORMAT_DEPENDENT)");
    }
    return status;
}

static int csv_to_pdf_internal(const uint8_t *data, size_t size,
                               QwSpreadsheetFormat format,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    QwSpreadsheetDocument *document = NULL;
    const QwSpreadsheetSheet *sheet;
    size_t cell_count, index;
    size_t current_row = (size_t)-1;
    char *html = NULL;
    size_t html_size = 0u, capacity = 0u;
    int status;
    status = qw_spreadsheet_parse(format, data, size, &document);
    if (status != QW_SPREADSHEET_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "Spreadsheet parse failed with status %d", status);
        return convert_map_compact_status((int)status);
    }
    if (qw_spreadsheet_sheet_count(document) == 0u) {
        qw_spreadsheet_document_free(document);
        return QW_STATUS_ERR_FORMAT;
    }
    sheet = qw_spreadsheet_sheet_at(document, 0u);
    cell_count = sheet ? qw_spreadsheet_cell_count(sheet) : 0u;
    if (!csv_html_append(&html, &html_size, &capacity, "<html><body><table>",
                         strlen("<html><body><table>"))) {
        qw_spreadsheet_document_free(document);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    for (index = 0u; index < cell_count; ++index) {
        QwSpreadsheetCellView cell;
        memset(&cell, 0, sizeof(cell));
        if (qw_spreadsheet_cell_at(sheet, index, &cell) != QW_SPREADSHEET_OK)
            continue;
        if (cell.row != current_row) {
            if (current_row != (size_t)-1)
                if (!csv_html_append(&html, &html_size, &capacity, "</tr>", 5u))
                    break;
            if (!csv_html_append(&html, &html_size, &capacity, "<tr>", 4u))
                break;
            current_row = cell.row;
        }
        if (!csv_html_append(&html, &html_size, &capacity, "<td>", 4u) ||
            !csv_html_escape(&html, &html_size, &capacity, cell.value,
                             cell.value ? cell.value_size : 0u) ||
            !csv_html_append(&html, &html_size, &capacity, "</td>", 5u))
            break;
    }
    if (cell_count && current_row != (size_t)-1)
        (void)csv_html_append(&html, &html_size, &capacity, "</tr>", 5u);
    (void)csv_html_append(&html, &html_size, &capacity, "</table></body></html>",
                          strlen("</table></body></html>"));
    qw_spreadsheet_document_free(document);
    if (!html) return QW_STATUS_ERR_OUT_OF_MEMORY;
    status = qw_doc_convert_html(html, html_size, out_pdf, out_size, report);
    free(html);
    if (status == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "CSV layout: cells rendered as an HTML table; styles omitted "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT|QW_ROUTE_LOSS_FORMAT_DEPENDENT)");
    }
    return status;
}

static int convert_text_extension(const char *extension)
{
    return strstr(extension, "txt") || strstr(extension, "text") ||
           strstr(extension, "md") || strstr(extension, "markdown") ||
           strstr(extension, "markdn");
}

static QwSpreadsheetFormat convert_spreadsheet_format(const char *extension,
                                                      int *recognized)
{
    *recognized = 1;
    if (strstr(extension, "xlsx")) return QW_SPREADSHEET_FORMAT_XLSX;
    if (strstr(extension, "xlsm")) return QW_SPREADSHEET_FORMAT_XLSM;
    if (strstr(extension, "xltx")) return QW_SPREADSHEET_FORMAT_XLTX;
    if (strstr(extension, "xlt")) return QW_SPREADSHEET_FORMAT_XLT;
    if (strstr(extension, "xls")) return QW_SPREADSHEET_FORMAT_XLS;
    if (strstr(extension, "fods")) return QW_SPREADSHEET_FORMAT_FODS;
    if (strstr(extension, "ots")) return QW_SPREADSHEET_FORMAT_OTS;
    if (strstr(extension, "ods")) return QW_SPREADSHEET_FORMAT_ODS;
    *recognized = 0;
    return QW_SPREADSHEET_FORMAT_INVALID;
}

static int convert_text_to_pdf(const uint8_t *data, size_t size,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    const uint8_t *text = data;
    size_t text_size = size;
    int rc;
    if (size >= 3u && data[0] == 0xEFu && data[1] == 0xBBu &&
        data[2] == 0xBFu) {
        text = data + 3u;
        text_size = size - 3u;
    }
    rc = qw_native_text_pdf_from_utf8(text, text_size, out_pdf, out_size);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "plain text rendered as paragraphs "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT)");
    }
    return rc;
}

static int convert_spreadsheet_to_pdf(const uint8_t *data, size_t size,
                                      QwSpreadsheetFormat format,
                                      uint8_t **out_pdf, size_t *out_size,
                                      QwConvertReport *report)
{
    QwSpreadsheetDocument *document = NULL;
    char *html = NULL;
    size_t html_size = 0u;
    size_t capacity = 0u;
    size_t sheet_count;
    size_t sheet_index;
    int status;

    status = qw_spreadsheet_parse(format, data, size, &document);
    if (status != QW_SPREADSHEET_OK || !document) {
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "spreadsheet parse failed with status %d", status);
        }
        return convert_map_compact_status((int)status);
    }
    sheet_count = qw_spreadsheet_sheet_count(document);
    if (sheet_count == 0u) {
        qw_spreadsheet_document_free(document);
        return QW_STATUS_ERR_FORMAT;
    }
    if (!csv_html_append(&html, &html_size, &capacity, "<html><body>",
                         strlen("<html><body>"))) {
        qw_spreadsheet_document_free(document);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    for (sheet_index = 0u; sheet_index < sheet_count; ++sheet_index) {
        const QwSpreadsheetSheet *sheet =
            qw_spreadsheet_sheet_at(document, sheet_index);
        size_t name_size = 0u;
        const char *name;
        size_t cell_count;
        size_t cell_index;
        size_t current_row = (size_t)-1;
        size_t current_column = 0u;

        if (!sheet) continue;
        if (sheet_index > 0u &&
            !csv_html_append(&html, &html_size, &capacity,
                             "<div style=\"page-break-before:always\"></div>",
                             strlen("<div style=\"page-break-before:always\"></div>")))
            break;
        name = qw_spreadsheet_sheet_name(sheet, &name_size);
        if (name && name_size) {
            if (!csv_html_append(&html, &html_size, &capacity, "<h1>", 4u) ||
                !csv_html_escape(&html, &html_size, &capacity,
                                 (const uint8_t *)name, name_size) ||
                !csv_html_append(&html, &html_size, &capacity, "</h1>", 5u))
                break;
        }
        if (!csv_html_append(&html, &html_size, &capacity, "<table>",
                             strlen("<table>")))
            break;
        cell_count = qw_spreadsheet_cell_count(sheet);
        for (cell_index = 0u; cell_index < cell_count; ++cell_index) {
            QwSpreadsheetCellView cell;
            memset(&cell, 0, sizeof(cell));
            if (qw_spreadsheet_cell_at(sheet, cell_index, &cell) !=
                QW_SPREADSHEET_OK)
                continue;
            if (cell.row != current_row) {
                if (current_row != (size_t)-1 &&
                    !csv_html_append(&html, &html_size, &capacity, "</tr>", 5u))
                    break;
                if (!csv_html_append(&html, &html_size, &capacity, "<tr>", 4u))
                    break;
                current_row = cell.row;
                current_column = 0u;
            }
            while (current_column < cell.column) {
                if (!csv_html_append(&html, &html_size, &capacity,
                                     "<td></td>", 9u))
                    break;
                ++current_column;
            }
            if (current_column != cell.column) break;
            if (!csv_html_append(&html, &html_size, &capacity, "<td>", 4u) ||
                (cell.value && cell.value_size &&
                 !csv_html_escape(&html, &html_size, &capacity, cell.value,
                                  cell.value_size)) ||
                !csv_html_append(&html, &html_size, &capacity, "</td>", 5u))
                break;
            ++current_column;
        }
        if (current_row != (size_t)-1 &&
            !csv_html_append(&html, &html_size, &capacity, "</tr>", 5u))
            break;
        if (!csv_html_append(&html, &html_size, &capacity, "</table>",
                             strlen("</table>")))
            break;
    }
    if (!csv_html_append(&html, &html_size, &capacity, "</body></html>",
                         strlen("</body></html>"))) {
        qw_spreadsheet_document_free(document);
        free(html);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    qw_spreadsheet_document_free(document);
    status = qw_doc_convert_html(html, html_size, out_pdf, out_size, report);
    free(html);
    if (status == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "spreadsheet layout: %zu sheet(s) rendered as tables "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT)", sheet_count);
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* Layout and ebook adapter routes                                    */
/*                                                                    */
/* The format packages expose typed readers that produce layout HTML  */
/* or plain text. `qw_doc_convert_memory` used to ignore them, so the  */
/* advertised PPTX/ODP/HWPX/EML/HTMLZ/TXTZ/PMLZ/MOBI routes were only  */
/* reachable through the per-format APIs. These adapters connect them  */
/* to the shared file conversion entry point.                          */
/* ------------------------------------------------------------------ */


/* SVG is XML but belongs to the vector package; detect it before the generic
 * XML/ODT branch so it is not reported as a broken OpenDocument file. */
static int convert_looks_like_svg(const uint8_t *data, size_t size)
{
    const uint8_t *first = convert_skip_preamble(data, size);
    const uint8_t *limit = data + size;
    if (first + 5u <= limit && memcmp(first, "<?xml", 5u) == 0) {
        const uint8_t *cursor = first + 5u;
        const uint8_t *after = NULL;
        while (cursor + 1u < limit) {
            if (cursor[0] == '?' && cursor[1] == '>') {
                after = cursor + 2u;
                break;
            }
            ++cursor;
        }
        if (!after) return 0;
        first = convert_skip_preamble(after, (size_t)(limit - after));
    }
    return first + 4u <= limit && memcmp(first, "<svg", 4u) == 0;
}

static int convert_extension_contains(const char *extension, const char *name)
{
    return extension && extension[0] && strstr(extension, name) != NULL;
}

/* TTML is XML but belongs to the subtitle package; detect the <tt> root before
 * the generic XML/ODT branch so a TTML file is not reported as a broken
 * OpenDocument text (the previous behaviour for sample.ttml). */
static int convert_looks_like_ttml(const uint8_t *data, size_t size)
{
    const uint8_t *first = convert_skip_preamble(data, size);
    const uint8_t *limit = data + size;
    const uint8_t *name_start;
    const uint8_t *local;
    const uint8_t *cursor;
    size_t name_size;

    if (first + 5u <= limit && memcmp(first, "<?xml", 5u) == 0) {
        const uint8_t *after = NULL;
        const uint8_t *scan = first + 5u;
        while (scan + 1u < limit) {
            if (scan[0] == '?' && scan[1] == '>') {
                after = scan + 2u;
                break;
            }
            ++scan;
        }
        if (!after) return 0;
        first = convert_skip_preamble(after, (size_t)(limit - after));
    }
    if (first + 3u > limit || first[0] != '<' || first[1] == '/' ||
        first[1] == '!') return 0;
    name_start = first + 1u;
    cursor = name_start;
    while (cursor < limit && *cursor != '>' && *cursor != '/' &&
           *cursor != ' ' && *cursor != '\t' && *cursor != '\r' &&
           *cursor != '\n') {
        ++cursor;
    }
    name_size = (size_t)(cursor - name_start);
    local = name_start;
    {
        const uint8_t *scan = name_start;
        for (; scan < cursor; ++scan) {
            if (*scan == ':') local = scan + 1u;
        }
    }
    name_size = (size_t)(cursor - local);
    return name_size == 2u && local[0] == 't' && local[1] == 't';
}

/*
 * The per-format libraries use two status layouts. Adapters must map them,
 * not flatten every failure to FORMAT: "the input is unsupported" and "the
 * input is corrupt" are different outcomes for callers and for --check.
 *
 * standard: -1 invalid, -2 OOM, -4 format, -5 unsupported, -6 decode,
 *           -11 resource limit (EML, FB2, MOBI and QwStatus itself).
 * compact:  -1 invalid, -2 OOM, -3 format, -4 unsupported, -5 resource limit,
 *           -6 decode (spreadsheet, PPTX, ODP, HWPX, EPUB).
 */
static int convert_map_standard_status(int status)
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

static int convert_map_compact_status(int status)
{
    switch (status) {
    case 0: return QW_STATUS_OK;
    case -1: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case -2: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case -3: return QW_STATUS_ERR_FORMAT;
    case -4: return QW_STATUS_ERR_UNSUPPORTED;
    case -5: return QW_STATUS_ERR_RESOURCE_LIMIT;
    case -6: return QW_STATUS_ERR_DECODE;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

/* RTF status values are positive: 1 invalid, 2 OOM, 3 format,
 * 4 unsupported, 5 resource limit, 6 encoding/decode. */
static int convert_map_rtf_status(int status)
{
    switch (status) {
    case 0: return QW_STATUS_OK;
    case 1: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case 2: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case 3: return QW_STATUS_ERR_FORMAT;
    case 4: return QW_STATUS_ERR_UNSUPPORTED;
    case 5: return QW_STATUS_ERR_RESOURCE_LIMIT;
    case 6: return QW_STATUS_ERR_DECODE;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

/* HTMLZ and the text archives: -3 archive, -4 missing member,
 * -5 resource limit. */
static int convert_map_archive_status(int status)
{
    switch (status) {
    case 0: return QW_STATUS_OK;
    case -1: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case -2: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case -3: return QW_STATUS_ERR_FORMAT;
    case -4: return QW_STATUS_ERR_FORMAT;
    case -5: return QW_STATUS_ERR_RESOURCE_LIMIT;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

/* PML: -1 invalid, -2 OOM, -3 resource limit. */
static int convert_map_pml_status(int status)
{
    switch (status) {
    case 0: return QW_STATUS_OK;
    case -1: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case -2: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case -3: return QW_STATUS_ERR_RESOURCE_LIMIT;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

static int convert_html_to_pdf(const uint8_t *html, size_t size,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    if (!html || !size) return QW_STATUS_ERR_FORMAT;
    return qw_doc_convert_html((const char *)html, size, out_pdf, out_size,
                               report);
}

static int convert_pptx_to_pdf(const uint8_t *data, size_t size,
                               const char *extension, uint8_t **out_pdf,
                               size_t *out_size, QwConvertReport *report)
{
    QwPptxFormat format = QW_PPTX_FORMAT_PPTX;
    QwPptxDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    QwPptxStatus status;
    int rc;

    if (convert_extension_contains(extension, "pptm"))
        format = QW_PPTX_FORMAT_PPTM;
    else if (convert_extension_contains(extension, "potx"))
        format = QW_PPTX_FORMAT_POTX;
    else if (convert_extension_contains(extension, "potm"))
        format = QW_PPTX_FORMAT_POTM;
    else if (convert_extension_contains(extension, "ppsx"))
        format = QW_PPTX_FORMAT_PPSX;
    else if (convert_extension_contains(extension, "ppsm"))
        format = QW_PPTX_FORMAT_PPSM;

    status = qw_pptx_parse(format, data, size, &document);
    if (status != QW_PPTX_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "presentation parse failed with status %d",
                             (int)status);
        return convert_map_compact_status((int)status);
    }
    html = qw_pptx_document_html(document, &html_size);
    rc = convert_html_to_pdf(html, html_size, out_pdf, out_size, report);
    qw_pptx_document_free(document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "presentation layout: slide text rendered; shapes, images, "
                 "notes and styles are not preserved");
    }
    return rc;
}

static int convert_odp_to_pdf(const uint8_t *data, size_t size,
                              const char *extension, uint8_t **out_pdf,
                              size_t *out_size, QwConvertReport *report)
{
    QwOdpFormat format = QW_ODP_FORMAT_ODP;
    QwOdpDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    QwOdpStatus status;
    int rc;

    if (convert_extension_contains(extension, "otp"))
        format = QW_ODP_FORMAT_OTP;
    else if (convert_extension_contains(extension, "fodp"))
        format = QW_ODP_FORMAT_FODP;

    status = qw_odp_parse(format, data, size, &document);
    if (status != QW_ODP_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "OpenDocument presentation parse failed with "
                             "status %d", (int)status);
        return convert_map_compact_status((int)status);
    }
    html = qw_odp_document_html(document, &html_size);
    rc = convert_html_to_pdf(html, html_size, out_pdf, out_size, report);
    qw_odp_document_free(document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "OpenDocument presentation layout: slide text rendered; "
                 "shapes, images and styles are not preserved");
    }
    return rc;
}

static int convert_hwpx_to_pdf(const uint8_t *data, size_t size,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    QwHwpxDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    QwHwpxStatus status;
    int rc;

    status = qw_hwpx_parse(data, size, &document);
    if (status != QW_HWPX_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "HWPX parse failed with status %d", (int)status);
        return convert_map_compact_status((int)status);
    }
    html = qw_hwpx_document_html(document, &html_size);
    rc = convert_html_to_pdf(html, html_size, out_pdf, out_size, report);
    qw_hwpx_document_free(document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "HWPX layout: paragraph text and simple tables rendered; "
                 "styles, images and metadata are not preserved");
    }
    return rc;
}

static int convert_email_to_pdf(const uint8_t *data, size_t size,
                                uint8_t **out_pdf, size_t *out_size,
                                QwConvertReport *report)
{
    QwEmailDocument *document = NULL;
    const uint8_t *html;
    size_t html_size = 0u;
    QwEmailStatus status;
    int rc;

    status = qw_email_parse(data, size, &document);
    if (status != QW_EMAIL_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "EML parse failed with status %d", (int)status);
        return convert_map_standard_status((int)status);
    }
    html = qw_email_document_html(document, &html_size);
    rc = convert_html_to_pdf(html, html_size, out_pdf, out_size, report);
    qw_email_document_free(document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "EML layout: selected body rendered; attachments are listed, "
                 "not exported");
    }
    return rc;
}

static int convert_htmlz_to_pdf(const uint8_t *data, size_t size,
                                uint8_t **out_pdf, size_t *out_size,
                                QwConvertReport *report)
{
    QwHtmlzDocument document;
    const char *error = NULL;
    QwHtmlzStatus status;
    int rc;

    memset(&document, 0, sizeof(document));
    status = qw_htmlz_extract_primary_document(data, size, &document, &error);
    if (status != QW_HTMLZ_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "HTMLZ extraction failed: %s",
                             error ? error : qw_htmlz_status_string(status));
        return convert_map_archive_status((int)status);
    }
    rc = convert_html_to_pdf(document.html_bytes, document.html_size, out_pdf,
                             out_size, report);
    qw_htmlz_document_free(&document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "HTMLZ layout: index document rendered; package CSS, images "
                 "and other members are not preserved");
    }
    return rc;
}

static int convert_text_archive_to_pdf(const uint8_t *data, size_t size,
                                       QwTextArchiveKind kind,
                                       uint8_t **out_pdf, size_t *out_size,
                                       QwConvertReport *report)
{
    QwTextArchiveDocument document;
    const char *error = NULL;
    QwTextArchiveStatus status;
    int rc;

    memset(&document, 0, sizeof(document));
    status = qw_text_archive_extract_primary(data, size, kind, &document,
                                             &error);
    if (status != QW_TEXT_ARCHIVE_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "text archive extraction failed: %s",
                             error ? error
                                   : qw_text_archive_status_string(status));
        return convert_map_archive_status((int)status);
    }
    rc = convert_text_to_pdf(document.text_bytes, document.text_size, out_pdf,
                             out_size, report);
    qw_text_archive_document_free(&document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "text archive: primary member rendered as paragraphs; other "
                 "members are not preserved");
    }
    return rc;
}

static int convert_pmlz_to_pdf(const uint8_t *data, size_t size,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    QwTextArchiveDocument archive;
    QwPmlDocument document;
    const char *error = NULL;
    QwTextArchiveStatus archive_status;
    QwPmlStatus pml_status;
    int rc;

    memset(&archive, 0, sizeof(archive));
    archive_status = qw_text_archive_extract_primary(
        data, size, QW_TEXT_ARCHIVE_PMLZ, &archive, &error);
    if (archive_status != QW_TEXT_ARCHIVE_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "PMLZ extraction failed: %s",
                             error ? error
                                   : qw_text_archive_status_string(
                                         archive_status));
        return convert_map_archive_status((int)archive_status);
    }
    memset(&document, 0, sizeof(document));
    pml_status = qw_pml_parse(archive.text_bytes, archive.text_size, &document);
    qw_text_archive_document_free(&archive);
    if (pml_status != QW_PML_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "PML parse failed with status %d",
                             (int)pml_status);
        return convert_map_pml_status((int)pml_status);
    }
    rc = convert_text_to_pdf(document.text_bytes, document.text_size, out_pdf,
                             out_size, report);
    qw_pml_document_free(&document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "PMLZ layout: basic PML commands stripped; fonts and advanced "
                 "layout are not preserved");
    }
    return rc;
}

static int convert_mobi_to_pdf(const uint8_t *data, size_t size,
                               uint8_t **out_pdf, size_t *out_size,
                               QwConvertReport *report)
{
    QwMobiDocument *document = NULL;
    const uint8_t *text;
    size_t text_size = 0u;
    QwMobiStatus status;
    int rc;

    status = qw_mobi_parse(data, size, &document);
    if (status != QW_MOBI_OK || !document) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "MOBI parse failed with status %d", (int)status);
        return convert_map_standard_status((int)status);
    }
    text = qw_mobi_document_text(document, &text_size);
    rc = convert_text_to_pdf(text, text_size, out_pdf, out_size, report);
    qw_mobi_document_free(document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "MOBI/AZW3 layout: PalmDOC text rendered; markup, styles, "
                 "images and DRM-protected content are not preserved");
    }
    return rc;
}

static int convert_mdx_to_pdf(const uint8_t *data, size_t size,
                              uint8_t **out_pdf, size_t *out_size,
                              QwConvertReport *report)
{
    QwMdxDocument *document = NULL;
    const uint8_t *html = NULL;
    size_t html_size = 0u;
    size_t entries = 0u;
    char error[192];
    QwMdxStatus status;
    int rc;

    error[0] = '\0';
    status = qw_mdx_parse(data, size, &document, error, sizeof(error));
    if (status != QW_MDX_OK || !document) {
        if (report) {
            snprintf(report->note, sizeof(report->note), "MDX: %s",
                     error[0] ? error : "dictionary could not be parsed");
        }
        return convert_map_standard_status((int)status);
    }
    status = qw_mdx_document_html(document, &html, &html_size);
    if (status != QW_MDX_OK) {
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "MDX rendering failed with status %d", (int)status);
        }
        qw_mdx_document_free(document);
        return convert_map_standard_status((int)status);
    }
    entries = qw_mdx_document_entry_count(document);
    rc = qw_doc_convert_html((const char *)html, html_size, out_pdf, out_size,
                             report);
    qw_mdx_document_free(document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "MDict dictionary layout: %zu entries rendered as headings "
                 "and articles; MDD image resources and entry links are not "
                 "embedded (QW_ROUTE_LOSS_TEXT_LAYOUT)", entries);
    }
    return rc;
}

static int mdx_route_to_text(const uint8_t *data, size_t size,
                             uint8_t **out_data, size_t *out_size,
                             QwConvertReport *report)
{
    QwMdxDocument *document = NULL;
    const uint8_t *text = NULL;
    size_t text_size = 0u;
    char error[192];
    QwMdxStatus status;
    uint8_t *copy;

    error[0] = '\0';
    status = qw_mdx_parse(data, size, &document, error, sizeof(error));
    if (status != QW_MDX_OK || !document) {
        if (report) {
            snprintf(report->note, sizeof(report->note), "MDX: %s",
                     error[0] ? error : "dictionary could not be parsed");
        }
        return convert_map_standard_status((int)status);
    }
    status = qw_mdx_document_text(document, &text, &text_size);
    if (status != QW_MDX_OK) {
        if (report) {
            snprintf(report->note, sizeof(report->note),
                     "MDX text rendering failed with status %d", (int)status);
        }
        qw_mdx_document_free(document);
        return convert_map_standard_status((int)status);
    }
    copy = (uint8_t *)malloc(text_size ? text_size : 1u);
    if (!copy) {
        qw_mdx_document_free(document);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, text, text_size);
    qw_mdx_document_free(document);
    *out_data = copy;
    *out_size = text_size;
    if (report) {
        report->output_size = text_size;
        snprintf(report->note, sizeof(report->note),
                 "MDict dictionary text: one key line and its tag-stripped "
                 "article per entry; MDD image resources are not extracted "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT)");
    }
    return QW_STATUS_OK;
}

static int convert_rtf_to_pdf(const uint8_t *data, size_t size,
                              uint8_t **out_pdf, size_t *out_size,
                              QwConvertReport *report)
{
    QwRtfDocument document;
    QwRtfStatus status;
    int rc;

    memset(&document, 0, sizeof(document));
    status = qw_rtf_parse_html(data, size, &document);
    if (status != QW_RTF_OK) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "RTF parse failed with status %d", (int)status);
        return convert_map_rtf_status((int)status);
    }
    rc = convert_html_to_pdf((const uint8_t *)document.html, document.html_size,
                             out_pdf, out_size, report);
    qw_rtf_document_free(&document);
    if (rc == QW_STATUS_OK && report) {
        snprintf(report->note, sizeof(report->note),
                 "RTF layout: paragraphs, character styles and inline pictures "
                 "rendered; tables are flattened and colours/headers are not "
                 "preserved");
    }
    return rc;
}
int qw_doc_convert_memory_with_options(const uint8_t *data, size_t size,
                                        const char *input_extension,
                                        int reflow_epub,
                                        const QwConvertOptions *options,
                                        uint8_t **out_pdf,
                                        size_t *out_size,
                                        QwConvertReport *report)
{
    uint8_t *html = NULL;
    size_t html_size = 0u;
    char *docx_html = NULL;
    size_t docx_html_size = 0u;
    QwDocxPageLayout layout;
    int rc = QW_STATUS_OK;
    int is_docx = 0;
    int is_odt = 0;
    int is_html = 0;
    int is_pdf = 0;
    int is_fb2 = 0;
    int is_csv = 0;
    int is_text = 0;
    int is_spreadsheet = 0;
    QwSpreadsheetFormat spreadsheet_format = QW_SPREADSHEET_FORMAT_INVALID;
    const char *extension = input_extension ? input_extension : "";

    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    if (!data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (reflow_epub && convert_region_equals(data, size, 0u, "PK\x03\x04")) {
        char *epub_html = NULL;
        size_t epub_html_size = 0u;
        int epub_status = epub_to_html_internal(data, size, &epub_html,
                                                &epub_html_size, report);
        if (epub_status != QW_STATUS_OK) return epub_status;
        epub_status = qw_doc_convert_html(epub_html, epub_html_size, out_pdf,
                                            out_size, report);
        free(epub_html);
        if (epub_status == QW_STATUS_OK && report) {
            snprintf(report->note, sizeof(report->note),
                     "EPUB reflow: text/styles rendered; images and CSS approximated "
                     "(QW_ROUTE_LOSS_TEXT_LAYOUT|QW_ROUTE_LOSS_FORMAT_DEPENDENT)");
        }
        return epub_status;
    }

    if (convert_region_equals(data, size, 0u, "%PDF-")) is_pdf = 1;
    else if (qw_image_probe(data, size)) {
        QwImage image;
        int status;
        memset(&image, 0, sizeof(image));
        status = qw_image_decode(data, size, &image);
        if (status != QW_IMAGE_OK) {
            if (report) snprintf(report->note, sizeof(report->note),
                                 "image decode failed with status %d", status);
            return status;
        }
        status = qw_doc_image_to_pdf_mode(
            &image, options ? options->page_mode : QW_RASTER_PAGE_MODE_IMAGE,
            out_pdf, out_size, report);
        qw_image_free(&image);
        return status;
    }
    else if (qw_mdx_probe(data, size)) {
        return convert_mdx_to_pdf(data, size, out_pdf, out_size, report);
    }
    else if (qw_mdd_probe(data, size)) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "MDD is the MDict resource container for a "
                             "dictionary; convert the companion MDX and use "
                             "--check to validate this file");
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    else if (convert_looks_like_svg(data, size)) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "SVG input is routed by the vector package, "
                             "not the document package");
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    else if (qw_email_probe(data, size) ||
             convert_extension_contains(extension, "eml")) {
        return convert_email_to_pdf(data, size, out_pdf, out_size, report);
    }
    else if (qw_mobi_probe(data, size) ||
             convert_extension_contains(extension, "mobi") ||
             convert_extension_contains(extension, "azw3") ||
             convert_extension_contains(extension, "azw")) {
        return convert_mobi_to_pdf(data, size, out_pdf, out_size, report);
    }
    else if (convert_extension_contains(extension, "rtf") ||
             convert_region_equals(data, size, 0u, "{\\rtf")) {
        return convert_rtf_to_pdf(data, size, out_pdf, out_size, report);
    }
    else if ((spreadsheet_format = convert_spreadsheet_format(
                  extension, &is_spreadsheet)) !=
             QW_SPREADSHEET_FORMAT_INVALID) {
        /* ZIP-based XLSX/ODS packages are spreadsheets, not DOCX/ODT. */
    } else if (convert_region_equals(data, size, 0u, "PK\x03\x04")) {
        /* Member names are stored verbatim in the ZIP central directory, so a
         * byte search identifies the package family before any parser runs.
         * Unknown packages are reported as unsupported instead of being
         * mistaken for a broken OpenDocument file. */
        if (contains_text(data, size, "application/epub+zip")) {
            char *epub_html = NULL;
            size_t epub_html_size = 0u;
            int epub_status =
                epub_to_html_internal(data, size, &epub_html, &epub_html_size,
                                      report);
            if (epub_status != QW_STATUS_OK) return epub_status;
            epub_status = qw_doc_convert_html(epub_html, epub_html_size, out_pdf,
                                              out_size, report);
            free(epub_html);
            if (epub_status == QW_STATUS_OK && report) {
                snprintf(report->note, sizeof(report->note),
                         "EPUB reflow: text/styles rendered; images and CSS "
                         "approximated");
            }
            return epub_status;
        }
        if (convert_extension_contains(extension, "pptx") ||
            convert_extension_contains(extension, "pptm") ||
            convert_extension_contains(extension, "potx") ||
            convert_extension_contains(extension, "potm") ||
            convert_extension_contains(extension, "ppsx") ||
            convert_extension_contains(extension, "ppsm") ||
            contains_text(data, size, "ppt/presentation.xml")) {
            return convert_pptx_to_pdf(data, size, extension, out_pdf, out_size,
                                       report);
        }
        if (convert_extension_contains(extension, "odp") ||
            convert_extension_contains(extension, "otp") ||
            contains_text(data, size,
                          "application/vnd.oasis.opendocument.presentation")) {
            return convert_odp_to_pdf(data, size, extension, out_pdf, out_size,
                                      report);
        }
        if (convert_extension_contains(extension, "hwpx") ||
            contains_text(data, size, "Contents/section")) {
            return convert_hwpx_to_pdf(data, size, out_pdf, out_size, report);
        }
        if (convert_extension_contains(extension, "htmlz") ||
            contains_text(data, size, "index.html") ||
            contains_text(data, size, "index.htm")) {
            return convert_htmlz_to_pdf(data, size, out_pdf, out_size, report);
        }
        if (convert_extension_contains(extension, "txtz") ||
            contains_text(data, size, "index.txt")) {
            return convert_text_archive_to_pdf(data, size,
                                               QW_TEXT_ARCHIVE_TXTZ, out_pdf,
                                               out_size, report);
        }
        if (convert_extension_contains(extension, "pmlz") ||
            contains_text(data, size, "index.pml")) {
            return convert_pmlz_to_pdf(data, size, out_pdf, out_size, report);
        }
        if (convert_extension_contains(extension, "xlsx") ||
            convert_extension_contains(extension, "xlsm") ||
            convert_extension_contains(extension, "xltx") ||
            contains_text(data, size, "xl/workbook.xml")) {
            spreadsheet_format = QW_SPREADSHEET_FORMAT_XLSX;
            is_spreadsheet = 1;
        } else if (convert_extension_contains(extension, "ods") ||
                   convert_extension_contains(extension, "ots") ||
                   contains_text(
                       data, size,
                       "application/vnd.oasis.opendocument.spreadsheet")) {
            spreadsheet_format = QW_SPREADSHEET_FORMAT_ODS;
            is_spreadsheet = 1;
        } else if (convert_extension_contains(extension, "docx") ||
                   convert_extension_contains(extension, "docm") ||
                   convert_extension_contains(extension, "dotx") ||
                   convert_extension_contains(extension, "dotm") ||
                   qw_docx_probe(data, size) != 0) {
            is_docx = 1;
        } else if (convert_extension_contains(extension, "odt") ||
                   convert_extension_contains(extension, "ott") ||
                   contains_text(data, size,
                                 "application/vnd.oasis.opendocument.text") ||
                   contains_text(data, size, "content.xml")) {
            is_odt = 1;
        } else {
            if (report) snprintf(report->note, sizeof(report->note),
                                 "unrecognized ZIP package: not DOCX/DOCM/DOTX/"
                                 "DOTM, ODT/OTT, PPTX family, ODP/OTP, HWPX, "
                                 "XLSX family, ODS, EPUB, HTMLZ, TXTZ or PMLZ");
            return QW_STATUS_ERR_UNSUPPORTED;
        }
    } else if (strstr(extension, "fb2") ||
               contains_text((const char *)data, size, "<FictionBook")) {
        is_fb2 = 1;
    } else if (strstr(extension, "csv") || strstr(extension, "tsv")) {
        is_csv = 1;
    } else if (convert_extension_contains(extension, "mdx") ||
               convert_extension_contains(extension, "mdd")) {
        /* Both extensions contain "md", so without this guard a broken or
         * legacy dictionary would be laid out as Markdown. */
        if (report) snprintf(report->note, sizeof(report->note),
                             "MDict input without a recognized container "
                             "header; engine 1.2 MDX files and register-code "
                             "encryption are not supported");
        return QW_STATUS_ERR_FORMAT;
    } else if (convert_text_extension(extension)) {
        is_text = 1;
    } else if (convert_region_equals(data, size, 0u, "<?xml") ||
               contains_text((const char *)data, size, "office:document")) {
        /* SVG is an XML document but belongs to quirewise-vector; the
         * application routes it. Do not report it as a broken OpenDocument. */
        if (contains_text((const char *)data, size, "<svg")) {
            if (report) snprintf(report->note, sizeof(report->note),
                                 "SVG input is routed by the vector package, "
                                 "not the document package");
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (convert_looks_like_ttml(data, size)) {
            if (report) snprintf(report->note, sizeof(report->note),
                                 "TTML input is routed by the subtitle package, "
                                 "not the document package");
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        if (contains_text((const char *)data, size, "office:presentation")) {
            return convert_odp_to_pdf(data, size, extension, out_pdf, out_size,
                                      report);
        }
        is_odt = 1;
    } else if (convert_skip_preamble(data, size) < data + size &&
               convert_skip_preamble(data, size)[0] == '<' &&
               (contains_text((const char *)data, size, "<html") ||
                contains_text((const char *)data, size, "<p") ||
                contains_text((const char *)data, size, "<h1"))) {
        is_html = 1;
    } else {
        if (report) snprintf(report->note, sizeof(report->note),
                             "No internal renderer for this input");
        return QW_STATUS_ERR_UNSUPPORTED;
    }

    if (is_fb2) {
        return fb2_to_pdf_internal(data, size, out_pdf, out_size, report);
    }
    if (is_csv) {
        QwSpreadsheetFormat format =
            strstr(extension, "tsv") ? QW_SPREADSHEET_FORMAT_TSV
                                      : QW_SPREADSHEET_FORMAT_CSV;
        return csv_to_pdf_internal(data, size, format, out_pdf, out_size,
                                   report);
    }
    if (is_text) {
        return convert_text_to_pdf(data, size, out_pdf, out_size, report);
    }
    if (is_spreadsheet) {
        return convert_spreadsheet_to_pdf(data, size, spreadsheet_format,
                                          out_pdf, out_size, report);
    }
    if (is_pdf) {
        uint8_t *copy = (uint8_t *)malloc(size);
        if (!copy) return QW_STATUS_ERR_OUT_OF_MEMORY;
        memcpy(copy, data, size);
        *out_pdf = copy;
        *out_size = size;
        if (report) snprintf(report->note, sizeof(report->note),
                             "Input already is a PDF document; passed through");
        return QW_STATUS_OK;
    }
    memset(&layout, 0, sizeof(layout));
    if (is_docx) {
        rc = qw_docx_parse_native_html(data, size, &docx_html, &docx_html_size,
                                       &layout);
        if (rc != QW_STATUS_OK) {
            if (report) snprintf(report->note, sizeof(report->note),
                                 "DOCX layout extraction failed (%d)", rc);
            return rc;
        }
        html = (uint8_t *)docx_html;
        html_size = docx_html_size;
    } else if (is_odt) {
        QwInputFormat format = QW_FORMAT_ODT;
        if (convert_region_equals(data, size, 0u, "<?xml")) format = QW_FORMAT_FODT;
        rc = qw_odt_parse_native_html(data, size, format, &html, &html_size, report);
        if (rc != QW_STATUS_OK) return rc;
    } else if (is_html) {
        const uint8_t *first = convert_skip_preamble(data, size);
        size_t html_length = (size_t)(data + size - first);
        html = (uint8_t *)malloc(html_length ? html_length : 1u);
        if (!html) return QW_STATUS_ERR_OUT_OF_MEMORY;
        memcpy(html, first, html_length);
        html_size = html_length;
    }

    if (!html) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "No layout HTML produced for this input");
        return QW_STATUS_ERR_FORMAT;
    }

    if (layout.width_twips > 0u && layout.height_twips > 0u) {
        float width = (float)layout.width_twips / 20.0f;
        float height = (float)layout.height_twips / 20.0f;
        rc = qw_doc_convert_html_with_page_size((const char *)html, html_size,
                                                  width, height, out_pdf, out_size,
                                                  report);
    } else {
        rc = qw_doc_convert_html((const char *)html, html_size, out_pdf,
                                   out_size, report);
    }
    free(html);
    return rc;
}

int qw_doc_convert_memory(const uint8_t *data, size_t size,
                          const char *input_extension, int reflow_epub,
                          uint8_t **out_pdf, size_t *out_size,
                          QwConvertReport *report)
{
    return qw_doc_convert_memory_with_options(data, size, input_extension,
                                               reflow_epub, NULL, out_pdf,
                                               out_size, report);
}

void qw_buffer_free(void *buffer)
{
    free(buffer);
}

/* PDF input routes that produce something other than PDF. */

#define QW_PDF_ROUTE_DPI 144u

static int pdf_route_to_text(const uint8_t *data, size_t size,
                             uint8_t **out_data, size_t *out_size,
                             QwConvertReport *report)
{
    QwPdfDocument *document = NULL;
    size_t pages = 0u;
    uint8_t *text = NULL;
    size_t text_size = 0u;
    int rc = qw_pdf_open(data, size, &document, &pages, report);
    if (rc != QW_STATUS_OK) return rc;
    rc = qw_pdf_extract_text(document, &text, &text_size, report);
    qw_pdf_close(document);
    if (rc != QW_STATUS_OK) {
        free(text);
        return rc;
    }
    *out_data = text;
    *out_size = text_size;
    if (report) {
        report->pages_written = pages;
        report->output_size = text_size;
        snprintf(report->note, sizeof(report->note),
                 "PDF text extraction: %zu page(s), no OCR "
                 "(QW_ROUTE_LOSS_TEXT_LAYOUT)", pages);
    }
    return QW_STATUS_OK;
}

static int pdf_route_to_png(const uint8_t *data, size_t size,
                            uint8_t **out_data, size_t *out_size,
                            QwConvertReport *report)
{
    QwPdfDocument *document = NULL;
    size_t pages = 0u;
    QwPageImage page;
    int rc;
    memset(&page, 0, sizeof(page));
    rc = qw_pdf_open(data, size, &document, &pages, report);
    if (rc != QW_STATUS_OK) return rc;
    rc = qw_pdf_render_page(document, 0u, QW_PDF_ROUTE_DPI, &page, report);
    if (rc == QW_STATUS_OK) {
        rc = qw_png_encode_page(&page, out_data, out_size);
    }
    qw_pdf_page_free(&page);
    qw_pdf_close(document);
    if (rc == QW_STATUS_OK && report) {
        report->pages_written = 1u;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "PNG output rendered the first of %zu page(s) at %u dpi; "
                 "text uses the built-in approximation font "
                 "(QW_ROUTE_LOSS_RASTERIZATION)",
                 pages, (unsigned)QW_PDF_ROUTE_DPI);
    }
    return rc;
}

int qw_convert_to_format_memory(const void *data, size_t size,
                                QwOutputFormat output_format,
                                const QwConvertOptions *options,
                                uint8_t **out_data, size_t *out_size,
                                QwConvertReport *report)
{
    const uint8_t *bytes = (const uint8_t *)data;
    (void)options;
    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (!bytes || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (report) {
        report->output_format = output_format;
        report->pages_written = 0u;
        report->output_size = 0u;
        report->stage = QW_CONVERT_STAGE_EXECUTE;
    }
    if (convert_region_equals(bytes, size, 0u, "%PDF-")) {
        if (report) report->format = QW_FORMAT_PDF;
        switch (output_format) {
        case QW_FORMAT_PDF:
            return pdf_passthrough(bytes, size, out_data, out_size, report);
        case QW_FORMAT_TXT:
            return pdf_route_to_text(bytes, size, out_data, out_size, report);
        case QW_FORMAT_PNG:
            return pdf_route_to_png(bytes, size, out_data, out_size, report);
        case QW_FORMAT_JPG:
            if (report) {
                snprintf(report->note, sizeof(report->note),
                         "JPEG output needs an encoder this package does not "
                         "ship; request PNG or TXT instead");
            }
            return QW_STATUS_ERR_UNSUPPORTED;
        default:
            if (report) {
                snprintf(report->note, sizeof(report->note),
                         "No PDF output route for the requested format");
            }
            return QW_STATUS_ERR_UNSUPPORTED;
        }
    }
    if (output_format == QW_FORMAT_TXT && qw_mdx_probe(bytes, size)) {
        if (report) report->format = QW_FORMAT_MDX;
        return mdx_route_to_text(bytes, size, out_data, out_size, report);
    }
    if (output_format == QW_FORMAT_PDF) {
        return qw_doc_convert_memory(bytes, size, NULL, 0, out_data, out_size,
                                     report);
    }
    if (output_format == QW_FORMAT_MARKDOWN) {
        return qw_doc_convert_markdown_memory(bytes, size, NULL, out_data,
                                              out_size, report);
    }
    if (output_format == QW_FORMAT_DOCX) {
        return qw_doc_convert_docx_memory(bytes, size, NULL, out_data,
                                          out_size, report);
    }
    if (output_format == QW_FORMAT_EPUB) {
        return qw_doc_convert_epub_memory(bytes, size, NULL, out_data,
                                          out_size, report);
    }
    if (qw_image_probe(bytes, size)) {
        int image_status = qw_doc_convert_image_memory(
            bytes, size, output_format, out_data, out_size, report);
        if (image_status != QW_STATUS_ERR_UNSUPPORTED) return image_status;
    }
    if (output_format == QW_FORMAT_HTML) {
        int html_status = qw_doc_convert_html_memory(
            bytes, size, NULL, out_data, out_size, report);
        if (html_status != QW_STATUS_ERR_UNSUPPORTED) return html_status;
        /* Structured data, subtitles and worksheets have no layout HTML. */
    }
    {
        /* No path here, so the family writers fall back to content sniffing. */
        int family = qw_doc_convert_family_to_format(
            bytes, size, NULL, output_format, options, out_data, out_size,
            report);
        if (family != QW_STATUS_ERR_UNSUPPORTED) return family;
    }
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "Only PDF inputs have a non-PDF output route in this package");
    }
    return QW_STATUS_ERR_UNSUPPORTED;
}
