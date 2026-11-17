/* Binary office writers: EPUB and DOCX.
 *
 * Both formats reuse the readers' HTML intermediate and the internal ZIP
 * writer that the archive package already provides (the document package pins
 * the same private zip_native.h contract). The result is deliberately a
 * minimal, valid package:
 *
 * - EPUB: one XHTML chapter, OPF manifest/spine and NCX table of contents.
 * - DOCX: WordprocessingML body with paragraphs, heading styles, character
 *   emphasis, bullet lists, hyperlinks, embedded images and basic tables
 *   (colspan/rowspan). There is no numbering, theme or section layout.
 *
 * The packages are validated in the test suite by reading them back with the
 * package's own EPUB and DOCX parsers.
 */

#include <quirewise/convert.h>
#include <quirewise/image.h>
#include <quirewise/htmlz.h>
#include <quirewise/hwpx.h>
#include <quirewise/text_archive.h>

#include "../backends/document_backend.h"
#include "../formats/archive/zip_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_OFFICE_OUTPUT_LIMIT ((size_t)64u * 1024u * 1024u)

typedef struct OfficeBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} OfficeBuffer;

typedef struct OfficeTag {
    char name[16];
    int closing;
    int self_closing;
    char alt[512];
    const uint8_t *src;
    size_t src_size;
    const uint8_t *href;
    size_t href_size;
    int colspan;
    int rowspan;
} OfficeTag;

typedef struct OfficeDocxImage {
    uint8_t *png;
    size_t png_size;
    unsigned width;
    unsigned height;
} OfficeDocxImage;

#define OFFICE_DOCX_MAX_IMAGES 64u

typedef struct OfficeDocxImages {
    OfficeDocxImage images[OFFICE_DOCX_MAX_IMAGES];
    size_t count;
} OfficeDocxImages;

#define OFFICE_DOCX_MAX_LINKS 64u

typedef struct OfficeDocxLink {
    char href[2048];
} OfficeDocxLink;

typedef struct OfficeDocxLinks {
    OfficeDocxLink links[OFFICE_DOCX_MAX_LINKS];
    size_t count;
} OfficeDocxLinks;

