/* Cross-family conversion for formats whose parsers live in the data,
 * subtitle and spreadsheet packages.
 *
 * The document package owns the public "convert to any format" entry points,
 * so it maps a target format to the owning package's writer, parses the input
 * with that package's public parser and copies the result into the shared
 * malloc-owned buffer contract. PDF and the native text/PNG routes stay in
 * convert_dispatch.c; this unit is only reached for non-PDF targets.
 */

#include <quirewise/convert.h>
#include <quirewise/data.h>
#include <quirewise/spreadsheet.h>
#include <quirewise/subtitle.h>

#include "../backends/document_backend.h"
#include "text_buffer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int family_name_is(const char *extension, const char *name)
{
    return extension && name && strcmp(extension, name) == 0;
}

static int family_data_format(const char *extension, QwDataFormat *out_format)
{
    if (!extension || !extension[0]) return 0;
    if (family_name_is(extension, "json")) *out_format = QW_DATA_FORMAT_JSON;
    else if (family_name_is(extension, "jsonl") ||
             family_name_is(extension, "ndjson"))
        *out_format = QW_DATA_FORMAT_JSONL;
    else if (family_name_is(extension, "yaml") ||
             family_name_is(extension, "yml"))
        *out_format = QW_DATA_FORMAT_YAML;
    else if (family_name_is(extension, "toml")) *out_format = QW_DATA_FORMAT_TOML;
    else if (family_name_is(extension, "vcf") ||
             family_name_is(extension, "vcard"))
        *out_format = QW_DATA_FORMAT_VCARD;
    else if (family_name_is(extension, "xml")) *out_format = QW_DATA_FORMAT_XML;
    else if (family_name_is(extension, "opml")) *out_format = QW_DATA_FORMAT_OPML;
    else if (family_name_is(extension, "ipynb"))
        *out_format = QW_DATA_FORMAT_IPYNB;
    else
        return 0;
    return 1;
}

static int family_subtitle_format(const char *extension,
                                  QwSubtitleInputFormat *out_format)
{
    if (!extension || !extension[0]) return 0;
    if (family_name_is(extension, "srt")) *out_format = QW_SUBTITLE_INPUT_SRT;
    else if (family_name_is(extension, "vtt")) *out_format = QW_SUBTITLE_INPUT_VTT;
    else if (family_name_is(extension, "lrc")) *out_format = QW_SUBTITLE_INPUT_LRC;
    else if (family_name_is(extension, "sub"))
        *out_format = QW_SUBTITLE_INPUT_MICRODVD;
    else if (family_name_is(extension, "ass")) *out_format = QW_SUBTITLE_INPUT_ASS;
    else if (family_name_is(extension, "ssa")) *out_format = QW_SUBTITLE_INPUT_SSA;
    else if (family_name_is(extension, "sami"))
        *out_format = QW_SUBTITLE_INPUT_SAMI;
    else if (family_name_is(extension, "scc")) *out_format = QW_SUBTITLE_INPUT_SCC;
    else if (family_name_is(extension, "ttml") ||
             family_name_is(extension, "dfxp"))
        *out_format = QW_SUBTITLE_INPUT_TTML;
    else
        return 0;
    return 1;
}

static int family_spreadsheet_format(const char *extension,
                                     QwSpreadsheetFormat *out_format)
{
    if (!extension || !extension[0]) return 0;
    if (family_name_is(extension, "csv")) *out_format = QW_SPREADSHEET_FORMAT_CSV;
    else if (family_name_is(extension, "tsv"))
        *out_format = QW_SPREADSHEET_FORMAT_TSV;
    else if (family_name_is(extension, "xls")) *out_format = QW_SPREADSHEET_FORMAT_XLS;
    else if (family_name_is(extension, "xlt")) *out_format = QW_SPREADSHEET_FORMAT_XLT;
    else if (family_name_is(extension, "xlsx"))
        *out_format = QW_SPREADSHEET_FORMAT_XLSX;
    else if (family_name_is(extension, "xlsm"))
        *out_format = QW_SPREADSHEET_FORMAT_XLSM;
    else if (family_name_is(extension, "xltx"))
        *out_format = QW_SPREADSHEET_FORMAT_XLTX;
    else if (family_name_is(extension, "ods")) *out_format = QW_SPREADSHEET_FORMAT_ODS;
    else if (family_name_is(extension, "ots")) *out_format = QW_SPREADSHEET_FORMAT_OTS;
    else if (family_name_is(extension, "fods"))
        *out_format = QW_SPREADSHEET_FORMAT_FODS;
    else
        return 0;
    return 1;
}