static int office_reserve(OfficeBuffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *grown;

    if (buffer->size > QW_OFFICE_OUTPUT_LIMIT ||
        additional > QW_OFFICE_OUTPUT_LIMIT - buffer->size)
        return 0;
    needed = buffer->size + additional;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 8192u;
    while (capacity < needed) {
        if (capacity > QW_OFFICE_OUTPUT_LIMIT / 2u) {
            capacity = QW_OFFICE_OUTPUT_LIMIT;
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

static int office_append(OfficeBuffer *buffer, const void *data, size_t size)
{
    if (!size) return 1;
    if (!office_reserve(buffer, size)) return 0;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static int office_text(OfficeBuffer *buffer, const char *text)
{
    return text && text[0] ? office_append(buffer, text, strlen(text)) : 1;
}

static int office_append_char(OfficeBuffer *buffer, char value)
{
    return office_append(buffer, &value, 1u);
}

static int office_escape_xml(OfficeBuffer *buffer, const uint8_t *text,
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
        case '\'': replacement = "&apos;"; break;
        default: break;
        }
        if (!replacement) continue;
        if (!office_append(buffer, text + start, index - start) ||
            !office_text(buffer, replacement))
            return 0;
        start = index + 1u;
    }
    return office_append(buffer, text + start, size - start);
}

static int office_fail(QwConvertReport *report, int status, const char *what,
                       const char *detail)
{
    if (report && !report->note[0]) {
        snprintf(report->note, sizeof(report->note), "%s: %s", what,
                 detail && detail[0] ? detail : "office output failed");
    }
    return status;
}

static int office_take(OfficeBuffer *buffer, uint8_t **out, size_t *out_size)
{
    if (!buffer->data) {
        buffer->data = (uint8_t *)malloc(1u);
        if (!buffer->data) return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    *out = buffer->data;
    *out_size = buffer->size;
    buffer->data = NULL;
    buffer->size = 0u;
    buffer->capacity = 0u;
    return QW_STATUS_OK;
}

static int office_find_tag_end(const uint8_t *data, size_t size, size_t start)
{
    size_t index = start;

    for (; index < size; ++index) {
        if (data[index] == '>') return (int)index;
    }
    return -1;
}

static void office_lower_name(char *name)
{
    size_t index;

    for (index = 0u; name[index]; ++index) {
        if (name[index] >= 'A' && name[index] <= 'Z')
            name[index] = (char)(name[index] - 'A' + 'a');
    }
}

/* Parse one tag at data[start] == '<'. Returns bytes consumed, 0 when the '<'
 * should be treated as text, and SIZE_MAX at the end of the input. */
static size_t office_parse_tag(const uint8_t *data, size_t size, size_t start,
                               OfficeTag *tag)
{
    size_t index = start + 1u;
    size_t name_start;

    memset(tag, 0, sizeof(*tag));
    if (index >= size) return 0u;
    if (data[index] == '!') {
        if (index + 1u < size && data[index + 1u] == '-' &&
            index + 2u < size && data[index + 2u] == '-') {
            index += 3u;
            while (index + 2u < size && !(data[index] == '-' &&
                                          data[index + 1u] == '-' &&
                                          data[index + 2u] == '>'))
                ++index;
            return index + 2u < size ? index + 3u - start : size - start;
        }
        {
            int end = office_find_tag_end(data, size, index);
            return end < 0 ? size - start : (size_t)end + 1u - start;
        }
    }
    if (data[index] == '?') {
        int end = office_find_tag_end(data, size, index);
        return end < 0 ? size - start : (size_t)end + 1u - start;
    }
    if (data[index] == '/') {
        tag->closing = 1;
        ++index;
    }
    name_start = index;
    while (index < size &&
           ((data[index] >= 'a' && data[index] <= 'z') ||
            (data[index] >= 'A' && data[index] <= 'Z') ||
            (data[index] >= '0' && data[index] <= '9'))) {
        if (index - name_start < sizeof(tag->name) - 1u)
            tag->name[index - name_start] = (char)data[index];
        ++index;
    }
    tag->name[index - name_start < sizeof(tag->name) - 1u
                    ? index - name_start : sizeof(tag->name) - 1u] = '\0';
    office_lower_name(tag->name);
    {
        int end = office_find_tag_end(data, size, index);
        size_t position;

        if (end < 0) return 0u;
        if ((size_t)end > index && data[(size_t)end - 1u] == '/')
            tag->self_closing = 1;
        /* alt= is the replacement text; src= carries data URIs. */
        for (position = index; position + 5u < (size_t)end; ++position) {
            if ((data[position] == 'a' || data[position] == 'A') &&
                (data[position + 1u] == 'l' || data[position + 1u] == 'L') &&
                (data[position + 2u] == 't' || data[position + 2u] == 'T') &&
                data[position + 3u] == '=' &&
                (data[position + 4u] == '"' || data[position + 4u] == '\'')) {
                char quote = (char)data[position + 4u];
                size_t alt = 0u;

                position += 5u;
                while (position < (size_t)end && data[position] != quote &&
                       alt + 1u < sizeof(tag->alt))
                    tag->alt[alt++] = (char)data[position++];
                tag->alt[alt] = '\0';
            }
        }
        for (position = index; position + 5u < (size_t)end; ++position) {
            if ((data[position] == 's' || data[position] == 'S') &&
                (data[position + 1u] == 'r' || data[position + 1u] == 'R') &&
                (data[position + 2u] == 'c' || data[position + 2u] == 'C') &&
                data[position + 3u] == '=' &&
                (data[position + 4u] == '"' || data[position + 4u] == '\'')) {
                char quote = (char)data[position + 4u];
                size_t start_value = position + 5u;
                position = start_value;
                while (position < (size_t)end && data[position] != quote)
                    ++position;
                tag->src = data + start_value;
                tag->src_size = position - start_value;
                break;
            }
        }
        for (position = index; position + 8u < (size_t)end; ++position) {
            if ((data[position] == 'r' || data[position] == 'R') &&
                (data[position + 1u] == 'o' || data[position + 1u] == 'O') &&
                (data[position + 2u] == 'w' || data[position + 2u] == 'W') &&
                (data[position + 3u] == 's' || data[position + 3u] == 'S') &&
                (data[position + 4u] == 'p' || data[position + 4u] == 'P') &&
                (data[position + 5u] == 'a' || data[position + 5u] == 'A') &&
                (data[position + 6u] == 'n' || data[position + 6u] == 'N') &&
                data[position + 7u] == '=' &&
                (data[position + 8u] == '"' || data[position + 8u] == '\'')) {
                char quote = (char)data[position + 8u];
                int value = 0;
                position += 9u;
                while (position < (size_t)end && data[position] != quote &&
                       data[position] >= '0' && data[position] <= '9') {
                    value = value * 10 + (data[position] - '0');
                    if (value > 64) value = 64;
                    ++position;
                }
                tag->rowspan = value;
                break;
            }
        }
        for (position = index; position + 8u < (size_t)end; ++position) {
            if ((data[position] == 'c' || data[position] == 'C') &&
                (data[position + 1u] == 'o' || data[position + 1u] == 'O') &&
                (data[position + 2u] == 'l' || data[position + 2u] == 'L') &&
                (data[position + 3u] == 's' || data[position + 3u] == 'S') &&
                (data[position + 4u] == 'p' || data[position + 4u] == 'P') &&
                (data[position + 5u] == 'a' || data[position + 5u] == 'A') &&
                (data[position + 6u] == 'n' || data[position + 6u] == 'N') &&
                data[position + 7u] == '=' &&
                (data[position + 8u] == '"' || data[position + 8u] == '\'')) {
                char quote = (char)data[position + 8u];
                int value = 0;
                position += 9u;
                while (position < (size_t)end && data[position] != quote &&
                       data[position] >= '0' && data[position] <= '9') {
                    value = value * 10 + (data[position] - '0');
                    if (value > 64) value = 64;
                    ++position;
                }
                tag->colspan = value;
                break;
            }
        }
        for (position = index; position + 6u < (size_t)end; ++position) {
            if ((data[position] == 'h' || data[position] == 'H') &&
                (data[position + 1u] == 'r' || data[position + 1u] == 'R') &&
                (data[position + 2u] == 'e' || data[position + 2u] == 'E') &&
                (data[position + 3u] == 'f' || data[position + 3u] == 'F') &&
                data[position + 4u] == '=' &&
                (data[position + 5u] == '"' || data[position + 5u] == '\'')) {
                char quote = (char)data[position + 5u];
                size_t start_value = position + 6u;
                position = start_value;
                while (position < (size_t)end && data[position] != quote)
                    ++position;
                tag->href = data + start_value;
                tag->href_size = position - start_value;
                break;
            }
        }
        return (size_t)end + 1u - start;
    }
}

/* Skip comments, script, style and head content. */
static size_t office_skip_element(const uint8_t *data, size_t size,
                                  size_t start, const char *name)
{
    size_t index = start;
    size_t name_size = strlen(name);

    for (; index + name_size + 3u <= size; ++index) {
        if (data[index] == '<' && data[index + 1u] == '/') {
            size_t candidate = index + 2u;
            size_t offset;
            int matches = 1;

            for (offset = 0u; offset < name_size; ++offset) {
                char value = (char)data[candidate + offset];
                if (value >= 'A' && value <= 'Z')
                    value = (char)(value - 'A' + 'a');
                if (value != name[offset]) {
                    matches = 0;
                    break;
                }
            }
            if (matches) {
                int end = office_find_tag_end(data, size, candidate);
                return end < 0 ? size : (size_t)end + 1u;
            }
        }
    }
    return size;
}

/* ------------------------------------------------------------------------- */
/* DOCX                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct DocxState {
    OfficeBuffer body;
    int paragraph_open;
    int heading_level;
    int quote;
    int bold;
    int italic;
    int code;
    int pre;
    int table_cell_open;
    int first_cell;
    int table_open;
    int table_row_open;
    int table_cell_active;
    size_t table_column;
    int vmerge_remaining[64];
    int link_open;
    size_t link_index;
} DocxState;

static int docx_open_paragraph(DocxState *state)
{
    char properties[128];

    if (state->paragraph_open) return 1;
    if (!office_text(&state->body, "<w:p>")) return 0;
    if (state->heading_level > 0 || state->quote) {
        if (!office_text(&state->body, "<w:pPr>")) return 0;
        if (state->heading_level > 0) {
            snprintf(properties, sizeof(properties),
                     "<w:pStyle w:val=\"Heading%d\"/>",
                     state->heading_level);
            if (!office_text(&state->body, properties)) return 0;
        }
        if (state->quote && !office_text(&state->body,
                                         "<w:ind w:left=\"720\"/>"))
            return 0;
        if (!office_text(&state->body, "</w:pPr>")) return 0;
    }
    state->paragraph_open = 1;
    return 1;
}

static int docx_append_run(DocxState *state, const uint8_t *text, size_t size)
{
    size_t index;

    if (!size) return 1;
    if (!docx_open_paragraph(state)) return 0;
    if (!office_text(&state->body, "<w:r><w:rPr>")) return 0;
    if (state->bold && !office_text(&state->body, "<w:b/>")) return 0;
    if (state->italic && !office_text(&state->body, "<w:i/>")) return 0;
    if (state->code &&
        !office_text(&state->body,
                     "<w:rFonts w:ascii=\"Courier New\" "
                     "w:hAnsi=\"Courier New\"/>"))
        return 0;
    if (!office_text(&state->body, "</w:rPr>")) return 0;
    if (state->pre) {
        /* Preserve line breaks inside preformatted text. */
        size_t start = 0u;

        for (index = 0u; index < size; ++index) {
            if (text[index] != '\n') continue;
            if (index > start) {
                if (!office_text(&state->body,
                                 "<w:t xml:space=\"preserve\">") ||
                    !office_escape_xml(&state->body, text + start,
                                       index - start) ||
                    !office_text(&state->body, "</w:t>"))
                    return 0;
            }
            if (!office_text(&state->body, "<w:br/>")) return 0;
            start = index + 1u;
        }
        if (start < size) {
            if (!office_text(&state->body, "<w:t xml:space=\"preserve\">") ||
                !office_escape_xml(&state->body, text + start, size - start) ||
                !office_text(&state->body, "</w:t>"))
                return 0;
        }
        return office_text(&state->body, "</w:r>");
    }
    if (!office_text(&state->body, "<w:t xml:space=\"preserve\">") ||
        !office_escape_xml(&state->body, text, size) ||
        !office_text(&state->body, "</w:t></w:r>"))
        return 0;
    return 1;
}


static void docx_images_free(OfficeDocxImages *images)
{
    size_t i;
    if (!images) return;
    for (i = 0u; i < images->count; ++i)
        free(images->images[i].png);
    memset(images, 0, sizeof(*images));
}

static int office_base64_value(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') return (int)(ch - 'A');
    if (ch >= 'a' && ch <= 'z') return (int)(ch - 'a') + 26;
    if (ch >= '0' && ch <= '9') return (int)(ch - '0') + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static uint8_t *office_base64_decode(const uint8_t *data, size_t size,
                                     size_t *out_size)
{
    uint8_t *decoded;
    size_t capacity = (size / 4u + 1u) * 3u;
    size_t written = 0u;
    uint32_t value = 0u;
    int bits = 0;
    size_t i;
    if (out_size) *out_size = 0u;
    if (!data || !size || !out_size) return NULL;
    decoded = (uint8_t *)malloc(capacity);
    if (!decoded) return NULL;
    for (i = 0u; i < size; ++i) {
        int digit;
        if (data[i] == '=') break;
        digit = office_base64_value(data[i]);
        if (digit < 0) continue;
        value = (value << 6) | (uint32_t)digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= capacity) {
                free(decoded);
                return NULL;
            }
            decoded[written++] = (uint8_t)((value >> bits) & 0xFFu);
        }
    }
    *out_size = written;
    return decoded;
}