static int family_data_target(QwOutputFormat format)
{
    switch (format) {
    case QW_FORMAT_JSON:
    case QW_FORMAT_JSONL:
    case QW_FORMAT_YAML:
    case QW_FORMAT_TOML:
    case QW_FORMAT_VCF:
    case QW_FORMAT_XML:
        return 1;
    default:
        return 0;
    }
}

static int family_subtitle_target(QwOutputFormat format)
{
    switch (format) {
    case QW_FORMAT_SRT:
    case QW_FORMAT_VTT:
    case QW_FORMAT_LRC:
    case QW_FORMAT_SUB:
    case QW_FORMAT_ASS:
    case QW_FORMAT_SSA:
    case QW_FORMAT_SAMI:
    case QW_FORMAT_SCC:
    case QW_FORMAT_TTML:
        return 1;
    default:
        return 0;
    }
}

static int family_spreadsheet_target(QwOutputFormat format)
{
    return format == QW_FORMAT_CSV || format == QW_FORMAT_TSV ||
           format == QW_FORMAT_XLSX;
}

static int family_status_from_data(QwDataStatus status)
{
    switch (status) {
    case QW_DATA_OK: return QW_STATUS_OK;
    case QW_DATA_ERR_INVALID_ARGUMENT: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case QW_DATA_ERR_OUT_OF_MEMORY: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case QW_DATA_ERR_FORMAT: return QW_STATUS_ERR_FORMAT;
    case QW_DATA_ERR_UNSUPPORTED: return QW_STATUS_ERR_UNSUPPORTED;
    case QW_DATA_ERR_RESOURCE_LIMIT: return QW_STATUS_ERR_RESOURCE_LIMIT;
    case QW_DATA_ERR_TYPE: return QW_STATUS_ERR_FORMAT;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

static int family_status_from_subtitle(QwSubtitleStatus status)
{
    switch (status) {
    case QW_SUBTITLE_OK: return QW_STATUS_OK;
    case QW_SUBTITLE_INVALID_ARGUMENT: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case QW_SUBTITLE_UNSUPPORTED: return QW_STATUS_ERR_UNSUPPORTED;
    case QW_SUBTITLE_FORMAT_ERROR: return QW_STATUS_ERR_FORMAT;
    case QW_SUBTITLE_DECODE_ERROR: return QW_STATUS_ERR_DECODE;
    case QW_SUBTITLE_OUT_OF_MEMORY: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case QW_SUBTITLE_RESOURCE_LIMIT: return QW_STATUS_ERR_RESOURCE_LIMIT;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

static int family_status_from_spreadsheet(QwSpreadsheetStatus status)
{
    switch (status) {
    case QW_SPREADSHEET_OK: return QW_STATUS_OK;
    case QW_SPREADSHEET_ERR_INVALID_ARGUMENT:
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    case QW_SPREADSHEET_ERR_OUT_OF_MEMORY: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case QW_SPREADSHEET_ERR_FORMAT: return QW_STATUS_ERR_FORMAT;
    case QW_SPREADSHEET_ERR_UNSUPPORTED: return QW_STATUS_ERR_UNSUPPORTED;
    case QW_SPREADSHEET_ERR_RESOURCE_LIMIT: return QW_STATUS_ERR_RESOURCE_LIMIT;
    case QW_SPREADSHEET_ERR_DECODE: return QW_STATUS_ERR_DECODE;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

static int family_fail(QwConvertReport *report, int status, const char *what,
                       const char *detail)
{
    if (report) {
        /* The owning writer often explains the refusal better than this
         * dispatcher can; only fill a note it left empty. */
        if (!report->note[0]) {
            snprintf(report->note, sizeof(report->note), "%s: %s", what,
                     detail && detail[0] ? detail : "conversion failed");
        }
    }
    return status;
}

static int family_convert_data(const uint8_t *data, size_t size,
                               const char *extension,
                               QwOutputFormat output_format,
                               const QwConvertOptions *options,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report)
{
    QwDataFormat format = QW_DATA_FORMAT_INVALID;
    QwDataDocument *document = NULL;
    QwDataStatus status;
    int rc;

    document = NULL;
    if (family_data_format(extension, &format)) {
        status = qw_data_parse(format, data, size, &document);
        if (status != QW_DATA_OK) {
            return family_fail(report, family_status_from_data(status),
                               "data parse", "");
        }
    } else if (!extension || !extension[0]) {
        /* Memory API without a path: sniff the unambiguous roots. */
        const uint8_t *cursor = data;
        size_t remaining = size;
        while (remaining && (*cursor == ' ' || *cursor == '\t' ||
                             *cursor == '\r' || *cursor == '\n')) {
            ++cursor;
            --remaining;
        }
        if (remaining >= 14u && memcmp(cursor, "BEGIN:VCARD", 11u) == 0)
            status = qw_data_parse(QW_DATA_FORMAT_VCARD, data, size, &document);
        else if (remaining && (*cursor == '{' || *cursor == '['))
            status = qw_data_parse(QW_DATA_FORMAT_JSON, data, size, &document);
        else if (remaining && *cursor == '<')
            status = qw_data_parse(QW_DATA_FORMAT_XML, data, size, &document);
        else
            return family_fail(report, QW_STATUS_ERR_UNSUPPORTED,
                               "data input", "unknown structured-data input");
        if (status != QW_DATA_OK)
            return family_fail(report, family_status_from_data(status),
                               "data parse", "");
    } else {
        return family_fail(report, QW_STATUS_ERR_UNSUPPORTED, "data input",
                           "input is not a structured data file");
    }
    rc = family_status_from_data(
        qw_data_write_document(document, output_format, options, out_data,
                               out_size, report));
    if (rc != QW_STATUS_OK)
        family_fail(report, rc, "data write", "target format not writable");
    qw_data_document_free(document);
    return rc;
}

static int family_sniff_subtitle(const uint8_t *data, size_t size,
                                 QwSubtitleInputFormat *out_format)
{
    const uint8_t *cursor = data;
    size_t remaining = size;
    size_t index;

    while (remaining >= 3u && cursor[0] == 0xefu && cursor[1] == 0xbbu &&
           cursor[2] == 0xbfu) {
        cursor += 3u;
        remaining -= 3u;
    }
    while (remaining && (*cursor == ' ' || *cursor == '\t' ||
                         *cursor == '\r' || *cursor == '\n')) {
        ++cursor;
        --remaining;
    }
    if (remaining >= 6u && memcmp(cursor, "WEBVTT", 6u) == 0) {
        *out_format = QW_SUBTITLE_INPUT_VTT;
        return 1;
    }
    if (remaining >= 13u && memcmp(cursor, "[Script Info]", 13u) == 0) {
        *out_format = QW_SUBTITLE_INPUT_ASS;
        return 1;
    }
    if (remaining >= 13u && memcmp(cursor, "Scenarist_SCC", 13u) == 0) {
        *out_format = QW_SUBTITLE_INPUT_SCC;
        return 1;
    }
    if (remaining >= 5u && memcmp(cursor, "<SAMI", 5u) == 0) {
        *out_format = QW_SUBTITLE_INPUT_SAMI;
        return 1;
    }
    for (index = 0u; index + 5u <= size; ++index) {
        if (memcmp(data + index, " --> ", 5u) == 0) {
            *out_format = QW_SUBTITLE_INPUT_SRT;
            return 1;
        }
    }
    return 0;
}

static int family_convert_subtitle(const uint8_t *data, size_t size,
                                   const char *extension,
                                   QwOutputFormat output_format,
                                   const QwConvertOptions *options,
                                   uint8_t **out_data, size_t *out_size,
                                   QwConvertReport *report)
{
    QwSubtitleInputFormat format = (QwSubtitleInputFormat)0;
    QwSubtitle *document = NULL;
    QwSubtitleStatus status;
    int rc;

    if (!family_subtitle_format(extension, &format)) {
        if (!extension || !extension[0]) {
            if (!family_sniff_subtitle(data, size, &format)) {
                return family_fail(report, QW_STATUS_ERR_UNSUPPORTED,
                                   "subtitle input",
                                   "input is not a subtitle file");
            }
        } else {
            return family_fail(report, QW_STATUS_ERR_UNSUPPORTED,
                               "subtitle input",
                               "input is not a subtitle file");
        }
    }
    status = qw_subtitle_parse(data, size, format, &document);
    if (status != QW_SUBTITLE_OK) {
        return family_fail(report, family_status_from_subtitle(status),
                           "subtitle parse", "");
    }
    rc = family_status_from_subtitle(qw_subtitle_write(
        document, output_format, options, out_data, out_size, report));
    if (rc != QW_STATUS_OK)
        family_fail(report, rc, "subtitle write", "target format not writable");
    qw_subtitle_free(document);
    return rc;
}

static int family_convert_spreadsheet(const uint8_t *data, size_t size,
                                      const char *extension,
                                      QwOutputFormat output_format,
                                      const QwConvertOptions *options,
                                      uint8_t **out_data, size_t *out_size,
                                      QwConvertReport *report)
{
    QwSpreadsheetFormat format = QW_SPREADSHEET_FORMAT_INVALID;
    QwSpreadsheetDocument *document = NULL;
    QwSpreadsheetStatus status;
    int rc;

    if (!family_spreadsheet_format(extension, &format)) {
        return family_fail(report, QW_STATUS_ERR_UNSUPPORTED,
                           "spreadsheet input",
                           "input is not a spreadsheet file");
    }
    status = qw_spreadsheet_parse(format, data, size, &document);
    if (status != QW_SPREADSHEET_OK) {
        return family_fail(report, family_status_from_spreadsheet(status),
                           "spreadsheet parse", "");
    }
    rc = family_status_from_spreadsheet(qw_spreadsheet_write_document(
        document, output_format, options, out_data, out_size, report));
    if (rc != QW_STATUS_OK)
        family_fail(report, rc, "spreadsheet write",
                    "target format not writable");
    qw_spreadsheet_document_free(document);
    return rc;
}


/* ------------------------------------------------------------------ */
/* Markdown-source exports: IPYNB notebook and OPML outline.          */
/* ------------------------------------------------------------------ */

static int family_export_json_escape(QwTextBuffer *output, const char *text,
                                     size_t size)
{
    size_t index;
    for (index = 0u; index < size; ++index) {
        unsigned char value = (unsigned char)text[index];
        char escaped[8];
        size_t length = 0u;
        switch (value) {
        case '"': memcpy(escaped, "\\\"", 2u); length = 2u; break;
        case '\\': memcpy(escaped, "\\\\", 2u); length = 2u; break;
        case '\n': memcpy(escaped, "\\n", 2u); length = 2u; break;
        case '\r': memcpy(escaped, "\\r", 2u); length = 2u; break;
        case '\t': memcpy(escaped, "\\t", 2u); length = 2u; break;
        default:
            if (value < 0x20u) {
                int written = snprintf(escaped, sizeof(escaped), "\\u%04x",
                                       (unsigned)value);
                if (written <= 0) return 0;
                length = (size_t)written;
            } else {
                escaped[0] = (char)value;
                length = 1u;
            }
            break;
        }
        if (!qw_text_buffer_append(output, escaped, length)) return 0;
    }
    return 1;
}

static int family_export_xml_escape(QwTextBuffer *output, const char *text,
                                    size_t size)
{
    size_t index;
    for (index = 0u; index < size; ++index) {
        const char *entity = NULL;
        switch (text[index]) {
        case '&': entity = "&amp;"; break;
        case '<': entity = "&lt;"; break;
        case '>': entity = "&gt;"; break;
        case '"': entity = "&quot;"; break;
        case '\'': entity = "&apos;"; break;
        default: break;
        }
        if (entity) {
            if (!qw_text_buffer_append(output, entity, strlen(entity)))
                return 0;
        } else if (!qw_text_buffer_append(output, text + index, 1u)) {
            return 0;
        }
    }
    return 1;
}

static int family_export_ipynb(const uint8_t *data, size_t size,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report)
{
    QwTextBuffer output = {0};
    size_t cursor = 0u;
    int first = 1;

    output.limit = 128u * 1024u * 1024u;
    if (memchr(data, 0, size) != NULL)
        return QW_STATUS_ERR_FORMAT;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu &&
        data[2] == 0xbfu) {
        data += 3;
        size -= 3u;
    }
    if (!qw_text_buffer_append(
            &output,
            "{\"cells\":[{\"cell_type\":\"markdown\",\"metadata\":{},"
            "\"source\":[",
            strlen("{\"cells\":[{\"cell_type\":\"markdown\",\"metadata\":{},"
                   "\"source\":["))) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    while (cursor < size) {
        size_t start = cursor;
        size_t end;
        while (cursor < size && data[cursor] != '\n') ++cursor;
        end = cursor;
        if (cursor < size) ++cursor; /* keep the newline with the line */
        if (!first && !qw_text_buffer_append(&output, ",", 1u)) {
            free(output.data);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
        first = 0;
        if (!qw_text_buffer_append(&output, "\"", 1u) ||
            !family_export_json_escape(&output, (const char *)data + start,
                                       end - start) ||
            (cursor > end && !qw_text_buffer_append(&output, "\\n", 2u)) ||
            !qw_text_buffer_append(&output, "\"", 1u)) {
            free(output.data);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
    }
    if (!qw_text_buffer_append(
            &output,
            "]}],\"metadata\":{\"language_info\":{\"name\":\"markdown\"}},"
            "\"nbformat\":4,\"nbformat_minor\":5}",
            strlen("]}],\"metadata\":{\"language_info\":{\"name\":\"markdown\"}},"
                   "\"nbformat\":4,\"nbformat_minor\":5}"))) {
        free(output.data);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "IPYNB output: Markdown source written as one markdown "
                 "cell (nbformat 4.5)");
        report->format = QW_FORMAT_IPYNB;
    }
    return QW_STATUS_OK;
}

static int family_export_opml(const uint8_t *data, size_t size,
                              uint8_t **out_data, size_t *out_size,
                              QwConvertReport *report)
{
    QwTextBuffer output = {0};
    size_t cursor = 0u;
    unsigned open_levels[8];
    size_t depth = 0u;
    size_t outline_count = 0u;

    output.limit = 128u * 1024u * 1024u;
    if (memchr(data, 0, size) != NULL)
        return QW_STATUS_ERR_FORMAT;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu &&
        data[2] == 0xbfu) {
        data += 3;
        size -= 3u;
    }
    if (!qw_text_buffer_append(
            &output,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<opml version=\"2.0\">\n<head><title>Quirewise outline</title>"
            "</head>\n<body>\n",
            strlen("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<opml version=\"2.0\">\n<head><title>Quirewise outline</title>"
                   "</head>\n<body>\n"))) {
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    while (cursor < size) {
        size_t start = cursor;
        size_t end;
        unsigned level = 0u;
        size_t text_start;
        while (cursor < size && data[cursor] != '\n') ++cursor;
        end = cursor;
        if (cursor < size) ++cursor;
        if (end > start && data[end - 1u] == '\r') --end;
        text_start = start;
        while (text_start < end && data[text_start] == '#') {
            ++text_start;
            ++level;
        }
        if (level == 1u || level > 6u) level = level > 6u ? 0u : 1u;
        if (level && text_start < end && data[text_start] != ' ') level = 0u;
        if (text_start < end && data[text_start] == ' ') ++text_start;
        if (text_start >= end && !level) continue;
        if (level) {
            while (depth > 0u && open_levels[depth - 1u] >= level) {
                if (!qw_text_buffer_append(&output, "</outline>\n", 11u)) {
                    free(output.data);
                    return QW_STATUS_ERR_OUT_OF_MEMORY;
                }
                --depth;
            }
            if (depth >= sizeof(open_levels) / sizeof(open_levels[0])) {
                free(output.data);
                return QW_STATUS_ERR_RESOURCE_LIMIT;
            }
            if (!qw_text_buffer_append(&output, "<outline text=\"", 15u) ||
                !family_export_xml_escape(&output,
                                          (const char *)data + text_start,
                                          end - text_start) ||
                !qw_text_buffer_append(&output, "\">\n", 3u)) {
                free(output.data);
                return QW_STATUS_ERR_OUT_OF_MEMORY;
            }
            open_levels[depth++] = level;
        } else {
            if (!qw_text_buffer_append(&output, "<outline text=\"", 15u) ||
                !family_export_xml_escape(&output,
                                          (const char *)data + text_start,
                                          end - text_start) ||
                !qw_text_buffer_append(&output, "\"/>\n", 4u)) {
                free(output.data);
                return QW_STATUS_ERR_OUT_OF_MEMORY;
            }
            ++outline_count;
        }
    }
    while (depth > 0u) {
        if (!qw_text_buffer_append(&output, "</outline>\n", 11u)) {
            free(output.data);
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
        --depth;
    }
    if (!qw_text_buffer_append(&output, "</body>\n</opml>\n",
                               strlen("</body>\n</opml>\n"))) {
        free(output.data);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "OPML output: Markdown headings became nested outlines "
                 "(%zu leaf outline(s))",
                 outline_count);
        report->format = QW_FORMAT_OPML;
    }
    return QW_STATUS_OK;
}
/* Non-PDF target dispatch: try the owning package's writer, then report a
 * precise unsupported pair. */
int qw_doc_convert_family_to_format(const uint8_t *data, size_t size,
                                    const char *input_extension,
                                    QwOutputFormat output_format,
                                    const QwConvertOptions *options,
                                    uint8_t **out_data, size_t *out_size,
                                    QwConvertReport *report)
{
    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (!data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (output_format == QW_FORMAT_IPYNB) {
        return family_export_ipynb(data, size, out_data, out_size, report);
    }
    if (output_format == QW_FORMAT_OPML) {
        return family_export_opml(data, size, out_data, out_size, report);
    }
    if (family_data_target(output_format))
        return family_convert_data(data, size, input_extension, output_format,
                                   options, out_data, out_size, report);
    if (family_subtitle_target(output_format))
        return family_convert_subtitle(data, size, input_extension,
                                       output_format, options, out_data,
                                       out_size, report);
    if (family_spreadsheet_target(output_format))
        return family_convert_spreadsheet(data, size, input_extension,
                                          output_format, options, out_data,
                                          out_size, report);
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "no writer for %s output; supported family targets: JSON, "
                 "JSON Lines, YAML, TOML, vCard, XML, SRT, WebVTT, LRC, "
                 "MicroDVD, ASS, SSA, SAMI, SCC, TTML, CSV and TSV",
                 qw_format_name(output_format));
    }
    return QW_STATUS_ERR_UNSUPPORTED;
}