static int office_append_docx_drawing(OfficeBuffer *body, size_t index,
                                      unsigned width, unsigned height)
{
    char xml[1024];
    long cx = (long)width * 9525L;
    long cy = (long)height * 9525L;
    int length = snprintf(
        xml, sizeof(xml),
        "<w:p><w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" "
        "distL=\"0\" distR=\"0\"><wp:extent cx=\"%ld\" cy=\"%ld\"/>"
        "<wp:docPr id=\"%lu\" name=\"Picture %lu\"/>"
        "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/"
        "drawingml/2006/picture\"><pic:pic><pic:nvPicPr>"
        "<pic:cNvPr id=\"%lu\" name=\"image%lu.png\"/><pic:cNvPicPr/>"
        "</pic:nvPicPr><pic:blipFill><a:blip r:embed=\"rIdImg%lu\"/>"
        "<a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
        "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%ld\" cy=\"%ld\"/>"
        "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>"
        "</pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline>"
        "</w:drawing></w:r></w:p>",
        cx, cy, (unsigned long)(index + 1u), (unsigned long)(index + 1u),
        (unsigned long)(index + 1u), (unsigned long)(index + 1u),
        (unsigned long)index, cx, cy);
    if (length <= 0 || (size_t)length >= sizeof(xml)) return 0;
    return office_append(body, xml, (size_t)length);
}

static int docx_emit_vmerge_cell(DocxState *state, int restart)
{
    if (!office_text(&state->body, "<w:tc><w:tcPr><w:vMerge")) return 0;
    if (restart && !office_text(&state->body, " w:val=\"restart\"")) return 0;
    if (!office_text(&state->body, "/></w:tcPr></w:tc>")) return 0;
    return 1;
}

static int docx_flush_vmerge_gap(DocxState *state)
{
    while (state->table_column < 64u &&
           state->vmerge_remaining[state->table_column] > 0) {
        if (!docx_emit_vmerge_cell(state, 0)) return 0;
        --state->vmerge_remaining[state->table_column];
        ++state->table_column;
    }
    return 1;
}

static int docx_flush_paragraph(DocxState *state);

static int docx_close_cell(DocxState *state)
{
    if (!state->table_cell_active) return 1;
    if (!docx_flush_paragraph(state)) return 0;
    if (state->link_open) {
        if (!office_text(&state->body, "</w:hyperlink>")) return 0;
        state->link_open = 0;
    }
    if (!office_text(&state->body, "</w:tc>")) return 0;
    state->table_cell_active = 0;
    state->table_cell_open = 0;
    return 1;
}

static int docx_close_row(DocxState *state)
{
    if (!state->table_row_open) return 1;
    if (!docx_close_cell(state)) return 0;
    if (!docx_flush_vmerge_gap(state)) return 0;
    if (!office_text(&state->body, "</w:tr>")) return 0;
    state->table_row_open = 0;
    return 1;
}

static int docx_flush_paragraph(DocxState *state)
{
    if (!state->paragraph_open) return 1;
    state->paragraph_open = 0;
    return office_text(&state->body, "</w:p>\n");
}

static int office_link_is_safe(const uint8_t *href, size_t size)
{
    size_t i;
    if (!href || size == 0u) return 0;
    for (i = 0u; i < size; ++i) {
        if (href[i] == ' ' || href[i] == '"' || href[i] == '\'' ||
            href[i] == '\r' || href[i] == '\n' || href[i] == '\t')
            return 0;
    }
    if (size >= 7u && memcmp(href, "http://", 7u) == 0) return 1;
    if (size >= 8u && memcmp(href, "https://", 8u) == 0) return 1;
    if (size >= 7u && memcmp(href, "mailto:", 7u) == 0) return 1;
    return 0;
}

static int docx_body_from_html(const uint8_t *html, size_t size,
                               OfficeDocxImages *images,
                               OfficeDocxLinks *links,
                               OfficeBuffer *body)
{
    DocxState state;
    size_t index = 0u;
    int in_head = 0;

    memset(&state, 0, sizeof(state));
    while (index < size) {
        if (html[index] != '<') {
            size_t start = index;
            while (index < size && html[index] != '<') ++index;
            if (!in_head && !docx_append_run(&state, html + start,
                                             index - start))
                goto out_of_memory;
            continue;
        }
        {
            OfficeTag tag;
            size_t consumed = office_parse_tag(html, size, index, &tag);
            if (!consumed) {
                if (!in_head &&
                    !docx_append_run(&state, (const uint8_t *)"<", 1u))
                    goto out_of_memory;
                ++index;
                continue;
            }
            index += consumed;
            if (!tag.name[0]) continue;
            if (strcmp(tag.name, "head") == 0) {
                in_head = tag.closing ? 0 : 1;
                continue;
            }
            if (strcmp(tag.name, "script") == 0 ||
                strcmp(tag.name, "style") == 0 ||
                strcmp(tag.name, "title") == 0) {
                if (!tag.closing)
                    index = office_skip_element(html, size, index, tag.name);
                continue;
            }
            if (in_head) continue;
            if (tag.closing) {
                if (strcmp(tag.name, "strong") == 0 ||
                    strcmp(tag.name, "b") == 0) {
                    state.bold = 0;
                } else if (strcmp(tag.name, "em") == 0 ||
                           strcmp(tag.name, "i") == 0) {
                    state.italic = 0;
                } else if (strcmp(tag.name, "code") == 0) {
                    state.code = 0;
                } else if (strcmp(tag.name, "pre") == 0) {
                    if (!docx_flush_paragraph(&state)) goto out_of_memory;
                    state.pre = 0;
                } else if (strcmp(tag.name, "p") == 0 ||
                           strcmp(tag.name, "div") == 0 ||
                           strcmp(tag.name, "li") == 0) {
                    if (!docx_flush_paragraph(&state)) goto out_of_memory;
                } else if (tag.name[0] == 'h' && tag.name[1] >= '1' &&
                           tag.name[1] <= '6' && !tag.name[2]) {
                    if (!docx_flush_paragraph(&state)) goto out_of_memory;
                    state.heading_level = 0;
                } else if (strcmp(tag.name, "blockquote") == 0) {
                    if (!docx_flush_paragraph(&state)) goto out_of_memory;
                    state.quote = 0;
                } else if (strcmp(tag.name, "tr") == 0) {
                    if (!docx_close_row(&state)) goto out_of_memory;
                } else if (strcmp(tag.name, "td") == 0 ||
                           strcmp(tag.name, "th") == 0) {
                    if (!docx_close_cell(&state)) goto out_of_memory;
                    if (strcmp(tag.name, "th") == 0) state.bold = 0;
                } else if (strcmp(tag.name, "table") == 0) {
                    if (!docx_close_row(&state)) goto out_of_memory;
                    if (state.table_open &&
                        !office_text(&state.body, "</w:tbl>"))
                        goto out_of_memory;
                    state.table_open = 0;
                    if (!docx_flush_paragraph(&state)) goto out_of_memory;
                } else if (strcmp(tag.name, "a") == 0) {
                    if (state.link_open) {
                        if (!office_text(&state.body, "</w:hyperlink>"))
                            goto out_of_memory;
                        state.link_open = 0;
                    }
                }
                continue;
            }
            if (strcmp(tag.name, "strong") == 0 ||
                strcmp(tag.name, "b") == 0) {
                state.bold = 1;
            } else if (strcmp(tag.name, "em") == 0 ||
                       strcmp(tag.name, "i") == 0) {
                state.italic = 1;
            } else if (strcmp(tag.name, "code") == 0) {
                state.code = 1;
            } else if (strcmp(tag.name, "p") == 0 ||
                       strcmp(tag.name, "div") == 0) {
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
            } else if (strcmp(tag.name, "pre") == 0) {
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
                state.pre = 1;
            } else if (tag.name[0] == 'h' && tag.name[1] >= '1' &&
                       tag.name[1] <= '6' && !tag.name[2]) {
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
                state.heading_level = tag.name[1] - '0';
            } else if (strcmp(tag.name, "blockquote") == 0) {
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
                state.quote = 1;
            } else if (strcmp(tag.name, "ul") == 0 ||
                       strcmp(tag.name, "ol") == 0) {
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
            } else if (strcmp(tag.name, "li") == 0) {
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
                if (!docx_append_run(&state, (const uint8_t *)"\xe2\x80\xa2 ",
                                     4u))
                    goto out_of_memory;
            } else if (strcmp(tag.name, "br") == 0) {
                if (!docx_open_paragraph(&state)) goto out_of_memory;
                if (!office_text(&state.body, "<w:r><w:br/></w:r>"))
                    goto out_of_memory;
            } else if (strcmp(tag.name, "hr") == 0) {
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
                if (!docx_append_run(&state,
                                     (const uint8_t *)"\xe2\x80\x95"
                                     "\xe2\x80\x95\xe2\x80\x95", 9u))
                    goto out_of_memory;
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
            } else if (strcmp(tag.name, "table") == 0) {
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
                if (state.table_open) {
                    /* Nested tables are flattened into the current cell. */
                    if (!docx_append_run(&state, (const uint8_t *)" ", 1u))
                        goto out_of_memory;
                } else {
                    if (!office_text(&state.body,
                            "<w:tbl><w:tblPr><w:tblW w:w=\"0\" "
                            "w:type=\"auto\"/></w:tblPr>"))
                        goto out_of_memory;
                    state.table_open = 1;
                    state.table_row_open = 0;
                    state.table_cell_active = 0;
                    memset(state.vmerge_remaining, 0,
                           sizeof(state.vmerge_remaining));
                }
            } else if (strcmp(tag.name, "tr") == 0) {
                if (!state.table_open) {
                    if (!office_text(&state.body,
                            "<w:tbl><w:tblPr><w:tblW w:w=\"0\" "
                            "w:type=\"auto\"/></w:tblPr>"))
                        goto out_of_memory;
                    state.table_open = 1;
                }
                if (!docx_close_row(&state)) goto out_of_memory;
                if (!office_text(&state.body, "<w:tr>")) goto out_of_memory;
                state.table_row_open = 1;
                state.table_cell_open = 0;
                state.table_column = 0u;
                state.first_cell = 1;
            } else if (strcmp(tag.name, "td") == 0 ||
                       strcmp(tag.name, "th") == 0) {
                if (!state.table_open) {
                    if (!office_text(&state.body,
                            "<w:tbl><w:tblPr><w:tblW w:w=\"0\" "
                            "w:type=\"auto\"/></w:tblPr>"))
                        goto out_of_memory;
                    state.table_open = 1;
                }
                if (!docx_close_cell(&state)) goto out_of_memory;
                if (!state.table_row_open) {
                    if (!office_text(&state.body, "<w:tr>")) goto out_of_memory;
                    state.table_row_open = 1;
                }
                if (!docx_flush_paragraph(&state)) goto out_of_memory;
                if (!docx_flush_vmerge_gap(&state)) goto out_of_memory;
                if (!office_text(&state.body, "<w:tc><w:tcPr>")) goto out_of_memory;
                if (tag.colspan > 1) {
                    char span[64];
                    int length = snprintf(span, sizeof(span),
                                          "<w:gridSpan w:val=\"%d\"/>",
                                          tag.colspan);
                    if (length <= 0 || (size_t)length >= sizeof(span) ||
                        !office_append(&state.body, span, (size_t)length))
                        goto out_of_memory;
                }
                if (tag.rowspan > 1 && state.table_column < 64u) {
                    size_t column;
                    if (!office_text(&state.body,
                                     "<w:vMerge w:val=\"restart\"/>"))
                        goto out_of_memory;
                    for (column = 0u;
                         column < (size_t)(tag.colspan > 0 ? tag.colspan : 1) &&
                         state.table_column + column < 64u;
                         ++column)
                        state.vmerge_remaining[state.table_column + column] =
                            tag.rowspan - 1;
                }
                if (!office_text(&state.body, "</w:tcPr>")) goto out_of_memory;
                state.table_cell_active = 1;
                state.table_cell_open = 1;
                state.table_column += tag.colspan > 0 ? (size_t)tag.colspan : 1u;
                state.first_cell = 0;
                if (strcmp(tag.name, "th") == 0) state.bold = 1;
            } else if (strcmp(tag.name, "a") == 0) {
                if (links && state.link_index < OFFICE_DOCX_MAX_LINKS &&
                           office_link_is_safe(tag.href, tag.href_size)) {
                    size_t href_size = tag.href_size;
                    if (href_size >= sizeof(links->links[state.link_index].href))
                        href_size = sizeof(links->links[state.link_index].href) - 1u;
                    if (!docx_open_paragraph(&state)) goto out_of_memory;
                    memcpy(links->links[state.link_index].href, tag.href,
                           href_size);
                    links->links[state.link_index].href[href_size] = '\0';
                    {
                        char opening[64];
                        int length = snprintf(opening, sizeof(opening),
                                              "<w:hyperlink r:id=\"rIdLink%lu\">",
                                              (unsigned long)state.link_index);
                        if (length <= 0 || (size_t)length >= sizeof(opening) ||
                            !office_append(&state.body, opening, (size_t)length))
                            goto out_of_memory;
                    }
                    state.link_open = 1;
                    ++links->count;
                    ++state.link_index;
                }
            } else if (strcmp(tag.name, "img") == 0) {
                int embedded = 0;
                if (images && images->count < OFFICE_DOCX_MAX_IMAGES &&
                    tag.src && tag.src_size > 20u &&
                    memcmp(tag.src, "data:image/", 11u) == 0) {
                    const uint8_t *marker = NULL;
                    size_t marker_at = 0u;
                    size_t i;
                    for (i = 11u; i + 8u <= tag.src_size; ++i) {
                        if (memcmp(tag.src + i, ";base64,", 8u) == 0) {
                            marker = tag.src + i + 8u;
                            marker_at = i + 8u;
                            break;
                        }
                    }
                    if (marker && marker_at < tag.src_size) {
                        size_t raw_size = 0u;
                        uint8_t *raw = office_base64_decode(
                            marker, tag.src_size - marker_at, &raw_size);
                        if (raw && raw_size > 0u) {
                            QwImage image;
                            uint8_t *png = NULL;
                            size_t png_size = 0u;
                            memset(&image, 0, sizeof(image));
                            if (qw_image_decode(raw, raw_size, &image) ==
                                    QW_IMAGE_OK &&
                                image.pixels &&
                                qw_image_encode(&image, QW_IMAGE_ENCODE_PNG,
                                                &png, &png_size) ==
                                    QW_IMAGE_OK &&
                                png && png_size) {
                                OfficeDocxImage *slot =
                                    &images->images[images->count];
                                if (!docx_flush_paragraph(&state)) {
                                    qw_image_free(&image);
                                    free(png);
                                    free(raw);
                                    goto out_of_memory;
                                }
                                slot->png = png;
                                slot->png_size = png_size;
                                slot->width = image.width;
                                slot->height = image.height;
                                if (!office_append_docx_drawing(
                                        &state.body, images->count,
                                        image.width, image.height)) {
                                    qw_image_free(&image);
                                    free(raw);
                                    goto out_of_memory;
                                }
                                ++images->count;
                                embedded = 1;
                                png = NULL;
                            }
                            qw_image_free(&image);
                            free(png);
                        }
                        free(raw);
                    }
                }
                if (!embedded && tag.alt[0] &&
                    !docx_append_run(&state, (const uint8_t *)tag.alt,
                                     strlen(tag.alt)))
                    goto out_of_memory;
            }
        }
    }
    if (!docx_flush_paragraph(&state)) goto out_of_memory;
    body->data = state.body.data;
    body->size = state.body.size;
    body->capacity = state.body.capacity;
    return 1;

out_of_memory:
    free(state.body.data);
    return 0;
}

static int office_zip_begin(QwZipWriter *writer, int stored_mimetype)
{
    (void)stored_mimetype;
    return qw_zip_writer_init_heap(writer, 0u, 0u);
}


static int office_zip_finish(QwZipWriter *writer, uint8_t **out,
                             size_t *out_size, OfficeBuffer *owned)
{
    void *archive = NULL;
    size_t archive_size = 0u;

    if (!qw_zip_writer_finalize_heap_archive(writer, &archive,
                                             &archive_size) ||
        !archive) {
        qw_zip_writer_end(writer);
        return 0;
    }
    qw_zip_writer_end(writer);
    *out = (uint8_t *)archive;
    *out_size = archive_size;
    (void)owned;
    return 1;
}

int qw_doc_convert_docx_memory(const uint8_t *data, size_t size,
                               const char *input_extension,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report)
{
    static const char content_types[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/"
        "content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/"
        "vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"png\" ContentType=\"image/png\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/"
        "vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"
        "\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/"
        "vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
        "</Types>";
    static const char package_rels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
        "2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
        "officeDocument/2006/relationships/officeDocument\" "
        "Target=\"word/document.xml\"/>"
        "</Relationships>";
    static const char document_rels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
        "2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
        "officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "</Relationships>";
    static const char styles[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/"
        "wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\"/>"
        "<w:sz w:val=\"22\"/></w:rPr></w:rPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\">"
        "<w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr>"
        "<w:rPr><w:b/><w:sz w:val=\"36\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\">"
        "<w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:outlineLvl w:val=\"1\"/></w:pPr>"
        "<w:rPr><w:b/><w:sz w:val=\"30\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\">"
        "<w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:outlineLvl w:val=\"2\"/></w:pPr>"
        "<w:rPr><w:b/><w:sz w:val=\"26\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading4\">"
        "<w:name w:val=\"heading 4\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:outlineLvl w:val=\"3\"/></w:pPr>"
        "<w:rPr><w:b/><w:sz w:val=\"24\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading5\">"
        "<w:name w:val=\"heading 5\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:outlineLvl w:val=\"4\"/></w:pPr>"
        "<w:rPr><w:b/><w:sz w:val=\"22\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading6\">"
        "<w:name w:val=\"heading 6\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:outlineLvl w:val=\"5\"/></w:pPr>"
        "<w:rPr><w:b/><w:sz w:val=\"22\"/></w:rPr></w:style>"
        "</w:styles>";
    static const char document_prefix[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
        "wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/"
        "relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/"
        "wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/"
        "picture\"><w:body>";
    static const char document_suffix[] =
        "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/>"
        "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
        "w:left=\"1440\"/></w:sectPr></w:body></w:document>";
    uint8_t *html = NULL;
    size_t html_size = 0u;
    OfficeBuffer body;
    OfficeBuffer document;
    OfficeBuffer docx_rels;
    OfficeDocxImages images;
    OfficeDocxLinks links;
    QwZipWriter writer;
    int status;

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if ((size >= 5u && memcmp(data, "%PDF-", 5u) == 0) ||
        (input_extension && strcmp(input_extension, "pdf") == 0))
        return qw_doc_office_docx_from_pdf(data, size, NULL, out_data,
                                           out_size, report);
    status = qw_doc_convert_html_memory(data, size, input_extension, &html,
                                        &html_size, report);
    if (status != QW_STATUS_OK) return status;
    memset(&body, 0, sizeof(body));
    memset(&images, 0, sizeof(images));
    memset(&links, 0, sizeof(links));
    if (!docx_body_from_html(html, html_size, &images, &links, &body)) {
        free(html);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY,
                           "DOCX body", "out of memory");
    }
    free(html);
    memset(&document, 0, sizeof(document));
    if (!office_text(&document, document_prefix) ||
        !office_append(&document, body.data, body.size) ||
        !office_text(&document, document_suffix)) {
        free(body.data);
        free(document.data);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "DOCX build",
                           "out of memory");
    }
    free(body.data);
    memset(&docx_rels, 0, sizeof(docx_rels));
    if (!office_text(&docx_rels,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/"
            "package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
            "officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>")) {
        free(body.data);
        free(document.data);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "DOCX build",
                           "out of memory");
    }
    {
        size_t image_index;
        for (image_index = 0u; image_index < images.count; ++image_index) {
            char relationship[320];
            int length = snprintf(
                relationship, sizeof(relationship),
                "<Relationship Id=\"rIdImg%lu\" Type=\"http://schemas."
                "openxmlformats.org/officeDocument/2006/relationships/image\" "
                "Target=\"media/image%lu.png\"/>",
                (unsigned long)image_index, (unsigned long)image_index);
            if (length <= 0 || (size_t)length >= sizeof(relationship) ||
                !office_append(&docx_rels, relationship, (size_t)length)) {
                free(body.data);
                free(document.data);
                free(docx_rels.data);
                return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY,
                                   "DOCX build", "out of memory");
            }
        }
    }
    {
        size_t link_index;
        for (link_index = 0u; link_index < links.count; ++link_index) {
            char relationship[128];
            int length = snprintf(
                relationship, sizeof(relationship),
                "<Relationship Id=\"rIdLink%lu\" Type=\"http://schemas."
                "openxmlformats.org/officeDocument/2006/relationships/"
                "hyperlink\" Target=\"",
                (unsigned long)link_index);
            if (length <= 0 || (size_t)length >= sizeof(relationship) ||
                !office_append(&docx_rels, relationship, (size_t)length) ||
                !office_escape_xml(&docx_rels,
                                   (const uint8_t *)links.links[link_index].href,
                                   strlen(links.links[link_index].href)) ||
                !office_text(&docx_rels, "\" TargetMode=\"External\"/>")) {
                free(body.data);
                free(document.data);
                free(docx_rels.data);
                return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY,
                                   "DOCX build", "out of memory");
            }
        }
    }
    if (!office_text(&docx_rels, "</Relationships>")) {
        free(body.data);
        free(document.data);
        free(docx_rels.data);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "DOCX build",
                           "out of memory");
    }
    {
        char document_name[] = "[Content_Types].xml";

        if (!office_zip_begin(&writer, 0)) {
            free(document.data);
            return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "DOCX ZIP",
                               "cannot start the archive");
        }
        /* office_zip_add uses strlen(); document XML contains no NUL bytes. */
        if (!qw_zip_writer_add_mem(&writer, document_name, content_types,
                                   strlen(content_types),
                                   QW_ZIP_NO_COMPRESSION) ||
            !qw_zip_writer_add_mem(&writer, "_rels/.rels", package_rels,
                                   strlen(package_rels),
                                   QW_ZIP_NO_COMPRESSION) ||
            !qw_zip_writer_add_mem(&writer, "word/document.xml",
                                   document.data, document.size,
                                   QW_ZIP_NO_COMPRESSION) ||
            !qw_zip_writer_add_mem(&writer, "word/styles.xml", styles,
                                   strlen(styles),
                                   QW_ZIP_NO_COMPRESSION) ||
            !qw_zip_writer_add_mem(&writer, "word/_rels/document.xml.rels",
                                   docx_rels.data, docx_rels.size,
                                   QW_ZIP_NO_COMPRESSION)) {
            qw_zip_writer_end(&writer);
            free(document.data);
            return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY,
                               "DOCX ZIP", "cannot add a package part");
        }
    }
    {
        size_t image_index;
        for (image_index = 0u; image_index < images.count; ++image_index) {
            char name[64];
            int length = snprintf(name, sizeof(name),
                                  "word/media/image%lu.png",
                                  (unsigned long)image_index);
            if (length <= 0 || (size_t)length >= sizeof(name) ||
                !qw_zip_writer_add_mem(&writer, name,
                                       images.images[image_index].png,
                                       images.images[image_index].png_size,
                                       QW_ZIP_NO_COMPRESSION)) {
                docx_images_free(&images);
                qw_zip_writer_end(&writer);
                free(document.data);
                free(docx_rels.data);
                return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY,
                                   "DOCX ZIP", "cannot add an image part");
            }
        }
    }
    if (!office_zip_finish(&writer, out_data, out_size, &document)) {
        free(document.data);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "DOCX ZIP",
                           "cannot finalize the archive");
    }
    free(document.data);
    free(docx_rels.data);
    docx_images_free(&images);
    if (report) {
        report->format = QW_FORMAT_DOCX;
        report->output_format = QW_FORMAT_DOCX;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "minimal WordprocessingML package: paragraphs, headings, "
                 "character emphasis, bullet text, %zu embedded image(s) and "
                 "%zu hyperlink(s); tables are written as Word tables",
                 images.count, links.count);
    }
    return QW_STATUS_OK;
}

/* ------------------------------------------------------------------------- */
/* EPUB                                                                       */
/* ------------------------------------------------------------------------- */

static int epub_strip_document(const uint8_t *html, size_t size,
                               OfficeBuffer *body)
{
    size_t start = 0u;
    size_t end = size;
    size_t index;

    /* Locate <body ...> and </body>. */
    for (index = 0u; index + 5u <= size; ++index) {
        if (html[index] == '<' && html[index + 1u] == 'b' &&
            html[index + 2u] == 'o' && html[index + 3u] == 'd' &&
            html[index + 4u] == 'y') {
            int tag_end = office_find_tag_end(html, size, index);
            if (tag_end < 0) break;
            start = (size_t)tag_end + 1u;
            break;
        }
    }
    if (start) {
        for (index = start; index + 7u <= size; ++index) {
            if (html[index] == '<' && html[index + 1u] == '/' &&
                html[index + 2u] == 'b' && html[index + 3u] == 'o' &&
                html[index + 4u] == 'd' && html[index + 5u] == 'y') {
                end = index;
                break;
            }
        }
    }
    /* Self-close void HTML tags so the chapter is well-formed XML. */
    index = start;
    while (index < end) {
        if (html[index] != '<') {
            if (html[index] == '&' && index + 5u <= end &&
                memcmp(html + index, "&nbsp;", 6u) == 0) {
                if (!office_text(body, "&#160;")) return 0;
                index += 6u;
                continue;
            }
            if (!office_append_char(body, (char)html[index])) return 0;
            ++index;
            continue;
        }
        {
            size_t name_start = index + 1u;
            size_t name_size = 0u;
            size_t tag_end;
            int closing = 0;

            if (name_start < end && html[name_start] == '/') {
                closing = 1;
                ++name_start;
            }
            while (name_start + name_size < end &&
                   ((html[name_start + name_size] >= 'a' &&
                     html[name_start + name_size] <= 'z') ||
                    (html[name_start + name_size] >= 'A' &&
                     html[name_start + name_size] <= 'Z') ||
                    (html[name_start + name_size] >= '0' &&
                     html[name_start + name_size] <= '9')))
                ++name_size;
            tag_end = (size_t)office_find_tag_end(html, end, index);
            if (!name_size || tag_end == (size_t)-1) {
                if (!office_append_char(body, '<')) return 0;
                ++index;
                continue;
            }
            {
                const uint8_t *name = html + name_start;
                int void_tag =
                    (name_size == 2u && (name[0] == 'b' || name[0] == 'B') &&
                     (name[1] == 'r' || name[1] == 'R')) ||
                    (name_size == 2u && (name[0] == 'h' || name[0] == 'H') &&
                     (name[1] == 'r' || name[1] == 'R')) ||
                    (name_size == 3u && (name[0] == 'i' || name[0] == 'I') &&
                     (name[1] == 'm' || name[1] == 'M') &&
                     (name[2] == 'g' || name[2] == 'G'));
                size_t copy_end = tag_end;

                if (!closing && void_tag && tag_end > index &&
                    html[tag_end - 1u] == '/')
                    copy_end = tag_end - 1u;
                if (!office_append(body, html + index, copy_end - index))
                    return 0;
                if (!closing && void_tag) {
                    if (html[tag_end - 1u] == '/') {
                        if (!office_append_char(body, '/') ||
                            !office_append_char(body, '>')) return 0;
                    } else if (!office_append_char(body, '/') ||
                               !office_append_char(body, '>')) {
                        return 0;
                    }
                } else if (!office_append_char(body, '>')) {
                    return 0;
                }
                index = tag_end + 1u;
            }
        }
    }
    return 1;
}


static int epub_rewrite_images(OfficeBuffer *body, OfficeDocxImages *images)
{
    OfficeBuffer output;
    size_t index = 0u;
    memset(&output, 0, sizeof(output));
    while (index < body->size) {
        if (body->data[index] != '<') {
            size_t start = index;
            while (index < body->size && body->data[index] != '<') ++index;
            if (!office_append(&output, body->data + start, index - start)) {
                free(output.data);
                return 0;
            }
            continue;
        }
        if (index + 4u < body->size &&
            (body->data[index + 1u] == 'i' || body->data[index + 1u] == 'I') &&
            (body->data[index + 2u] == 'm' || body->data[index + 2u] == 'M') &&
            (body->data[index + 3u] == 'g' || body->data[index + 3u] == 'G') &&
            (body->data[index + 4u] == ' ' || body->data[index + 4u] == '\t' ||
             body->data[index + 4u] == '\r' || body->data[index + 4u] == '\n' ||
             body->data[index + 4u] == '/')) {
            int tag_end = office_find_tag_end(body->data, body->size, index);
            if (tag_end >= 0) {
                size_t end = (size_t)tag_end;
                size_t position;
                for (position = index + 4u; position + 5u < end; ++position) {
                    char quote;
                    size_t value_start;
                    size_t value_end;
                    size_t marker;
                    if (!((body->data[position] == 's' ||
                           body->data[position] == 'S') &&
                          (body->data[position + 1u] == 'r' ||
                           body->data[position + 1u] == 'R') &&
                          (body->data[position + 2u] == 'c' ||
                           body->data[position + 2u] == 'C') &&
                          body->data[position + 3u] == '=' &&
                          (body->data[position + 4u] == '"' ||
                           body->data[position + 4u] == '\'')))
                        continue;
                    quote = (char)body->data[position + 4u];
                    value_start = position + 5u;
                    value_end = value_start;
                    while (value_end < end && body->data[value_end] != quote)
                        ++value_end;
                    if (value_end >= end ||
                        value_end - value_start <= 20u ||
                        memcmp(body->data + value_start, "data:image/", 11u) != 0 ||
                        images->count >= OFFICE_DOCX_MAX_IMAGES)
                        break;
                    for (marker = value_start + 11u;
                         marker + 8u <= value_end; ++marker) {
                        if (memcmp(body->data + marker, ";base64,", 8u) == 0)
                            break;
                    }
                    if (marker + 8u > value_end) break;
                    {
                        size_t raw_size = 0u;
                        int rewritten = 0;
                        uint8_t *raw = office_base64_decode(
                            body->data + marker + 8u,
                            value_end - marker - 8u, &raw_size);
                        if (raw && raw_size > 0u) {
                            QwImage image;
                            uint8_t *png = NULL;
                            size_t png_size = 0u;
                            memset(&image, 0, sizeof(image));
                            if (qw_image_decode(raw, raw_size, &image) ==
                                    QW_IMAGE_OK && image.pixels &&
                                qw_image_encode(&image, QW_IMAGE_ENCODE_PNG,
                                                &png, &png_size) ==
                                    QW_IMAGE_OK && png && png_size) {
                                OfficeDocxImage *slot =
                                    &images->images[images->count];
                                char href[48];
                                int length = snprintf(
                                    href, sizeof(href), "images/image%lu.png",
                                    (unsigned long)images->count);
                                if (length > 0 &&
                                    (size_t)length < sizeof(href) &&
                                    office_append(&output, body->data + index,
                                                  value_start - index) &&
                                    office_append(&output, href,
                                                  (size_t)length) &&
                                    office_append(&output,
                                                  body->data + value_end,
                                                  end + 1u - value_end)) {
                                    slot->png = png;
                                    slot->png_size = png_size;
                                    slot->width = image.width;
                                    slot->height = image.height;
                                    ++images->count;
                                    rewritten = 1;
                                }
                            }
                            qw_image_free(&image);
                            if (!rewritten) free(png);
                        }
                        free(raw);
                        if (rewritten) {
                            index = end + 1u;
                            break;
                        }
                    }
                    break;
                }
                if (index == end + 1u) continue;
            }
        }
        if (!office_append_char(&output, '<')) {
            free(output.data);
            return 0;
        }
        ++index;
    }
    free(body->data);
    *body = output;
    return 1;
}

int qw_doc_convert_epub_memory(const uint8_t *data, size_t size,
                               const char *input_extension, uint8_t **out_data,
                               size_t *out_size, QwConvertReport *report)
{
    static const char container_xml[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:"
        "xmlns:container\"><rootfiles><rootfile full-path=\"OEBPS/"
        "content.opf\" media-type=\"application/oebps-package+xml\"/>"
        "</rootfiles></container>";
    static const char content_opf_prefix[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" "
        "unique-identifier=\"bookid\"><metadata xmlns:dc=\"http://purl.org/"
        "dc/elements/1.1/\"><dc:title>Quirewise document</dc:title>"
        "<dc:identifier id=\"bookid\">urn:uuid:quirewise-html</dc:identifier>"
        "<dc:language>und</dc:language></metadata><manifest>"
        "<item id=\"ncx\" href=\"toc.ncx\" "
        "media-type=\"application/x-dtbncx+xml\"/>"
        "<item id=\"chapter\" href=\"chapter.xhtml\" "
        "media-type=\"application/xhtml+xml\"/>";
    static const char content_opf_suffix[] =
        "</manifest><spine toc=\"ncx\"><itemref idref=\"chapter\"/></spine>"
        "</package>";
    static const char toc_ncx[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\""
        "><head><meta name=\"dtb:uid\" content=\"urn:uuid:quirewise-html\"/>"
        "<meta name=\"dtb:depth\" content=\"1\"/></head><docTitle>"
        "<text>Quirewise document</text></docTitle><navMap><navPoint "
        "id=\"chapter\" playOrder=\"1\"><navLabel><text>Quirewise document"
        "</text></navLabel><content src=\"chapter.xhtml\"/></navPoint>"
        "</navMap></ncx>";
    uint8_t *html = NULL;
    size_t html_size = 0u;
    OfficeBuffer body;
    OfficeBuffer chapter;
    OfficeBuffer opf;
    OfficeDocxImages images;
    QwZipWriter writer;
    int status;

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if ((size >= 5u && memcmp(data, "%PDF-", 5u) == 0) ||
        (input_extension && strcmp(input_extension, "pdf") == 0))
        return qw_doc_office_epub_from_pdf(data, size, NULL, out_data,
                                           out_size, report);
    status = qw_doc_convert_html_memory(data, size, input_extension, &html,
                                        &html_size, report);
    if (status != QW_STATUS_OK) return status;
    memset(&body, 0, sizeof(body));
    memset(&images, 0, sizeof(images));
    if (!epub_strip_document(html, html_size, &body)) {
        free(html);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "EPUB body",
                           "out of memory");
    }
    free(html);
    if (!epub_rewrite_images(&body, &images)) {
        free(body.data);
        docx_images_free(&images);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "EPUB body",
                           "out of memory");
    }
    memset(&chapter, 0, sizeof(chapter));
    if (!office_text(&chapter,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<!DOCTYPE html>\n"
                     "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
                     "<title>Quirewise document</title></head><body>\n") ||
        !office_append(&chapter, body.data, body.size) ||
        !office_text(&chapter, "\n</body></html>\n")) {
        free(body.data);
        free(chapter.data);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "EPUB build",
                           "out of memory");
    }
    free(body.data);
    memset(&opf, 0, sizeof(opf));
    if (!office_text(&opf, content_opf_prefix)) {
        free(chapter.data);
        docx_images_free(&images);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "EPUB build",
                           "out of memory");
    }
    {
        size_t image_index;
        for (image_index = 0u; image_index < images.count; ++image_index) {
            char item[160];
            int length = snprintf(
                item, sizeof(item),
                "<item id=\"img%lu\" href=\"images/image%lu.png\" "
                "media-type=\"image/png\"/>",
                (unsigned long)image_index, (unsigned long)image_index);
            if (length <= 0 || (size_t)length >= sizeof(item) ||
                !office_append(&opf, item, (size_t)length)) {
                free(chapter.data);
                docx_images_free(&images);
                return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY,
                                   "EPUB build", "out of memory");
            }
        }
    }
    if (!office_text(&opf, content_opf_suffix)) {
        free(chapter.data);
        docx_images_free(&images);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "EPUB build",
                           "out of memory");
    }
    if (!office_zip_begin(&writer, 1)) {
        free(chapter.data);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "EPUB ZIP",
                           "cannot start the archive");
    }
    if (!qw_zip_writer_add_mem(&writer, "mimetype",
                               "application/epub+zip", 20u,
                               QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_add_mem(&writer, "META-INF/container.xml",
                               container_xml, strlen(container_xml),
                               QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_add_mem(&writer, "OEBPS/content.opf", opf.data,
                               opf.size, QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_add_mem(&writer, "OEBPS/toc.ncx", toc_ncx,
                               strlen(toc_ncx), QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_add_mem(&writer, "OEBPS/chapter.xhtml", chapter.data,
                               chapter.size, QW_ZIP_NO_COMPRESSION)) {
        qw_zip_writer_end(&writer);
        free(chapter.data);
        docx_images_free(&images);
        free(opf.data);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "EPUB ZIP",
                           "cannot add a package part");
    }
    {
        size_t image_index;
        for (image_index = 0u; image_index < images.count; ++image_index) {
            char name[64];
            int length = snprintf(name, sizeof(name),
                                  "OEBPS/images/image%lu.png",
                                  (unsigned long)image_index);
            if (length <= 0 || (size_t)length >= sizeof(name) ||
                !qw_zip_writer_add_mem(&writer, name,
                                       images.images[image_index].png,
                                       images.images[image_index].png_size,
                                       QW_ZIP_NO_COMPRESSION)) {
                qw_zip_writer_end(&writer);
                free(chapter.data);
                docx_images_free(&images);
                free(opf.data);
                return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY,
                                   "EPUB ZIP", "cannot add an image part");
            }
        }
    }
    if (!office_zip_finish(&writer, out_data, out_size, &chapter)) {
        free(chapter.data);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY, "EPUB ZIP",
                           "cannot finalize the archive");
    }
    free(chapter.data);
    free(opf.data);
    docx_images_free(&images);
    if (report) {
        report->format = QW_FORMAT_EPUB;
        report->output_format = QW_FORMAT_EPUB;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "minimal EPUB 2 package with a single XHTML chapter, NCX "
                 "table of contents and %zu embedded image(s)",
                 images.count);
    }
    return QW_STATUS_OK;
}

/* ------------------------------------------------------------------------- */
/* Text archive / HTMLZ / HWPX                                               */
/* ------------------------------------------------------------------------- */

static int office_negative_status(int status)
{
    switch (status) {
    case 0: return QW_STATUS_OK;
    case -1: return QW_STATUS_ERR_INVALID_ARGUMENT;
    case -2: return QW_STATUS_ERR_OUT_OF_MEMORY;
    case -3: return QW_STATUS_ERR_FORMAT;
    case -4: return QW_STATUS_ERR_UNSUPPORTED;
    case -5: return QW_STATUS_ERR_RESOURCE_LIMIT;
    default: return QW_STATUS_ERR_FORMAT;
    }
}

/* Strip HTML down to readable text: block tags become line breaks, tags are
 * removed, script/style/head content is skipped and common entities are
 * decoded. Used to build a TXTZ index.txt from the HTML intermediate. */
static int office_html_to_text(const uint8_t *html, size_t size,
                               OfficeBuffer *text)
{
    size_t index = 0u;
    int newline_run = 0;

    while (index < size) {
        if (html[index] != '<') {
            size_t start = index;
            while (index < size && html[index] != '<') ++index;
            if (!office_append(text, html + start, index - start)) return 0;
            newline_run = 0;
            continue;
        }
        {
            OfficeTag tag;
            size_t consumed = office_parse_tag(html, size, index, &tag);
            if (!consumed) {
                if (!office_append_char(text, '<')) return 0;
                ++index;
                continue;
            }
            index += consumed;
            if (!tag.name[0]) continue;
            if (strcmp(tag.name, "script") == 0 ||
                strcmp(tag.name, "style") == 0 ||
                strcmp(tag.name, "head") == 0 ||
                strcmp(tag.name, "title") == 0) {
                if (!tag.closing)
                    index = office_skip_element(html, size, index, tag.name);
                continue;
            }
            if (strcmp(tag.name, "br") == 0 || strcmp(tag.name, "p") == 0 ||
                strcmp(tag.name, "div") == 0 || strcmp(tag.name, "li") == 0 ||
                strcmp(tag.name, "tr") == 0 || strcmp(tag.name, "pre") == 0 ||
                strcmp(tag.name, "blockquote") == 0 ||
                (tag.name[0] == 'h' && tag.name[1] >= '1' &&
                 tag.name[1] <= '6' && !tag.name[2])) {
                if (text->size && newline_run < 2 &&
                    !office_append_char(text, '\n'))
                    return 0;
                if (text->size) ++newline_run;
            }
            if (strcmp(tag.name, "td") == 0 || strcmp(tag.name, "th") == 0) {
                if (tag.closing && !office_text(text, "\t")) return 0;
            }
        }
    }
    return 1;
}

int qw_doc_convert_htmlz_memory(const uint8_t *data, size_t size,
                                const char *input_extension,
                                uint8_t **out_data, size_t *out_size,
                                QwConvertReport *report)
{
    uint8_t *html = NULL;
    size_t html_size = 0u;
    int status;

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    status = qw_doc_convert_html_memory(data, size, input_extension, &html,
                                        &html_size, report);
    if (status != QW_STATUS_OK) return status;
    status = office_negative_status(
        (int)qw_htmlz_create(html, html_size, out_data, out_size));
    free(html);
    if (status != QW_STATUS_OK)
        return office_fail(report, status, "HTMLZ write", "");
    if (report) {
        report->output_format = QW_FORMAT_HTMLZ;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "HTMLZ archive with index.html");
    }
    return QW_STATUS_OK;
}

int qw_doc_convert_text_archive_memory(QwTextArchiveKind kind,
                                           const uint8_t *data, size_t size,
                                           const char *input_extension,
                                           uint8_t **out_data,
                                           size_t *out_size,
                                           QwConvertReport *report)
{
    const char *label = kind == QW_TEXT_ARCHIVE_PMLZ ? "PMLZ" : "TXTZ";
    const char *member = kind == QW_TEXT_ARCHIVE_PMLZ ? "index.pml" : "index.txt";
    uint8_t *html = NULL;
    size_t html_size = 0u;
    OfficeBuffer text;
    int status;

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    status = qw_doc_convert_html_memory(data, size, input_extension, &html,
                                        &html_size, report);
    if (status != QW_STATUS_OK) return status;
    memset(&text, 0, sizeof(text));
    if (!office_html_to_text(html, html_size, &text)) {
        free(html);
        return office_fail(report, QW_STATUS_ERR_OUT_OF_MEMORY,
                           "text archive", "out of memory");
    }
    free(html);
    if (!text.size) {
        free(text.data);
        return office_fail(report, QW_STATUS_ERR_FORMAT, "text archive",
                           "the input has no extractable text");
    }
    status = office_negative_status(
        (int)qw_text_archive_create(kind, text.data, text.size, out_data,
                                    out_size));
    free(text.data);
    if (status != QW_STATUS_OK)
        return office_fail(report, status, "text archive write", "");
    if (report) {
        report->output_format = kind == QW_TEXT_ARCHIVE_PMLZ ? QW_FORMAT_PMLZ
                                                              : QW_FORMAT_TXTZ;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "%s archive with %s (text extracted from the HTML "
                 "intermediate)",
                 label, member);
    }
    return QW_STATUS_OK;
}

int qw_doc_convert_hwpx_memory(const uint8_t *data, size_t size,
                               const char *input_extension,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report)
{
    uint8_t *html = NULL;
    size_t html_size = 0u;
    int status;

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    status = qw_doc_convert_html_memory(data, size, input_extension, &html,
                                        &html_size, report);
    if (status != QW_STATUS_OK) return status;
    status = office_negative_status(
        (int)qw_hwpx_create(html, html_size, out_data, out_size));
    free(html);
    if (status != QW_STATUS_OK)
        return office_fail(report, status, "HWPX write", "");
    if (report) {
        report->output_format = QW_FORMAT_HWPX;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "minimal HWPX package with one OWPML section");
    }
    return QW_STATUS_OK;
}
int qw_doc_convert_txtz_memory(const uint8_t *data, size_t size,
                               const char *input_extension,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report)
{
    return qw_doc_convert_text_archive_memory(QW_TEXT_ARCHIVE_TXTZ, data, size,
                                              input_extension, out_data,
                                              out_size, report);
}

int qw_doc_convert_pmlz_memory(const uint8_t *data, size_t size,
                               const char *input_extension,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report)
{
    return qw_doc_convert_text_archive_memory(QW_TEXT_ARCHIVE_PMLZ, data, size,
                                              input_extension, out_data,
                                              out_size, report);
}