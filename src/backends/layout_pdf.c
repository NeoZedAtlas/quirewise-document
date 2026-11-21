/* Internal HTML-to-PDF renderer for Quirewise document formats.
 *
 * This module replaces the former external MuPDF backend for the HTML/layout
 * to PDF path. It intentionally implements a small, well-defined subset:
 *
 * - block structure: p, div, headings h1-h6, li, br, table rows/cells;
 * - inline markup is flattened to text;
 * - UTF-8 text is emitted with Helvetica for ASCII and a predefined Adobe-GB1
 *   CID font (STSong-Light) for non-ASCII runs, so no font file is embedded;
 * - content streams and raw image streams are Flate-compressed when that is
 *   smaller than the raw bytes; deterministic output: no timestamps.
 *
 * The boundary is declared in the private `document_backend.h`; no MuPDF code,
 * header or library is involved. */

#include "document_backend.h"
#include "pdf_font.h"
#include "pdfa_profile.h"

#include <quirewise/compression.h>
#include <quirewise/compression.h>
#include <quirewise/image.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_PDF_MAX_LINES 2000000u
#define QW_PDF_MAX_TEXT_BYTES ((size_t)256u * 1024u * 1024u)

typedef struct QwPdfText {
    char *data;
    size_t size;
    size_t capacity;
} QwPdfText;

typedef struct QwPdfImage {
    uint8_t *stream;      /* DCT payload for JPEG, raw RGB otherwise */
    size_t stream_size;
    uint32_t width;
    uint32_t height;
    int jpeg;
    double display_width; /* CSS pixels / points, 0 selects content width */
} QwPdfImage;

typedef struct QwPdfLines {
    QwPdfText *items;
    unsigned char *heading;
    unsigned char *page_break;
    size_t *image_refs; /* SIZE_MAX for text lines */
    QwPdfImage *images;
    size_t image_count;
    size_t image_capacity;
    size_t count;
    size_t capacity;
    size_t total_bytes;
    char title[256];
    char author[256];
} QwPdfLines;

typedef struct QwPdfOutlineLine {
    size_t line_index;
    size_t page_index;
    unsigned char level;
    double y;
} QwPdfOutlineLine;

typedef struct QwPdfCursor {
    const unsigned char *data;
    size_t size;
    size_t position;
} QwPdfCursor;

static void pdf_text_free(QwPdfText *text)
{
    if (!text) return;
    free(text->data);
    text->data = NULL;
    text->size = 0u;
    text->capacity = 0u;
}

static int pdf_text_reserve(QwPdfText *text, size_t needed)
{
    size_t capacity;
    char *grown;
    if (needed <= text->capacity) return 1;
    capacity = text->capacity ? text->capacity : 256u;
    while (capacity < needed) {
        if (capacity > QW_PDF_MAX_TEXT_BYTES / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > QW_PDF_MAX_TEXT_BYTES) return 0;
    grown = (char *)realloc(text->data, capacity);
    if (!grown) return 0;
    text->data = grown;
    text->capacity = capacity;
    return 1;
}

static int pdf_text_append(QwPdfText *text, const char *bytes, size_t size)
{
    if (!size) return 1;
    if (!pdf_text_reserve(text, text->size + size + 1u)) return 0;
    memcpy(text->data + text->size, bytes, size);
    text->size += size;
    text->data[text->size] = '\0';
    return 1;
}

static int pdf_text_append_byte(QwPdfText *text, char value)
{
    return pdf_text_append(text, &value, 1u);
}

static void pdf_lines_free(QwPdfLines *lines)
{
    size_t index;
    if (!lines) return;
    for (index = 0; index < lines->count; ++index) {
        pdf_text_free(&lines->items[index]);
    }
    free(lines->items);
    free(lines->heading);
    free(lines->page_break);
    free(lines->image_refs);
    for (size_t image = 0u; image < lines->image_count; ++image)
        free(lines->images[image].stream);
    free(lines->images);
    lines->items = NULL;
    lines->heading = NULL;
    lines->page_break = NULL;
    lines->image_refs = NULL;
    lines->images = NULL;
    lines->image_count = 0u;
    lines->image_capacity = 0u;
    lines->count = 0u;
    lines->capacity = 0u;
    lines->total_bytes = 0u;
    lines->title[0] = '\0';
    lines->author[0] = '\0';
}

static int pdf_lines_reserve(QwPdfLines *lines)
{
    size_t capacity;
    QwPdfText *items;
    unsigned char *heading;
    unsigned char *page_break;
    if (lines->count < lines->capacity) return 1;
    if (lines->count >= QW_PDF_MAX_LINES) return 0;
    capacity = lines->capacity ? lines->capacity * 2u : 64u;
    items = (QwPdfText *)realloc(lines->items, capacity * sizeof(*items));
    if (!items) return 0;
    lines->items = items;
    heading = (unsigned char *)realloc(lines->heading, capacity);
    if (!heading) return 0;
    lines->heading = heading;
    page_break = (unsigned char *)realloc(lines->page_break, capacity);
    if (!page_break) return 0;
    lines->page_break = page_break;
    {
        size_t *refs = (size_t *)realloc(lines->image_refs,
                                         capacity * sizeof(*refs));
        if (!refs) return 0;
        lines->image_refs = refs;
    }
    lines->capacity = capacity;
    return 1;
}

static int pdf_lines_flush(QwPdfLines *lines, QwPdfText *current,
                           unsigned char level, int break_before)
{
    if (!current->size) return 1;
    if (lines->total_bytes + current->size > QW_PDF_MAX_TEXT_BYTES) return 0;
    if (!pdf_lines_reserve(lines)) return 0;
    lines->items[lines->count] = *current;
    lines->heading[lines->count] = level;
    lines->page_break[lines->count] = break_before ? 1u : 0u;
    lines->image_refs[lines->count] = SIZE_MAX;
    lines->total_bytes += current->size;
    lines->count += 1u;
    current->data = NULL;
    current->size = 0u;
    current->capacity = 0u;
    return 1;
}

#define QW_PDF_MAX_IMAGES 64u
#define QW_PDF_MAX_IMAGE_BYTES ((size_t)16u * 1024u * 1024u)

static int pdf_base64_value(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

static uint8_t *pdf_base64_decode(const unsigned char *text, size_t size,
                                  size_t *out_size)
{
    uint8_t *out;
    size_t capacity = (size / 4u + 1u) * 3u;
    size_t written = 0u;
    uint32_t accumulator = 0u;
    int bits = 0;
    size_t index;

    if (!size || capacity > QW_PDF_MAX_IMAGE_BYTES) return NULL;
    out = (uint8_t *)malloc(capacity);
    if (!out) return NULL;
    for (index = 0u; index < size; ++index) {
        int value = pdf_base64_value(text[index]);
        if (value < 0) {
            if (text[index] == '=') break;
            if (text[index] == '\n' || text[index] == '\r' ||
                text[index] == ' ' || text[index] == '\t')
                continue;
            free(out);
            return NULL;
        }
        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[written++] = (uint8_t)((accumulator >> bits) & 0xffu);
        }
    }
    *out_size = written;
    return out;
}

static int pdf_tag_attr(const char *tag, size_t size, const char *name,
                        char *out, size_t out_capacity)
{
    size_t name_size = strlen(name);
    size_t index = 0u;

    if (!tag || !name_size || !out || out_capacity < 2u) return 0;
    while (index + name_size + 2u <= size) {
        if (tag[index] != name[0] ||
            memcmp(tag + index, name, name_size) != 0) {
            ++index;
            continue;
        }
        if (index > 0u && tag[index - 1u] != ' ' && tag[index - 1u] != '\t' &&
            tag[index - 1u] != '\n' && tag[index - 1u] != '\r') {
            ++index;
            continue;
        }
        index += name_size;
        while (index < size && (tag[index] == ' ' || tag[index] == '\t'))
            ++index;
        if (index >= size || tag[index] != '=') continue;
        ++index;
        while (index < size && (tag[index] == ' ' || tag[index] == '\t'))
            ++index;
        if (index >= size) return 0;
        if (tag[index] == '"' || tag[index] == '\'') {
            char quote = tag[index++];
            size_t copy = 0u;
            while (index < size && tag[index] != quote && copy + 1u < out_capacity)
                out[copy++] = tag[index++];
            out[copy] = '\0';
            return copy > 0u;
        }
        {
            size_t copy = 0u;
            while (index < size && tag[index] != ' ' && tag[index] != '\t' &&
                   tag[index] != '>' && copy + 1u < out_capacity)
                out[copy++] = tag[index++];
            out[copy] = '\0';
            return copy > 0u;
        }
    }
    return 0;
}

/* Parse an <img src="data:...;base64,..."> tag and append an image line. */
static int pdf_append_image(QwPdfLines *lines, const char *tag, size_t size,
                            QwPdfText *current)
{
    const char *value = NULL;
    const char *value_end = NULL;
    const char *marker = NULL;
    size_t position;
    size_t value_size;
    uint8_t *encoded;
    size_t encoded_size = 0u;
    QwImage image;
    QwPdfImage stored;
    char width_text[32];
    long display = 0;

    if (!lines || !tag || !size) return 1;
    for (position = 0u; position + 5u <= size; ++position) {
        if (tag[position] != 's' || memcmp(tag + position, "src=", 4u) != 0)
            continue;
        if (position > 0u && tag[position - 1u] != ' ' &&
            tag[position - 1u] != '\t' && tag[position - 1u] != '\n')
            continue;
        {
            size_t scan = position + 4u;
            char quote;
            while (scan < size && (tag[scan] == ' ' || tag[scan] == '\t'))
                ++scan;
            if (scan >= size) return 1;
            quote = tag[scan];
            if (quote != '"' && quote != '\'') return 1;
            ++scan;
            value = tag + scan;
            while (scan < size && tag[scan] != quote) ++scan;
            if (scan >= size) return 1;
            value_end = tag + scan;
            break;
        }
    }
    if (!value || value_end <= value) return 1;
    if ((size_t)(value_end - value) < 8u || memcmp(value, "data:", 5u) != 0)
        return 1;
    value_size = (size_t)(value_end - value);
    for (position = 5u; position + 7u <= value_size; ++position) {
        if (memcmp(value + position, "base64,", 7u) == 0) {
            marker = value + position + 7u;
            break;
        }
    }
    if (!marker) return 1;
    encoded = pdf_base64_decode((const unsigned char *)marker,
                                (size_t)(value_end - marker), &encoded_size);
    if (!encoded || !encoded_size) {
        free(encoded);
        return 1;
    }
    memset(&image, 0, sizeof(image));
    if (qw_image_decode(encoded, encoded_size, &image) != QW_IMAGE_OK ||
        !image.pixels || !image.width || !image.height) {
        free(encoded);
        if (image.pixels) qw_image_free(&image);
        return 1;
    }
    memset(&stored, 0, sizeof(stored));
    stored.width = image.width;
    stored.height = image.height;
    stored.display_width = (double)image.width;
    if (pdf_tag_attr(tag, size, "width", width_text, sizeof(width_text))) {
        display = strtol(width_text, NULL, 10);
        if (display > 0) stored.display_width = (double)display;
    }
    if (encoded_size > 2u && encoded[0] == 0xffu && encoded[1] == 0xd8u) {
        stored.stream = encoded;
        stored.stream_size = encoded_size;
        stored.jpeg = 1;
        qw_image_free(&image);
    } else {
        size_t pixel_bytes;
        if (image.width > SIZE_MAX / image.height ||
            image.width * image.height > QW_PDF_MAX_IMAGE_BYTES / 3u) {
            free(encoded);
            qw_image_free(&image);
            return 1;
        }
        pixel_bytes = (size_t)image.width * image.height * 3u;
        stored.stream = (uint8_t *)malloc(pixel_bytes);
        if (!stored.stream) {
            free(encoded);
            qw_image_free(&image);
            return 0;
        }
        for (uint32_t row = 0u; row < image.height; ++row) {
            const uint8_t *source = image.pixels + (size_t)row * image.stride;
            uint8_t *target = stored.stream + (size_t)row * image.width * 3u;
            for (uint32_t column = 0u; column < image.width; ++column) {
                if (image.format == QW_IMAGE_PIXEL_GRAY8) {
                    target[column * 3u] = source[column];
                    target[column * 3u + 1u] = source[column];
                    target[column * 3u + 2u] = source[column];
                } else {
                    target[column * 3u] = source[column * 3u];
                    target[column * 3u + 1u] = source[column * 3u + 1u];
                    target[column * 3u + 2u] = source[column * 3u + 2u];
                }
            }
        }
        stored.stream_size = pixel_bytes;
        stored.jpeg = 0;
        qw_image_free(&image);
        free(encoded);
    }
    if (!pdf_lines_flush(lines, current, 0u, 0)) {
        free(stored.stream);
        return 0;
    }
    if (lines->image_count >= QW_PDF_MAX_IMAGES) {
        free(stored.stream);
        return 1;
    }
    if (lines->image_count >= lines->image_capacity) {
        size_t capacity = lines->image_capacity ? lines->image_capacity * 2u : 8u;
        QwPdfImage *grown = (QwPdfImage *)realloc(
            lines->images, capacity * sizeof(*grown));
        if (!grown) {
            free(stored.stream);
            return 0;
        }
        lines->images = grown;
        lines->image_capacity = capacity;
    }
    lines->images[lines->image_count] = stored;
    if (!pdf_lines_reserve(lines)) return 0;
    memset(&lines->items[lines->count], 0, sizeof(lines->items[lines->count]));
    lines->heading[lines->count] = 0u;
    lines->page_break[lines->count] = 0u;
    lines->image_refs[lines->count] = lines->image_count;
    lines->count += 1u;
    lines->image_count += 1u;
    return 1;
}
static int pdf_is_space(unsigned char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\f' || value == 0x0bu;
}

static int pdf_tag_is_block(const char *name, size_t length)
{
    static const char *const block_tags[] = {
        "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "br", "tr", "td", "th",
        "table", "blockquote", "pre", "dt", "dd", "hr", "section", "article",
        "header", "footer", "figcaption", "caption", "ul", "ol"
    };
    size_t index;
    for (index = 0; index < sizeof(block_tags) / sizeof(block_tags[0]); ++index) {
        size_t candidate = strlen(block_tags[index]);
        size_t offset = 0u;
        if (candidate != length) continue;
        while (offset < length) {
            char left = name[offset];
            char right = block_tags[index][offset];
            if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
            if (left != right) break;
            ++offset;
        }
        if (offset == length) return 1;
    }
    return 0;
}

static unsigned char pdf_heading_level(const char *name, size_t length)
{
    if (length == 2u && (name[0] == 'h' || name[0] == 'H') &&
        name[1] >= '1' && name[1] <= '6') {
        return (unsigned char)(name[1] - '0');
    }
    return 0u;
}

static int pdf_append_utf8(QwPdfText *text, unsigned int code)
{
    char buffer[4];
    size_t length;
    if (code < 0x80u) {
        buffer[0] = (char)code;
        length = 1u;
    } else if (code < 0x800u) {
        buffer[0] = (char)(0xC0u | (code >> 6));
        buffer[1] = (char)(0x80u | (code & 0x3Fu));
        length = 2u;
    } else if (code < 0x10000u) {
        buffer[0] = (char)(0xE0u | (code >> 12));
        buffer[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        buffer[2] = (char)(0x80u | (code & 0x3Fu));
        length = 3u;
    } else if (code <= 0x10FFFFu) {
        buffer[0] = (char)(0xF0u | (code >> 18));
        buffer[1] = (char)(0x80u | ((code >> 12) & 0x3Fu));
        buffer[2] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        buffer[3] = (char)(0x80u | (code & 0x3Fu));
        length = 4u;
    } else {
        return 0;
    }
    return pdf_text_append(text, buffer, length);
}

static int pdf_append_entity(QwPdfText *text, const char *name, size_t length)
{
    if (length == 3u && memcmp(name, "amp", 3u) == 0) return pdf_text_append_byte(text, '&');
    if (length == 2u && memcmp(name, "lt", 2u) == 0) return pdf_text_append_byte(text, '<');
    if (length == 2u && memcmp(name, "gt", 2u) == 0) return pdf_text_append_byte(text, '>');
    if (length == 4u && memcmp(name, "quot", 4u) == 0) return pdf_text_append_byte(text, '"');
    if (length == 4u && memcmp(name, "apos", 4u) == 0) return pdf_text_append_byte(text, '\'');
    if (length == 4u && memcmp(name, "nbsp", 4u) == 0) return pdf_text_append_byte(text, ' ');
    return pdf_text_append(text, "&", 1u) &&
           pdf_text_append(text, name, length) &&
           pdf_text_append(text, ";", 1u);
}

static int pdf_parse_entity(QwPdfText *text, QwPdfCursor *cursor)
{
    size_t start = cursor->position;
    size_t scan = start;
    while (scan < cursor->size && scan - start < 12u &&
           cursor->data[scan] != ';' && !pdf_is_space(cursor->data[scan])) {
        ++scan;
    }
    if (scan >= cursor->size || cursor->data[scan] != ';') {
        cursor->position = start;
        return pdf_text_append_byte(text, '&');
    }
    {
        const char *name = (const char *)(cursor->data + start);
        size_t length = scan - start;
        cursor->position = scan + 1u;
        if (length >= 2u && name[0] == '#') {
            unsigned int code = 0u;
            size_t index = 1u;
            int hex = 0;
            if (name[1] == 'x' || name[1] == 'X') {
                hex = 1;
                index = 2u;
            }
            if (index >= length) return pdf_text_append_byte(text, '?');
            for (; index < length; ++index) {
                char value = name[index];
                unsigned int digit;
                if (hex) {
                    if (value >= '0' && value <= '9') digit = (unsigned int)(value - '0');
                    else if (value >= 'a' && value <= 'f') digit = (unsigned int)(value - 'a' + 10);
                    else if (value >= 'A' && value <= 'F') digit = (unsigned int)(value - 'A' + 10);
                    else return pdf_text_append_byte(text, '?');
                    code = code * 16u + digit;
                } else {
                    if (value < '0' || value > '9') return pdf_text_append_byte(text, '?');
                    code = code * 10u + (unsigned int)(value - '0');
                }
                if (code > 0x10FFFFu) return pdf_text_append_byte(text, '?');
            }
            return pdf_append_utf8(text, code);
        }
        return pdf_append_entity(text, name, length);
    }
}

static int pdf_skip_until(QwPdfCursor *cursor, const char *needle)
{
    size_t needle_length = strlen(needle);
    while (cursor->position + needle_length <= cursor->size) {
        if (memcmp(cursor->data + cursor->position, needle, needle_length) == 0) {
            cursor->position += needle_length;
            return 1;
        }
        cursor->position += 1u;
    }
    cursor->position = cursor->size;
    return 0;
}

static int pdf_parse_html(const char *html, size_t size, QwPdfLines *lines)
{
    QwPdfCursor cursor;
    QwPdfText current = {NULL, 0u, 0u};
    unsigned char level = 0u;
    unsigned char pending_heading = 0u;
    int pending_break = 0;
    int space_pending = 0;
    int ok = 1;

    cursor.data = (const unsigned char *)html;
    cursor.size = size;
    cursor.position = 0u;
    while (cursor.position < cursor.size && ok) {
        unsigned char value = cursor.data[cursor.position];
        if (value == '<') {
            if (cursor.position + 4u <= cursor.size &&
                memcmp(cursor.data + cursor.position, "<!--", 4u) == 0) {
                cursor.position += 4u;
                (void)pdf_skip_until(&cursor, "-->");
                continue;
            }
            {
                size_t start = cursor.position + 1u;
                size_t scan = start;
                int closing = 0;
                size_t name_start;
                size_t name_length;
                if (scan < cursor.size && cursor.data[scan] == '/') {
                    closing = 1;
                    ++scan;
                }
                name_start = scan;
                while (scan < cursor.size && cursor.data[scan] != '>' &&
                       !pdf_is_space(cursor.data[scan])) {
                    ++scan;
                }
                name_length = scan - name_start;
                if (scan >= cursor.size) {
                    cursor.position = cursor.size;
                    break;
                }
                while (scan < cursor.size && cursor.data[scan] != '>') ++scan;
                cursor.position = scan < cursor.size ? scan + 1u : cursor.size;
                if (!closing && name_length == 3u &&
                    (cursor.data[name_start] == 'i' ||
                     cursor.data[name_start] == 'I') &&
                    (cursor.data[name_start + 1u] == 'm' ||
                     cursor.data[name_start + 1u] == 'M') &&
                    (cursor.data[name_start + 2u] == 'g' ||
                     cursor.data[name_start + 2u] == 'G')) {
                    if (!pdf_append_image(
                            lines,
                            (const char *)cursor.data + start - 1u,
                            cursor.position - (start - 1u), &current)) {
                        ok = 0;
                    }
                    continue;
                }
                if (name_length == 5u && !closing &&
                    (cursor.data[name_start] == 't' ||
                     cursor.data[name_start] == 'T') &&
                    memcmp(cursor.data + name_start + 1u, "itle", 4u) == 0) {
                    size_t content_start = cursor.position;
                    size_t content_end = content_start;
                    while (content_end + 6u <= cursor.size) {
                        size_t check = content_end + 1u;
                        if (cursor.data[content_end] != '<') {
                            ++content_end;
                            continue;
                        }
                        if (check < cursor.size && cursor.data[check] == '/')
                            ++check;
                        if (check + 5u <= cursor.size &&
                            (cursor.data[check] == 't' ||
                             cursor.data[check] == 'T') &&
                            (cursor.data[check + 1u] == 'i' ||
                             cursor.data[check + 1u] == 'I') &&
                            (cursor.data[check + 2u] == 't' ||
                             cursor.data[check + 2u] == 'T') &&
                            (cursor.data[check + 3u] == 'l' ||
                             cursor.data[check + 3u] == 'L') &&
                            (cursor.data[check + 4u] == 'e' ||
                             cursor.data[check + 4u] == 'E'))
                            break;
                        ++content_end;
                    }
                    {
                        size_t copy = content_end - content_start;
                        if (copy >= sizeof(lines->title))
                            copy = sizeof(lines->title) - 1u;
                        memcpy(lines->title,
                               (const char *)cursor.data + content_start, copy);
                        lines->title[copy] = '\0';
                    }
                    while (content_end < cursor.size &&
                           cursor.data[content_end] != '>')
                        ++content_end;
                    if (content_end < cursor.size) ++content_end;
                    cursor.position = content_end;
                    continue;
                }
                if (name_length == 4u && !closing &&
                    (cursor.data[name_start] == 'm' ||
                     cursor.data[name_start] == 'M')) {
                    const char *tag_text =
                        (const char *)cursor.data + start - 1u;
                    size_t tag_size = cursor.position - (start - 1u);
                    size_t scan = 0u;
                    int author = 0;
                    while (scan + 4u <= tag_size) {
                        if ((tag_text[scan] == 'n' || tag_text[scan] == 'N') &&
                            (tag_text[scan + 1u] == 'a' ||
                             tag_text[scan + 1u] == 'A') &&
                            (tag_text[scan + 2u] == 'm' ||
                             tag_text[scan + 2u] == 'M') &&
                            (tag_text[scan + 3u] == 'e' ||
                             tag_text[scan + 3u] == 'E') &&
                            (scan + 4u >= tag_size ||
                             tag_text[scan + 4u] == '=' ||
                             tag_text[scan + 4u] == ' ' ||
                             tag_text[scan + 4u] == '\t')) {
                            author = 1;
                            break;
                        }
                        ++scan;
                    }
                    if (author) {
                        size_t probe = 0u;
                        while (probe + 7u <= tag_size) {
                            if (memcmp(tag_text + probe, "content", 7u) == 0) {
                                size_t value = probe + 7u;
                                while (value < tag_size &&
                                       (tag_text[value] == ' ' ||
                                        tag_text[value] == '=' ||
                                        tag_text[value] == '\t'))
                                    ++value;
                                if (value < tag_size &&
                                    (tag_text[value] == '"' ||
                                     tag_text[value] == '\'')) {
                                    char quote = tag_text[value++];
                                    size_t copy = 0u;
                                    while (value < tag_size &&
                                           tag_text[value] != quote &&
                                           copy + 1u < sizeof(lines->author))
                                        lines->author[copy++] =
                                            tag_text[value++];
                                    lines->author[copy] = '\0';
                                }
                                break;
                            }
                            ++probe;
                        }
                    }
                    continue;
                }
                if (name_length == 5u &&
                    memcmp(cursor.data + name_start, "style", 5u) == 0 && !closing) {
                    (void)pdf_skip_until(&cursor, "</style");
                    while (cursor.position < cursor.size &&
                           cursor.data[cursor.position] != '>') {
                        ++cursor.position;
                    }
                    if (cursor.position < cursor.size) ++cursor.position;
                    continue;
                }
                if (name_length == 6u &&
                    memcmp(cursor.data + name_start, "script", 6u) == 0 && !closing) {
                    (void)pdf_skip_until(&cursor, "</script");
                    while (cursor.position < cursor.size &&
                           cursor.data[cursor.position] != '>') {
                        ++cursor.position;
                    }
                    if (cursor.position < cursor.size) ++cursor.position;
                    continue;
                }
                if (name_length && pdf_tag_is_block((const char *)cursor.data + name_start,
                                                    name_length)) {
                    unsigned char heading = pdf_heading_level(
                        (const char *)cursor.data + name_start, name_length);
                    if (!closing && start - 1u < cursor.position) {
                        const char *tag_text =
                            (const char *)cursor.data + start - 1u;
                        size_t tag_size = cursor.position - (start - 1u);
                        size_t scan_style = 0u;
                        while (scan_style + 19u <= tag_size) {
                            if (memcmp(tag_text + scan_style,
                                       "page-break-before", 17u) == 0) {
                                size_t check = scan_style + 17u;
                                while (check < tag_size &&
                                       (tag_text[check] == ' ' ||
                                        tag_text[check] == ':' ||
                                        tag_text[check] == '\t'))
                                    ++check;
                                if (check + 6u <= tag_size &&
                                    memcmp(tag_text + check, "always", 6u) == 0)
                                    pending_break = 1;
                                break;
                            }
                            ++scan_style;
                        }
                    }
                    if (!closing && heading) {
                        pending_heading = heading;
                    }
                    if (!closing && name_length == 2u &&
                        memcmp(cursor.data + name_start, "br", 2u) == 0) {
                        ok = pdf_lines_flush(lines, &current, level, pending_break);
                        continue;
                    }
                    if (current.size) {
                        level = pending_heading;
                        ok = pdf_lines_flush(lines, &current, level, pending_break);
                        pending_break = 0;
                        if (closing || !heading) pending_heading = 0u;
                    } else if (closing && heading) {
                        pending_heading = 0u;
                    }
                }
                continue;
            }
        }
        if (value == '&') {
            cursor.position += 1u;
            ok = pdf_parse_entity(&current, &cursor);
            space_pending = 0;
            continue;
        }
        if (pdf_is_space(value)) {
            ++cursor.position;
            space_pending = 1;
            continue;
        }
        if (space_pending && current.size) {
            ok = ok && pdf_text_append_byte(&current, ' ');
        }
        space_pending = 0;
        {
            size_t start = cursor.position;
            while (cursor.position < cursor.size && cursor.data[cursor.position] != '<' &&
                   cursor.data[cursor.position] != '&' &&
                   !pdf_is_space(cursor.data[cursor.position])) {
                ++cursor.position;
            }
            ok = ok && pdf_text_append(&current,
                                       (const char *)cursor.data + start,
                                       cursor.position - start);
        }
    }
    if (ok) ok = pdf_lines_flush(lines, &current, level, pending_break);
    pdf_text_free(&current);
    return ok;
}

/* ---- layout ---- */

typedef struct QwPdfGlyphRun {
    int ascii;
    size_t offset;
    size_t length;
} QwPdfGlyphRun;

static size_t pdf_decode_utf8(const unsigned char *data, size_t size,
                              unsigned int *code)
{
    unsigned char first;
    if (!size) return 0u;
    first = data[0];
    if (first < 0x80u) { *code = first; return 1u; }
    if ((first & 0xE0u) == 0xC0u && size >= 2u) {
        *code = ((unsigned int)(first & 0x1Fu) << 6) | (data[1] & 0x3Fu);
        return 2u;
    }
    if ((first & 0xF0u) == 0xE0u && size >= 3u) {
        *code = ((unsigned int)(first & 0x0Fu) << 12) |
                ((unsigned int)(data[1] & 0x3Fu) << 6) | (data[2] & 0x3Fu);
        return 3u;
    }
    if ((first & 0xF8u) == 0xF0u && size >= 4u) {
        *code = ((unsigned int)(first & 0x07u) << 18) |
                ((unsigned int)(data[1] & 0x3Fu) << 12) |
                ((unsigned int)(data[2] & 0x3Fu) << 6) | (data[3] & 0x3Fu);
        return 4u;
    }
    *code = 0xFFFDu;
    return 1u;
}

static double pdf_glyph_width(unsigned int code, double font_size,
                                const QwPdfFont *font)
{
    if (font) {
        uint16_t gid = qw_pdf_font_gid(font, code);
        uint16_t advance = qw_pdf_font_advance(font, gid);
        if (advance) return (double)advance / 1000.0 * font_size;
    }
    if (code < 0x80u) {
        if (code == ' ') return 0.28 * font_size;
        if (code == '.' || code == ',' || code == ':' || code == ';' ||
            code == '!' || code == '\'' || code == '|') return 0.28 * font_size;
        return 0.5 * font_size;
    }
    return 1.0 * font_size;
}

static double pdf_font_size(unsigned char heading)
{
    if (heading == 1u) return 18.0;
    if (heading == 2u) return 15.0;
    if (heading >= 3u) return 13.0;
    return 12.0;
}

static int pdf_collect_runs(const char *text, size_t size,
                            QwPdfGlyphRun **out_runs, size_t *out_count)
{
    QwPdfGlyphRun *runs = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t index = 0u;
    while (index < size) {
        unsigned int code = 0u;
        size_t length = pdf_decode_utf8((const unsigned char *)text + index,
                                        size - index, &code);
        int ascii = code < 0x80u;
        size_t next = index + length;
        if (!length) break;
        if (!count || runs[count - 1u].ascii != ascii) {
            if (count >= capacity) {
                size_t grown = capacity ? capacity * 2u : 8u;
                QwPdfGlyphRun *resized =
                    (QwPdfGlyphRun *)realloc(runs, grown * sizeof(*resized));
                if (!resized) { free(runs); return 0; }
                runs = resized;
                capacity = grown;
            }
            runs[count].ascii = ascii;
            runs[count].offset = index;
            runs[count].length = 0u;
            count += 1u;
        }
        runs[count - 1u].length += next - index;
        index = next;
    }
    *out_runs = runs;
    *out_count = count;
    return 1;
}

static int pdf_wrap_line(const char *text, size_t size, double max_width,
                         double font_size, const QwPdfFont *font,
                         size_t **out_breaks, size_t *out_count)
{
    size_t *breaks = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t index = 0u;
    size_t start = 0u;
    size_t last_space = 0u;
    int have_space = 0;
    double width = 0.0;
    while (index < size) {
        unsigned int code = 0u;
        size_t length = pdf_decode_utf8((const unsigned char *)text + index,
                                        size - index, &code);
        double glyph = pdf_glyph_width(code, font_size, font);
        if (width + glyph > max_width && index > start) {
            size_t break_at = have_space ? last_space : index;
            if (count >= capacity) {
                size_t grown = capacity ? capacity * 2u : 16u;
                size_t *resized = (size_t *)realloc(breaks, grown * sizeof(*resized));
                if (!resized) { free(breaks); return 0; }
                breaks = resized;
                capacity = grown;
            }
            breaks[count++] = break_at;
            start = break_at + 1u < size ? break_at + 1u : break_at;
            index = start;
            last_space = 0u;
            have_space = 0;
            width = 0.0;
            while (start < size && text[start] == ' ') { ++start; ++index; }
            continue;
        }
        width += glyph;
        if (code == ' ' || code == '\t') {
            last_space = index;
            have_space = 1;
        }
        index += length;
    }
    *out_breaks = breaks;
    *out_count = count;
    return 1;
}

/* ---- PDF writing ---- */

typedef struct QwPdfOut {
    QwPdfText bytes;
    size_t *offsets;
    size_t offset_count;
    size_t offset_capacity;
} QwPdfOut;

/* PDF/A requires a file identifier in the trailer (ISO 19005-1 clause
   6.1.3). Deriving it from the bytes written so far keeps the output
   reproducible for identical input. */
static void pdf_trailer_id(const QwPdfOut *out, char *buffer, size_t size)
{
    unsigned long long first = 1469598103934665603ull;
    unsigned long long second = 1099511628211ull;
    size_t index;

    for (index = 0u; index < out->bytes.size; ++index) {
        unsigned char value = (unsigned char)out->bytes.data[index];
        first ^= value;
        first *= 1099511628211ull;
        second += first;
        second *= 0x100000001b3ull;
    }
    (void)snprintf(buffer, size, "<%016llx%016llx>", first, second);
}

/* Append UTF-8 text to an XMP buffer, escaping the XML metacharacters a
   document title or author may contain. */
static int pdf_append_xml_text(char *buffer, size_t capacity, size_t *used,
                               const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        const char *escape = NULL;
        size_t escape_size = 0u;
        switch (*cursor) {
        case '&': escape = "&amp;"; escape_size = 5u; break;
        case '<': escape = "&lt;"; escape_size = 4u; break;
        case '>': escape = "&gt;"; escape_size = 4u; break;
        case '"': escape = "&quot;"; escape_size = 6u; break;
        case '\'': escape = "&apos;"; escape_size = 6u; break;
        default: break;
        }
        if (escape) {
            if (*used + escape_size >= capacity) return 0;
            memcpy(buffer + *used, escape, escape_size);
            *used += escape_size;
        } else {
            if (*used + 1u >= capacity) return 0;
            buffer[(*used)++] = (char)*cursor;
        }
        ++cursor;
    }
    return 1;
}

/* Append XMP markup verbatim; only interpolated text goes through
   pdf_append_xml_text. */
static int pdf_append_xml_markup(char *buffer, size_t capacity, size_t *used,
                                 const char *text)
{
    size_t size = strlen(text);
    if (*used + size >= capacity) return 0;
    memcpy(buffer + *used, text, size);
    *used += size;
    return 1;
}

static int pdf_out_append(QwPdfOut *out, const char *bytes, size_t size)
{
    return pdf_text_append(&out->bytes, bytes, size);
}

static int pdf_out_printf(QwPdfOut *out, const char *format, ...)
{
    char buffer[512];
    va_list args;
    int written;
    va_start(args, format);
    written = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written < 0) return 0;
    if ((size_t)written >= sizeof(buffer)) return 0;
    return pdf_out_append(out, buffer, (size_t)written);
}

static int pdf_out_track(QwPdfOut *out, size_t offset)
{
    if (out->offset_count >= out->offset_capacity) {
        size_t grown = out->offset_capacity ? out->offset_capacity * 2u : 32u;
        size_t *resized = (size_t *)realloc(out->offsets, grown * sizeof(*resized));
        if (!resized) return 0;
        out->offsets = resized;
        out->offset_capacity = grown;
    }
    out->offsets[out->offset_count++] = offset;
    return 1;
}

static int pdf_append_hex_ascii(QwPdfOut *out, const char *data, size_t size)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t index;
    for (index = 0; index < size; ++index) {
        unsigned char value = (unsigned char)data[index];
        char pair[2];
        pair[0] = digits[value >> 4];
        pair[1] = digits[value & 0x0Fu];
        if (!pdf_out_append(out, pair, 2u)) return 0;
    }
    return 1;
}

static int pdf_append_hex_utf16(QwPdfOut *out, const char *data, size_t size)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t index = 0u;
    while (index < size) {
        unsigned int code = 0u;
        size_t length = pdf_decode_utf8((const unsigned char *)data + index,
                                        size - index, &code);
        unsigned int units[2];
        size_t unit_count;
        size_t unit_index;
        if (!length) break;
        if (code < 0x10000u) {
            units[0] = code;
            unit_count = 1u;
        } else {
            unsigned int adjusted = code - 0x10000u;
            units[0] = 0xD800u | (adjusted >> 10);
            units[1] = 0xDC00u | (adjusted & 0x3FFu);
            unit_count = 2u;
        }
        for (unit_index = 0; unit_index < unit_count; ++unit_index) {
            char pair[4];
            unsigned int unit = units[unit_index];
            pair[0] = digits[(unit >> 12) & 0x0Fu];
            pair[1] = digits[(unit >> 8) & 0x0Fu];
            pair[2] = digits[(unit >> 4) & 0x0Fu];
            pair[3] = digits[unit & 0x0Fu];
            if (!pdf_out_append(out, pair, 4u)) return 0;
        }
        index += length;
    }
    return 1;
}

static int pdf_write_text_line(QwPdfOut *out, const QwPdfGlyphRun *runs,
                               size_t run_count, const char *text, size_t size,
                               double font_size, double x, double y,
                               const QwPdfFont *font, const uint16_t *gid_map)
{
    size_t run_index;
    double cursor = x;
    uint16_t space_gid = font ? qw_pdf_font_gid(font, 0x20u) : 0u;
    if (!pdf_out_printf(out, "BT\n")) return 0;
    for (run_index = 0; run_index < run_count; ++run_index) {
        const QwPdfGlyphRun *run = &runs[run_index];
        const char *chunk = text + run->offset;
        size_t chunk_index;
        if (font) {
            if (!pdf_out_printf(out, "/F3 %.2f Tf 1 0 0 1 %.2f %.2f Tm <",
                                font_size, cursor, y)) {
                return 0;
            }
            for (chunk_index = 0u; chunk_index < run->length;) {
                unsigned int code = 0u;
                size_t length = pdf_decode_utf8(
                    (const unsigned char *)chunk + chunk_index,
                    run->length - chunk_index, &code);
                uint16_t gid;
                if (!length) break;
                gid = qw_pdf_font_gid(font, code);
                /* PDF/A-2 forbids showing the .notdef glyph: substitute the
                   space glyph and drop the character when even that is
                   unavailable. */
                if (gid == 0u) gid = space_gid;
                if (gid_map) {
                    gid = gid_map[gid];
                    if (gid == 0xFFFFu) gid = 0u;
                }
                if (gid == 0u) {
                    chunk_index += length;
                    continue;
                }
                if (!pdf_out_printf(out, "%04X", (unsigned)gid)) return 0;
                chunk_index += length;
            }
        } else {
            if (!pdf_out_printf(out, "%s %.2f Tf 1 0 0 1 %.2f %.2f Tm <",
                                run->ascii ? "/F1" : "/F2", font_size, cursor, y)) {
                return 0;
            }
            if (run->ascii) {
                if (!pdf_append_hex_ascii(out, chunk, run->length)) return 0;
            } else {
                if (!pdf_append_hex_utf16(out, chunk, run->length)) return 0;
            }
        }
        if (!pdf_out_printf(out, "> Tj\n")) return 0;
        for (chunk_index = 0u; chunk_index < run->length;) {
            unsigned int code = 0u;
            size_t length = pdf_decode_utf8(
                (const unsigned char *)chunk + chunk_index,
                run->length - chunk_index, &code);
            if (!length) break;
            cursor += pdf_glyph_width(code, font_size, font);
            chunk_index += length;
        }
    }
    return pdf_out_printf(out, "ET\n");
}

static int pdf_build_xobject_resources(const QwPdfLines *lines, size_t base,
                                       QwPdfText *out)
{
    size_t image;

    memset(out, 0, sizeof(*out));
    if (!lines->image_count) return 1;
    if (!pdf_text_append(out, " /XObject <<", 12u)) return 0;
    for (image = 0u; image < lines->image_count; ++image) {
        char entry[48];
        int length = snprintf(entry, sizeof(entry), " /Im%zu %zu 0 R", image,
                              base + image);
        if (length <= 0 || (size_t)length >= sizeof(entry) ||
            !pdf_text_append(out, entry, (size_t)length))
            return 0;
    }
    return pdf_text_append(out, " >>", 3u);
}

typedef struct QwPdfFontUse {
    uint16_t gid;
    unsigned int codepoint;
} QwPdfFontUse;

static int pdf_font_collect_uses(const QwPdfFont *font, const QwPdfLines *lines,
                                 QwPdfFontUse **out_uses, size_t *out_count)
{
    QwPdfFontUse *uses = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t item;
    for (item = 0u; item < lines->count; ++item) {
        const QwPdfText *text = &lines->items[item];
        size_t index = 0u;
        while (index < text->size) {
            unsigned int code = 0u;
            size_t length = pdf_decode_utf8(
                (const unsigned char *)text->data + index,
                text->size - index, &code);
            uint16_t gid;
            size_t scan;
            int seen = 0;
            if (!length) break;
            gid = qw_pdf_font_gid(font, code);
            for (scan = 0u; scan < count; ++scan) {
                if (uses[scan].gid == gid) { seen = 1; break; }
            }
            if (!seen && count < 4096u) {
                if (count == capacity) {
                    size_t grown = capacity ? capacity * 2u : 64u;
                    QwPdfFontUse *resized = (QwPdfFontUse *)realloc(
                        uses, grown * sizeof(*resized));
                    if (!resized) { free(uses); return 0; }
                    uses = resized;
                    capacity = grown;
                }
                uses[count].gid = gid;
                uses[count].codepoint = code;
                count += 1u;
            }
            index += length;
        }
    }
    *out_uses = uses;
    *out_count = count;
    return 1;
}

static int pdf_write_embedded_font(QwPdfOut *out, const QwPdfFont *font,
                                   const QwPdfFontSubset *subset,
                                   const QwPdfFontUse *uses, size_t use_count,
                                   size_t base)
{
    size_t type0 = base;
    size_t cid = base + 1u;
    size_t descriptor = base + 2u;
    size_t fontfile = base + 3u;
    size_t tounicode = base + 4u;
    int ascent = 0;
    int descent = 0;
    int cap_height = 0;
    int x_min = 0;
    int y_min = 0;
    int x_max = 0;
    int y_max = 0;
    const uint8_t *font_data;
    size_t font_size = 0u;
    size_t bound;
    size_t compressed_size = 0u;
    uint8_t *compressed = NULL;
    char *widths = NULL;
    size_t widths_capacity;
    size_t widths_size = 0u;
    char *cmap = NULL;
    size_t cmap_capacity;
    size_t cmap_size = 0u;
    size_t index;
    int cff;
    int ok = 1;

    if (!font || !subset || !subset->data || !subset->size) return 0;
    cff = qw_pdf_font_is_cff(font);
    qw_pdf_font_metrics(font, &ascent, &descent, &cap_height);
    qw_pdf_font_bbox(font, &x_min, &y_min, &x_max, &y_max);
    font_data = subset->data;
    font_size = subset->size;

    widths_capacity = use_count * 32u + 16u;
    cmap_capacity = use_count * 32u + 512u;
    widths = (char *)malloc(widths_capacity);
    cmap = (char *)malloc(cmap_capacity);
    if (!widths || !cmap) ok = 0;

    for (index = 0u; ok && index < use_count; ++index) {
        unsigned int old_gid = uses[index].gid;
        unsigned int new_gid = subset->map[old_gid];
        int written;
        if (new_gid == 0xFFFFu) continue;
        written = snprintf(widths + widths_size, widths_capacity - widths_size,
                           " %u [%u]", new_gid,
                           (unsigned)qw_pdf_font_advance(font, (uint16_t)old_gid));
        if (written < 0 || (size_t)written >= widths_capacity - widths_size) {
            ok = 0;
            break;
        }
        widths_size += (size_t)written;
    }
    if (ok) {
        int written = snprintf(
            cmap, cmap_capacity,
            "/CIDInit /ProcSet findresource begin\n"
            "12 dict begin\n"
            "begincmap\n"
            "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
            "/CMapName /Adobe-Identity-UCS def\n"
            "/CMapType 2 def\n"
            "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n"
            "%zu beginbfchar\n",
            use_count);
        if (written < 0) ok = 0;
        else cmap_size = (size_t)written;
    }
    for (index = 0u; ok && index < use_count; ++index) {
        unsigned int code = uses[index].codepoint;
        unsigned int new_gid = subset->map[uses[index].gid];
        int written;
        if (new_gid == 0xFFFFu) continue;
        if (code <= 0xFFFFu) {
            written = snprintf(cmap + cmap_size, cmap_capacity - cmap_size,
                               "<%04X> <%04X>\n", new_gid, code);
        } else {
            unsigned int high = 0xD800u + ((code - 0x10000u) >> 10);
            unsigned int low = 0xDC00u + ((code - 0x10000u) & 0x3FFu);
            written = snprintf(cmap + cmap_size, cmap_capacity - cmap_size,
                               "<%04X> <%04X%04X>\n", new_gid, high, low);
        }
        if (written < 0 || (size_t)written >= cmap_capacity - cmap_size) {
            ok = 0;
            break;
        }
        cmap_size += (size_t)written;
    }
    if (ok) {
        static const char tail[] =
            "endbfchar\nendcmap\nCMapName currentdict /CMap defineresource pop\n"
            "end\nend\n";
        if (cmap_capacity - cmap_size <= sizeof(tail)) ok = 0;
        else {
            memcpy(cmap + cmap_size, tail, sizeof(tail) - 1u);
            cmap_size += sizeof(tail) - 1u;
        }
    }

    bound = qw_compression_zlib_bound(font_size);
    if (bound != SIZE_MAX) compressed = (uint8_t *)malloc(bound);
    if (compressed &&
        (!qw_compression_deflate_zlib(font_data, font_size, compressed, bound,
                                      &compressed_size) ||
         compressed_size >= font_size)) {
        free(compressed);
        compressed = NULL;
        compressed_size = 0u;
    }

    if (ok) {
        ok = pdf_out_track(out, out->bytes.size) &&
             pdf_out_printf(out,
                 "%zu 0 obj\n<< /Type /Font /Subtype /Type0 "
                 "/BaseFont /QWEmbeddedFont /Encoding /Identity-H "
                 "/DescendantFonts [%zu 0 R] /ToUnicode %zu 0 R >>\nendobj\n",
                 type0, cid, tounicode);
    }
    if (ok) {
        const char *subtype = cff ? "CIDFontType0" : "CIDFontType2";
        ok = pdf_out_track(out, out->bytes.size) &&
             pdf_out_printf(out,
                 "%zu 0 obj\n<< /Type /Font /Subtype /%s "
                 "/BaseFont /QWEmbeddedFont /CIDSystemInfo << /Registry (Adobe) "
                 "/Ordering (Identity) /Supplement 0 >> /FontDescriptor %zu 0 R "
                 "/CIDToGIDMap /Identity /DW 1000 /W [%s] >>\nendobj\n",
                 cid, subtype, descriptor, widths ? widths : "");
    }
    if (ok) {
        ok = pdf_out_track(out, out->bytes.size) &&
             pdf_out_printf(out,
                 "%zu 0 obj\n<< /Type /FontDescriptor /FontName /QWEmbeddedFont "
                 "/Flags 4 /FontBBox [%d %d %d %d] /ItalicAngle 0 /Ascent %d "
                 "/Descent %d /CapHeight %d /StemV 80 /%s %zu 0 R >>\nendobj\n",
                 descriptor, x_min, y_min, x_max, y_max, ascent, descent,
                 cap_height, cff ? "FontFile3" : "FontFile2", fontfile);
    }
    if (ok) {
        const uint8_t *payload = compressed ? compressed : font_data;
        size_t payload_size = compressed ? compressed_size : font_size;
        ok = pdf_out_track(out, out->bytes.size) &&
             pdf_out_printf(out,
                            cff
                                ? "%zu 0 obj\n<< /Length %zu /Length1 %zu /Subtype /OpenType%s >>\nstream\n"
                                : "%zu 0 obj\n<< /Length %zu /Length1 %zu%s >>\nstream\n",
                            fontfile, payload_size, font_size,
                            compressed ? " /Filter /FlateDecode" : "") &&
             !(payload_size && !pdf_out_append(out, payload, payload_size)) &&
             pdf_out_printf(out, "\nendstream\nendobj\n");
    }
    if (ok) {
        ok = pdf_out_track(out, out->bytes.size) &&
             pdf_out_printf(out, "%zu 0 obj\n<< /Length %zu >>\nstream\n",
                            tounicode, cmap_size) &&
             !(cmap_size && !pdf_out_append(out, cmap, cmap_size)) &&
             pdf_out_printf(out, "\nendstream\nendobj\n");
    }

    free(compressed);
    free(widths);
    free(cmap);
    return ok;
}

static int pdf_emit_document(const QwPdfLines *lines, double page_width,
                             double page_height, const QwPdfFont *font,
                             int pdfa, QwPdfText *output, size_t *out_pages)
{
    const double margin = 56.0;
    const double line_gap = 1.22;
    typedef struct QwPdfSegment {
        size_t line_index;
        size_t start;
        size_t end;
        size_t image_index; /* SIZE_MAX for text */
        double font_size;
        double space;      /* vertical space consumed, in points */
        int break_before;
    } QwPdfSegment;
    QwPdfOut out;
    QwPdfSegment *segments = NULL;
    QwPdfOutlineLine *outline_entries = NULL;
    size_t outline_count = 0u;
    size_t segment_count = 0u;
    size_t segment_capacity = 0u;
    size_t *page_first = NULL;
    size_t *page_count_lines = NULL;
    size_t page_capacity_total = 0u;
    size_t page_count = 0u;
    size_t page_line_capacity;
    size_t line_index;
    size_t info_object_flag = 0u;
    size_t object_total_no_font = 0u;
    size_t font_object_base = 0u;
    size_t pdfa_object_base = 0u;
    QwPdfFontSubset subset;
    QwPdfFontUse *font_uses = NULL;
    size_t font_use_count = 0u;
    int ok = 1;

    memset(&out, 0, sizeof(out));
    memset(&subset, 0, sizeof(subset));
    page_line_capacity = (size_t)((page_height - 2.0 * margin) / (12.0 * line_gap));
    if (page_line_capacity < 8u) page_line_capacity = 8u;

    /* Pass 1: wrap every logical line into layout segments. Image lines
     * become one segment sized to the content width. */
    for (line_index = 0u; ok && line_index < lines->count; ++line_index) {
        const QwPdfText *item = &lines->items[line_index];
        double font_size = pdf_font_size(lines->heading[line_index]);
        size_t *breaks = NULL;
        if (lines->image_refs && lines->image_refs[line_index] != SIZE_MAX) {
            const QwPdfImage *image =
                &lines->images[lines->image_refs[line_index]];
            double content_width = page_width - 2.0 * margin;
            double width = image->display_width > 0.0
                               ? image->display_width : content_width;
            double height;
            if (width > content_width) width = content_width;
            if (!image->width) continue;
            height = width * (double)image->height / (double)image->width;
            if (height > page_height - 2.0 * margin)
                height = page_height - 2.0 * margin;
            if (segment_count >= segment_capacity) {
                size_t grown = segment_capacity ? segment_capacity * 2u : 64u;
                QwPdfSegment *resized = (QwPdfSegment *)realloc(
                    segments, grown * sizeof(*resized));
                if (!resized) { ok = 0; break; }
                segments = resized;
                segment_capacity = grown;
            }
            segments[segment_count].line_index = line_index;
            segments[segment_count].start = 0u;
            segments[segment_count].end = 0u;
            segments[segment_count].image_index =
                lines->image_refs[line_index];
            segments[segment_count].font_size = 0.0;
            segments[segment_count].space = height + 6.0;
            segments[segment_count].break_before =
                lines->page_break[line_index] != 0u;
            segment_count += 1u;
            continue;
        }
        size_t break_count = 0u;
        size_t segment_start = 0u;
        int line_break = lines->page_break[line_index] != 0u;
        size_t break_index;
        if (!pdf_wrap_line(item->data ? item->data : "", item->size,
                           page_width - 2.0 * margin, font_size, font,
                           &breaks, &break_count)) {
            ok = 0;
            break;
        }
        for (break_index = 0u; ok && break_index <= break_count; ++break_index) {
            size_t segment_end = break_index < break_count
                ? breaks[break_index] : item->size;
            if (segment_count >= segment_capacity) {
                size_t grown = segment_capacity ? segment_capacity * 2u : 64u;
                QwPdfSegment *resized =
                    (QwPdfSegment *)realloc(segments, grown * sizeof(*resized));
                if (!resized) { ok = 0; break; }
                segments = resized;
                segment_capacity = grown;
            }
            segments[segment_count].line_index = line_index;
            segments[segment_count].start = segment_start;
            segments[segment_count].end = segment_end;
            segments[segment_count].image_index = SIZE_MAX;
            segments[segment_count].font_size = font_size;
            segments[segment_count].space = font_size * line_gap;
            segments[segment_count].break_before =
                break_index == 0u && line_break;
            segment_count += 1u;
            segment_start = segment_end < item->size ? segment_end + 1u : item->size;
            if (segment_start > item->size) segment_start = item->size;
        }
        free(breaks);
    }

    if (font) {
        uint16_t *gids = NULL;
        unsigned int *codepoints = NULL;
        if (!pdf_font_collect_uses(font, lines, &font_uses, &font_use_count)) {
            ok = 0;
        } else {
            size_t use_index;
            gids = (uint16_t *)malloc(font_use_count * sizeof(uint16_t));
            codepoints = (unsigned int *)malloc(font_use_count *
                                                sizeof(unsigned int));
            if (!gids || !codepoints) {
                ok = 0;
            } else {
                for (use_index = 0u; use_index < font_use_count; ++use_index) {
                    gids[use_index] = font_uses[use_index].gid;
                    codepoints[use_index] = font_uses[use_index].codepoint;
                }
                if (!qw_pdf_font_subset(font, gids, codepoints, font_use_count,
                                        &subset)) {
                    ok = 0;
                }
            }
        }
        free(gids);
        free(codepoints);
    }

    /* Pass 2: pack segments into pages by their vertical space. */
    {
        double usable = page_height - 2.0 * margin;
        double minimum = (double)page_line_capacity * 12.0 * line_gap;
        if (usable < minimum) usable = minimum;
        for (line_index = 0u; ok && line_index < segment_count;) {
            size_t first_segment = line_index;
            double used = 0.0;

            while (line_index < segment_count) {
                double space = segments[line_index].space;
                if (used > 0.0 && (segments[line_index].break_before ||
                                  used + space > usable))
                    break;
                used += space;
                line_index += 1u;
            }
            if (page_count >= page_capacity_total) {
                size_t grown = page_capacity_total ? page_capacity_total * 2u : 16u;
                size_t *first = (size_t *)realloc(page_first, grown * sizeof(*first));
                size_t *counts;
                if (!first) { ok = 0; break; }
                page_first = first;
                counts = (size_t *)realloc(page_count_lines, grown * sizeof(*counts));
                if (!counts) { ok = 0; break; }
                page_count_lines = counts;
                page_capacity_total = grown;
            }
            page_first[page_count] = first_segment;
            page_count_lines[page_count] = line_index - first_segment;
            page_count += 1u;
        }
    }

    {
        size_t *line_page = NULL;
        size_t heading_total = 0u;
        size_t index;
        for (index = 0u; index < lines->count; ++index) {
            if (lines->heading[index]) ++heading_total;
        }
        if (heading_total) {
            line_page = (size_t *)malloc(lines->count * sizeof(*line_page));
            if (!line_page) {
                ok = 0;
            } else {
                for (index = 0u; index < lines->count; ++index)
                    line_page[index] = SIZE_MAX;
                for (index = 0u; index < page_count; ++index) {
                    size_t first = page_first[index];
                    size_t stop = first + page_count_lines[index];
                    size_t seg;
                    for (seg = first; seg < stop; ++seg) {
                        size_t line_index = segments[seg].line_index;
                        if (line_page[line_index] == SIZE_MAX)
                            line_page[line_index] = index;
                    }
                }
                outline_entries = (QwPdfOutlineLine *)malloc(
                    heading_total * sizeof(*outline_entries));
                if (!outline_entries) {
                    ok = 0;
                } else {
                    for (index = 0u; index < lines->count; ++index) {
                        if (!lines->heading[index] ||
                            line_page[index] == SIZE_MAX)
                            continue;
                        outline_entries[outline_count].line_index = index;
                        outline_entries[outline_count].page_index =
                            line_page[index];
                        outline_entries[outline_count].level =
                            lines->heading[index];
                        outline_entries[outline_count].y =
                            page_height - margin - 12.0;
                        outline_count += 1u;
                    }
                }
            }
            free(line_page);
        }
    }

    if (!ok) {
        free(segments);
        free(page_first);
        free(page_count_lines);
        free(outline_entries);
        return 0;
    }

    {
        size_t font_ascii = 3u;
        size_t font_cjk = 4u;
        size_t font_descendant = 5u;
        size_t font_descriptor = 6u;
        size_t pages_object = 2u;
        size_t first_page_object = 7u;
        size_t image_object_base = first_page_object + page_count * 2u;
        size_t outline_object_base = image_object_base + lines->image_count;
        size_t outline_root_object = outline_object_base + outline_count;
        info_object_flag = (lines->title[0] || lines->author[0]) ? 1u : 0u;
        object_total_no_font = 6u + (page_count ? page_count : 1u) * 2u +
                               lines->image_count +
                               (outline_count ? outline_count + 1u : 0u) +
                               info_object_flag;
        font_object_base = object_total_no_font + 1u;
        pdfa_object_base = font_object_base + (font ? 5u : 0u);
        char font_dict[128];
        QwPdfText resources;
        size_t object;
        if (font) {
            snprintf(font_dict, sizeof(font_dict), "/Font << /F3 %zu 0 R >>",
                     font_object_base);
        } else {
            snprintf(font_dict, sizeof(font_dict),
                     "/Font << /F1 %zu 0 R /F2 %zu 0 R >>", font_ascii, font_cjk);
        }

        ok = pdf_out_printf(&out, "%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n");
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        {
            char outline_ref[96] = "";
            char pdfa_ref[160] = "";
            if (outline_count)
                snprintf(outline_ref, sizeof(outline_ref),
                         " /Outlines %zu 0 R /PageMode /UseOutlines",
                         outline_root_object);
            if (pdfa)
                snprintf(pdfa_ref, sizeof(pdfa_ref),
                         " /Metadata %zu 0 R /OutputIntents [%zu 0 R]",
                         pdfa_object_base, pdfa_object_base + 2u);
            ok = pdf_out_printf(&out,
                                "1 0 obj\n<< /Type /Catalog /Pages %zu 0 R%s%s "
                                ">>\nendobj\n",
                                pages_object, outline_ref, pdfa_ref);
        }
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) {
            size_t kid;
            ok = pdf_out_printf(&out, "2 0 obj\n<< /Type /Pages /Count %zu /Kids [",
                                page_count ? page_count : 1u);
            for (kid = 0u; ok && kid < page_count; ++kid) {
                ok = pdf_out_printf(&out, "%zu 0 R ", first_page_object + kid * 2u);
            }
            if (ok) ok = pdf_out_printf(&out, "] >>\nendobj\n");
        }
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        /* Objects 3-6 are the built-in Helvetica/STSong-Light pair. When an
           embedded font is in use they are not referenced by any page, and a
           PDF/A validator still rejects the unembedded font dictionaries, so
           the slots stay in place but become null. */
        if (ok) ok = pdf_out_printf(&out, "%s",
            font ? "3 0 obj\nnull\nendobj\n"
                 : "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                   "/Encoding /WinAnsiEncoding >>\nendobj\n");
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) ok = pdf_out_printf(&out, "%s",
            font ? "4 0 obj\nnull\nendobj\n"
                 : "4 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light "
                   "/Encoding /UniGB-UCS2-H /DescendantFonts [%zu 0 R] >>\nendobj\n",
            font_descendant);
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) ok = pdf_out_printf(&out, "%s",
            font ? "5 0 obj\nnull\nendobj\n"
                 : "5 0 obj\n<< /Type /Font /Subtype /CIDFontType0 /BaseFont /STSong-Light "
                   "/CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 4 >> "
                   "/FontDescriptor %zu 0 R /DW 1000 >>\nendobj\n", font_descriptor);
        if (ok) ok = pdf_out_track(&out, out.bytes.size);
        if (ok) ok = pdf_out_printf(&out, "%s",
            font ? "6 0 obj\nnull\nendobj\n"
                 : "6 0 obj\n<< /Type /FontDescriptor /FontName /STSong-Light /Flags 6 "
                   "/FontBBox [-25 -254 1000 880] /ItalicAngle 0 /Ascent 880 /Descent -120 "
                   "/CapHeight 880 /StemV 80 >>\nendobj\n");

        if (ok && !pdf_build_xobject_resources(lines, image_object_base,
                                               &resources)) {
            ok = 0;
        }
        for (object = 0u; ok && object < page_count; ++object) {
            QwPdfOut stream;
            size_t first = page_first[object];
            size_t stop = first + page_count_lines[object];
            double y = page_height - margin - 12.0;
            size_t segment_index;
            memset(&stream, 0, sizeof(stream));
            for (segment_index = first; ok && segment_index < stop; ++segment_index) {
                const QwPdfSegment *segment = &segments[segment_index];
                const QwPdfText *item = &lines->items[segment->line_index];
                const char *text = (item->data ? item->data : "") + segment->start;
                if (segment->image_index != SIZE_MAX) {
                    const QwPdfImage *image =
                        &lines->images[segment->image_index];
                    double content_width = page_width - 2.0 * margin;
                    double width = image->display_width > 0.0
                                       ? image->display_width : content_width;
                    double height;
                    if (width > content_width) width = content_width;
                    if (!image->width) continue;
                    height = width * (double)image->height /
                             (double)image->width;
                    if (height > page_height - 2.0 * margin)
                        height = page_height - 2.0 * margin;
                    y -= height;
                    if (!pdf_out_printf(
                            &stream, "q %.2f 0 0 %.2f %.2f %.2f cm /Im%zu Do Q\n",
                            width, height, margin, y, segment->image_index)) {
                        ok = 0;
                        break;
                    }
                    y -= 6.0;
                    continue;
                }
                {
                    size_t outline_index;
                    for (outline_index = 0u; outline_index < outline_count;
                         ++outline_index) {
                        if (outline_entries[outline_index].line_index ==
                            segment->line_index) {
                            outline_entries[outline_index].y =
                                y + segment->font_size;
                            break;
                        }
                    }
                }
                size_t size = segment->end > segment->start
                    ? segment->end - segment->start : 0u;
                QwPdfGlyphRun *runs = NULL;
                size_t run_count = 0u;
                if (!pdf_collect_runs(text, size, &runs, &run_count)) { ok = 0; break; }
                if (!pdf_write_text_line(&stream, runs, run_count, text, size,
                                         segment->font_size, margin, y, font,
                                         font ? subset.map : NULL)) {
                    free(runs);
                    ok = 0;
                    break;
                }
                free(runs);
                y -= segment->font_size * line_gap;
            }
            if (!ok) { free(stream.bytes.data); free(stream.offsets); break; }
            {
                size_t page_object = first_page_object + object * 2u;
                size_t content_object = page_object + 1u;
                size_t content_size = stream.bytes.size;
                size_t compressed_size = 0u;
                uint8_t *compressed = NULL;
                int deflated = 0;
                if (content_size) {
                    size_t bound = qw_compression_zlib_bound(content_size);
                    if (bound != SIZE_MAX)
                        compressed = (uint8_t *)malloc(bound);
                    if (compressed &&
                        qw_compression_deflate_zlib(stream.bytes.data,
                                                   content_size, compressed,
                                                   bound, &compressed_size) &&
                        compressed_size < content_size) {
                        deflated = 1;
                    }
                }
                if (!pdf_out_track(&out, out.bytes.size) ||
                    !pdf_out_printf(&out,
                        "%zu 0 obj\n<< /Type /Page /Parent %zu 0 R /MediaBox [0 0 %.2f %.2f] "
                        "/Resources << %s%s >> "
                        "/Contents %zu 0 R >>\nendobj\n",
                        page_object, pages_object, page_width, page_height,
                        font_dict,
                        resources.data ? (const char *)resources.data : "",
                        content_object) ||
                    !pdf_out_track(&out, out.bytes.size) ||
                    !pdf_out_printf(&out,
                        "%zu 0 obj\n<< /Length %zu%s >>\nstream\n",
                        content_object,
                        deflated ? compressed_size : content_size,
                        deflated ? " /Filter /FlateDecode" : "") ||
                    (deflated
                         ? (compressed_size &&
                            !pdf_out_append(&out, compressed, compressed_size))
                         : (content_size &&
                            !pdf_out_append(&out, stream.bytes.data,
                                            content_size))) ||
                    !pdf_out_printf(&out, "\nendstream\nendobj\n")) {
                    ok = 0;
                }
                free(compressed);
            }
            free(stream.bytes.data);
            free(stream.offsets);
        }
        for (object = 0u; ok && object < lines->image_count; ++object) {
            const QwPdfImage *image = &lines->images[object];
            size_t image_object = image_object_base + object;
            size_t stream_size = image->stream_size;
            size_t compressed_size = 0u;
            uint8_t *compressed = NULL;
            int deflated = 0;
            if (!image->jpeg && stream_size) {
                size_t bound = qw_compression_zlib_bound(stream_size);
                if (bound != SIZE_MAX) compressed = (uint8_t *)malloc(bound);
                if (compressed &&
                    qw_compression_deflate_zlib(image->stream, stream_size,
                                               compressed, bound,
                                               &compressed_size) &&
                    compressed_size < stream_size) {
                    deflated = 1;
                }
            }
            if (!pdf_out_track(&out, out.bytes.size) ||
                !pdf_out_printf(&out,
                    "%zu 0 obj\n<< /Type /XObject /Subtype /Image /Width %u "
                    "/Height %u /ColorSpace /DeviceRGB /BitsPerComponent 8%s "
                    "/Length %zu >>\nstream\n",
                    image_object, image->width, image->height,
                    image->jpeg ? " /Filter /DCTDecode"
                                : (deflated ? " /Filter /FlateDecode" : ""),
                    deflated ? compressed_size : stream_size) ||
                ((deflated ? compressed_size : stream_size) &&
                 !pdf_out_append(&out, deflated ? compressed : image->stream,
                                 deflated ? compressed_size : stream_size)) ||
                !pdf_out_printf(&out, "\nendstream\nendobj\n")) {
                ok = 0;
            }
            free(compressed);
        }
        /* Build the heading tree: each item's parent is the nearest prior
         * heading with a lower level. */
        {
            size_t *parent = NULL;
            size_t *first_child = NULL;
            size_t *last_child = NULL;
            size_t *next_sibling = NULL;
            size_t *prev_sibling = NULL;
            size_t *child_count = NULL;
            size_t stack[7];
            size_t stack_depth = 0u;
            size_t item;
            if (outline_count) {
                parent = (size_t *)malloc(outline_count * sizeof(*parent));
                first_child = (size_t *)malloc(outline_count *
                                               sizeof(*first_child));
                last_child = (size_t *)malloc(outline_count *
                                              sizeof(*last_child));
                next_sibling = (size_t *)malloc(outline_count *
                                                sizeof(*next_sibling));
                prev_sibling = (size_t *)malloc(outline_count *
                                                sizeof(*prev_sibling));
                child_count = (size_t *)calloc(outline_count,
                                               sizeof(*child_count));
                if (!parent || !first_child || !last_child || !next_sibling ||
                    !prev_sibling || !child_count) {
                    ok = 0;
                }
                for (item = 0u; ok && item < outline_count; ++item) {
                    size_t level = outline_entries[item].level;
                    size_t parent_item;
                    if (level == 0u) level = 1u;
                    if (level > 6u) level = 6u;
                    while (stack_depth &&
                           outline_entries[stack[stack_depth - 1u]].level >=
                               level)
                        --stack_depth;
                    parent_item = stack_depth ? stack[stack_depth - 1u]
                                              : SIZE_MAX;
                    parent[item] = parent_item;
                    first_child[item] = SIZE_MAX;
                    last_child[item] = SIZE_MAX;
                    next_sibling[item] = SIZE_MAX;
                    prev_sibling[item] = SIZE_MAX;
                    if (parent_item == SIZE_MAX) {
                        if (outline_count && stack_depth == 0u) {
                            /* Top-level siblings are chained through item 0. */
                        }
                    } else {
                        if (first_child[parent_item] == SIZE_MAX)
                            first_child[parent_item] = item;
                        if (last_child[parent_item] != SIZE_MAX)
                            next_sibling[last_child[parent_item]] = item;
                        prev_sibling[item] = last_child[parent_item];
                        last_child[parent_item] = item;
                        child_count[parent_item] += 1u;
                    }
                    stack[stack_depth++] = item;
                }
                /* Chain top-level items in document order. */
                {
                    size_t previous = SIZE_MAX;
                    for (item = 0u; ok && item < outline_count; ++item) {
                        if (parent[item] != SIZE_MAX) continue;
                        if (previous != SIZE_MAX)
                            next_sibling[previous] = item;
                        prev_sibling[item] = previous;
                        previous = item;
                    }
                }
            }
            for (item = 0u; ok && outline_count && item < outline_count;
                 ++item) {
                const QwPdfText *text_item =
                    &lines->items[outline_entries[item].line_index];
                char title[768];
                size_t title_size = 0u;
                size_t source = 0u;
                size_t page_object =
                    first_page_object + outline_entries[item].page_index * 2u;
                size_t outline_object = outline_object_base + item;
                size_t parent_object =
                    parent[item] == SIZE_MAX
                        ? outline_root_object
                        : outline_object_base + parent[item];
                int title_is_ascii = 1;
                size_t scan_title;
                for (scan_title = 0u; scan_title < text_item->size; ++scan_title) {
                    unsigned char ch = (unsigned char)text_item->data[scan_title];
                    if (ch < 0x20u || ch >= 0x7fu) {
                        title_is_ascii = 0;
                        break;
                    }
                }
                if (title_is_ascii) {
                    title[title_size++] = '(';
                    while (source < text_item->size &&
                           title_size + 3u < sizeof(title) &&
                           title_size < 160u) {
                        unsigned char ch =
                            (unsigned char)text_item->data[source];
                        if (ch == '(' || ch == ')' || ch == '\\')
                            title[title_size++] = '\\';
                        title[title_size++] = (char)ch;
                        ++source;
                    }
                    title[title_size++] = ')';
                } else {
                    title[title_size++] = '<';
                    title[title_size++] = 'F';
                    title[title_size++] = 'E';
                    title[title_size++] = 'F';
                    title[title_size++] = 'F';
                    while (source < text_item->size &&
                           title_size + 4u < sizeof(title)) {
                        unsigned int cp = 0u;
                        unsigned char ch =
                            (unsigned char)text_item->data[source];
                        size_t extra = 0u;
                        if (ch < 0x80u) {
                            cp = ch;
                        } else if ((ch & 0xe0u) == 0xc0u) {
                            cp = ch & 0x1fu;
                            extra = 1u;
                        } else if ((ch & 0xf0u) == 0xe0u) {
                            cp = ch & 0x0fu;
                            extra = 2u;
                        } else if ((ch & 0xf8u) == 0xf0u) {
                            cp = ch & 0x07u;
                            extra = 3u;
                        } else {
                            cp = 0xfffdu;
                        }
                        ++source;
                        while (extra-- > 0u && source < text_item->size) {
                            unsigned char next =
                                (unsigned char)text_item->data[source];
                            if ((next & 0xc0u) != 0x80u) {
                                cp = 0xfffdu;
                                break;
                            }
                            cp = (cp << 6) | (next & 0x3fu);
                            ++source;
                        }
                        if (cp > 0xffffu) {
                            unsigned int value = cp - 0x10000u;
                            unsigned int high = 0xd800u + (value >> 10);
                            unsigned int low = 0xdc00u + (value & 0x3ffu);
                            title[title_size++] =
                                "0123456789ABCDEF"[(high >> 12) & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[(high >> 8) & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[(high >> 4) & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[high & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[(low >> 12) & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[(low >> 8) & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[(low >> 4) & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[low & 0xfu];
                        } else {
                            title[title_size++] =
                                "0123456789ABCDEF"[(cp >> 12) & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[(cp >> 8) & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[(cp >> 4) & 0xfu];
                            title[title_size++] =
                                "0123456789ABCDEF"[cp & 0xfu];
                        }
                    }
                    title[title_size++] = '>';
                }
                title[title_size] = '\0';
                if (!pdf_out_track(&out, out.bytes.size) ||
                    !pdf_out_printf(&out,
                        "%zu 0 obj\n<< /Title %s /Parent %zu 0 R",
                        outline_object, title, parent_object)) {
                    ok = 0;
                    break;
                }
                if (prev_sibling[item] != SIZE_MAX &&
                    !pdf_out_printf(&out, " /Prev %zu 0 R",
                                    outline_object_base + prev_sibling[item])) {
                    ok = 0;
                    break;
                }
                if (next_sibling[item] != SIZE_MAX &&
                    !pdf_out_printf(&out, " /Next %zu 0 R",
                                    outline_object_base + next_sibling[item])) {
                    ok = 0;
                    break;
                }
                if (first_child[item] != SIZE_MAX &&
                    (!pdf_out_printf(&out, " /First %zu 0 R",
                                     outline_object_base + first_child[item]) ||
                     !pdf_out_printf(&out, " /Last %zu 0 R",
                                     outline_object_base + last_child[item]) ||
                     !pdf_out_printf(&out, " /Count %zu",
                                     child_count[item]))) {
                    ok = 0;
                    break;
                }
                if (!pdf_out_printf(&out,
                            " /Dest [%zu 0 R /XYZ %.2f %.2f null] >>\nendobj\n",
                                    page_object, margin,
                                    outline_entries[item].y)) {
                    ok = 0;
                    break;
                }
            }
            free(parent);
            free(first_child);
            free(last_child);
            free(next_sibling);
            free(prev_sibling);
            free(child_count);
        }
        if (ok && outline_count) {
            if (!pdf_out_track(&out, out.bytes.size) ||
                !pdf_out_printf(&out,
                    "%zu 0 obj\n<< /Type /Outlines /First %zu 0 R /Last %zu 0 R "
                    "/Count %zu >>\nendobj\n",
                    outline_root_object, outline_object_base,
                    outline_object_base + outline_count - 1u,
                    outline_count)) {
                ok = 0;
            }
        }
        if (ok && (lines->title[0] || lines->author[0])) {
            size_t info_object = outline_root_object +
                                 (outline_count ? 1u : 0u);
            char escaped_title[768];
            char escaped_author[768];
            size_t title_size = 0u;
            size_t author_size = 0u;
            size_t source;
            int title_ascii = 1;
            int author_ascii = 1;
            for (source = 0u; lines->title[source]; ++source) {
                unsigned char ch = (unsigned char)lines->title[source];
                if (ch < 0x20u || ch >= 0x7fu) {
                    title_ascii = 0;
                    break;
                }
            }
            for (source = 0u; lines->author[source]; ++source) {
                unsigned char ch = (unsigned char)lines->author[source];
                if (ch < 0x20u || ch >= 0x7fu) {
                    author_ascii = 0;
                    break;
                }
            }
            if (title_ascii) {
                escaped_title[title_size++] = '(';
                for (source = 0u; lines->title[source] &&
                                  title_size + 3u < sizeof(escaped_title);
                     ++source) {
                    char ch = lines->title[source];
                    if (ch == '(' || ch == ')' || ch == '\\')
                        escaped_title[title_size++] = '\\';
                    escaped_title[title_size++] = ch;
                }
                escaped_title[title_size++] = ')';
            } else {
                escaped_title[title_size++] = '<';
                escaped_title[title_size++] = 'F';
                escaped_title[title_size++] = 'E';
                escaped_title[title_size++] = 'F';
                escaped_title[title_size++] = 'F';
                for (source = 0u; lines->title[source] &&
                                  title_size + 4u < sizeof(escaped_title);) {
                    unsigned int cp = 0u;
                    unsigned char ch = (unsigned char)lines->title[source];
                    size_t extra = 0u;
                    if (ch < 0x80u) cp = ch;
                    else if ((ch & 0xe0u) == 0xc0u) { cp = ch & 0x1fu; extra = 1u; }
                    else if ((ch & 0xf0u) == 0xe0u) { cp = ch & 0x0fu; extra = 2u; }
                    else if ((ch & 0xf8u) == 0xf0u) { cp = ch & 0x07u; extra = 3u; }
                    else cp = 0xfffdu;
                    ++source;
                    while (extra-- > 0u && lines->title[source]) {
                        unsigned char next = (unsigned char)lines->title[source];
                        if ((next & 0xc0u) != 0x80u) { cp = 0xfffdu; break; }
                        cp = (cp << 6) | (next & 0x3fu);
                        ++source;
                    }
                    escaped_title[title_size++] = "0123456789ABCDEF"[(cp >> 12) & 0xfu];
                    escaped_title[title_size++] = "0123456789ABCDEF"[(cp >> 8) & 0xfu];
                    escaped_title[title_size++] = "0123456789ABCDEF"[(cp >> 4) & 0xfu];
                    escaped_title[title_size++] = "0123456789ABCDEF"[cp & 0xfu];
                }
                escaped_title[title_size++] = '>';
            }
            escaped_title[title_size] = '\0';
            if (author_ascii) {
                escaped_author[author_size++] = '(';
                for (source = 0u; lines->author[source] &&
                                  author_size + 3u < sizeof(escaped_author);
                     ++source) {
                    char ch = lines->author[source];
                    if (ch == '(' || ch == ')' || ch == '\\')
                        escaped_author[author_size++] = '\\';
                    escaped_author[author_size++] = ch;
                }
                escaped_author[author_size++] = ')';
            } else {
                escaped_author[author_size++] = '<';
                escaped_author[author_size++] = 'F';
                escaped_author[author_size++] = 'E';
                escaped_author[author_size++] = 'F';
                escaped_author[author_size++] = 'F';
                for (source = 0u; lines->author[source] &&
                                  author_size + 4u < sizeof(escaped_author);) {
                    unsigned int cp = 0u;
                    unsigned char ch = (unsigned char)lines->author[source];
                    size_t extra = 0u;
                    if (ch < 0x80u) cp = ch;
                    else if ((ch & 0xe0u) == 0xc0u) { cp = ch & 0x1fu; extra = 1u; }
                    else if ((ch & 0xf0u) == 0xe0u) { cp = ch & 0x0fu; extra = 2u; }
                    else if ((ch & 0xf8u) == 0xf0u) { cp = ch & 0x07u; extra = 3u; }
                    else cp = 0xfffdu;
                    ++source;
                    while (extra-- > 0u && lines->author[source]) {
                        unsigned char next = (unsigned char)lines->author[source];
                        if ((next & 0xc0u) != 0x80u) { cp = 0xfffdu; break; }
                        cp = (cp << 6) | (next & 0x3fu);
                        ++source;
                    }
                    escaped_author[author_size++] = "0123456789ABCDEF"[(cp >> 12) & 0xfu];
                    escaped_author[author_size++] = "0123456789ABCDEF"[(cp >> 8) & 0xfu];
                    escaped_author[author_size++] = "0123456789ABCDEF"[(cp >> 4) & 0xfu];
                    escaped_author[author_size++] = "0123456789ABCDEF"[cp & 0xfu];
                }
                escaped_author[author_size++] = '>';
            }
            escaped_author[author_size] = '\0';
            char title_entry[800] = "";
            char author_entry[800] = "";
            /* Empty entries are omitted so the Info dictionary stays exactly
               equivalent to the XMP properties (ISO 19005-1 6.7.3). */
            if (lines->title[0]) {
                snprintf(title_entry, sizeof(title_entry), "/Title %s ",
                         escaped_title);
            }
            if (lines->author[0]) {
                snprintf(author_entry, sizeof(author_entry), "/Author %s ",
                         escaped_author);
            }
            if (!pdf_out_track(&out, out.bytes.size) ||
                !pdf_out_printf(&out,
                    "%zu 0 obj\n<< /Producer (Quirewise) %s%s>>\nendobj\n",
                    info_object, title_entry, author_entry)) {
                ok = 0;
            }
        }
        free(resources.data);
        if (ok && page_count == 0u) {
            size_t page_object = first_page_object;
            size_t content_object = first_page_object + 1u;
            if (!pdf_out_track(&out, out.bytes.size) ||
                !pdf_out_printf(&out,
                    "%zu 0 obj\n<< /Type /Page /Parent %zu 0 R /MediaBox [0 0 %.2f %.2f] "
                    "/Resources << %s >> "
                    "/Contents %zu 0 R >>\nendobj\n",
                    page_object, pages_object, page_width, page_height,
                    font_dict, content_object) ||
                !pdf_out_track(&out, out.bytes.size) ||
                !pdf_out_printf(&out, "%zu 0 obj\n<< /Length 0 >>\nstream\n\nendstream\nendobj\n",
                                content_object)) {
                ok = 0;
            }
        }
    }

    free(segments);
    free(page_first);
    free(page_count_lines);
    free(outline_entries);
    if (!ok) {
        free(out.bytes.data);
        free(out.offsets);
        return 0;
    }
    if (ok && font) {
        ok = pdf_write_embedded_font(&out, font, &subset, font_uses,
                                     font_use_count, font_object_base);
    }
    free(font_uses);
    qw_pdf_font_subset_free(&subset);
    if (ok && pdfa) {
        static const char xmp_prefix[] =
            "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
            " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
            "  <rdf:Description rdf:about=\"\"\n"
            "   xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\"\n"
            "   xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
            "   xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\"\n"
            "   xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
            "   <pdfaid:part>";
        static const char xmp_middle[] =
            "</pdfaid:part>\n"
            "   <pdfaid:conformance>B</pdfaid:conformance>\n"
            "   <xmp:CreatorTool>Quirewise</xmp:CreatorTool>\n";
        static const char xmp_producer[] =
            "   <pdf:Producer>Quirewise</pdf:Producer>\n";
        static const char xmp_suffix[] =
            "  </rdf:Description>\n"
            " </rdf:RDF>\n"
            "</x:xmpmeta>\n"
            "<?xpacket end=\"w\"?>\n";
        char xmp[2048];
        size_t xmp_used = 0u;
        int xmp_length = snprintf(xmp, sizeof(xmp), "%s%d%s", xmp_prefix,
                                  pdfa, xmp_middle);
        int xmp_ok = xmp_length > 0 && (size_t)xmp_length < sizeof(xmp);
        if (xmp_ok) {
            xmp_used = (size_t)xmp_length;
            /* ISO 19005-1/2 require the XMP properties to mirror the Info
               dictionary, so write the title and author the writer emits
               there instead of a fixed placeholder. */
            if (lines->title[0]) {
                xmp_ok = pdf_append_xml_markup(
                             xmp, sizeof(xmp), &xmp_used,
                             "   <dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">") &&
                         pdf_append_xml_text(xmp, sizeof(xmp), &xmp_used,
                                             lines->title) &&
                         pdf_append_xml_markup(xmp, sizeof(xmp), &xmp_used,
                                               "</rdf:li></rdf:Alt></dc:title>\n");
            }
            if (xmp_ok && lines->author[0]) {
                xmp_ok = pdf_append_xml_markup(
                             xmp, sizeof(xmp), &xmp_used,
                             "   <dc:creator><rdf:Seq><rdf:li>") &&
                         pdf_append_xml_text(xmp, sizeof(xmp), &xmp_used,
                                             lines->author) &&
                         pdf_append_xml_markup(xmp, sizeof(xmp), &xmp_used,
                                               "</rdf:li></rdf:Seq></dc:creator>\n");
            }
            if (xmp_ok) {
                xmp_ok = pdf_append_xml_markup(xmp, sizeof(xmp), &xmp_used,
                                               xmp_producer) &&
                         pdf_append_xml_markup(xmp, sizeof(xmp), &xmp_used,
                                               xmp_suffix);
            }
            if (xmp_ok) xmp_length = (int)xmp_used;
        }
        if (!xmp_ok) ok = 0;
        const uint8_t *icc;
        size_t icc_size = 0u;
        size_t icc_bound;
        uint8_t *icc_compressed = NULL;
        size_t icc_compressed_size = 0u;
        icc = qw_pdfa_srgb_profile(&icc_size);
        icc_bound = qw_compression_zlib_bound(icc_size);
        if (icc_bound != SIZE_MAX)
            icc_compressed = (uint8_t *)malloc(icc_bound);
        if (icc_compressed &&
            (!qw_compression_deflate_zlib(icc, icc_size, icc_compressed,
                                          icc_bound, &icc_compressed_size) ||
             icc_compressed_size >= icc_size)) {
            free(icc_compressed);
            icc_compressed = NULL;
            icc_compressed_size = 0u;
        }
        if (ok) {
            int t = pdf_out_track(&out, out.bytes.size);
            int p = pdf_out_printf(&out,
                                   "%zu 0 obj\n<< /Type /Metadata /Subtype /XML "
                                   "/Length %zu >>\nstream\n",
                                   pdfa_object_base, (size_t)xmp_length);
            int a = xmp_length > 0 &&
                    pdf_out_append(&out, xmp, (size_t)xmp_length);
            int e = pdf_out_printf(&out, "\nendstream\nendobj\n");
            ok = t && p && a && e;
        }
        if (ok)
            ok = pdf_out_track(&out, out.bytes.size) &&
                 pdf_out_printf(&out,
                                "%zu 0 obj\n<< /N 3 /Alternate /DeviceRGB "
                                "/Length %zu%s >>\nstream\n",
                                pdfa_object_base + 1u,
                                icc_compressed ? icc_compressed_size : icc_size,
                                icc_compressed ? " /Filter /FlateDecode" : "") &&
                 !(icc_compressed
                       ? (icc_compressed_size &&
                          !pdf_out_append(&out, icc_compressed,
                                          icc_compressed_size))
                       : (icc_size && !pdf_out_append(&out, icc, icc_size))) &&
                 pdf_out_printf(&out, "\nendstream\nendobj\n");
        if (ok)
            ok = pdf_out_track(&out, out.bytes.size) &&
                 pdf_out_printf(&out,
                                "%zu 0 obj\n<< /Type /OutputIntent "
                                "/S /GTS_PDFA1 "
                                "/OutputConditionIdentifier (sRGB) "
                                "/Info (sRGB IEC61966-2.1) "
                                "/DestOutputProfile %zu 0 R >>\nendobj\n",
                                pdfa_object_base + 2u, pdfa_object_base + 1u);
    }
    {
        size_t xref_offset = out.bytes.size;
        size_t entry;
        size_t object_total = object_total_no_font + (font ? 5u : 0u) +
                              (pdfa ? 3u : 0u);
        if (!pdf_out_printf(&out, "xref\n0 %zu\n", object_total + 1u) ||
            !pdf_out_printf(&out, "0000000000 65535 f \n")) {
            ok = 0;
        }
        for (entry = 0u; ok && entry < object_total; ++entry) {
            unsigned long long offset = entry < out.offset_count ? out.offsets[entry] : 0u;
            ok = pdf_out_printf(&out, "%010llu 00000 n \n", offset);
        }
        if (ok) {
            char file_id[48];
            pdf_trailer_id(&out, file_id, sizeof(file_id));
            if (lines->title[0] || lines->author[0]) {
                size_t info_object = 6u + (page_count ? page_count : 1u) * 2u +
                                     lines->image_count +
                                     (outline_count ? outline_count + 1u : 0u) +
                                     1u;
                ok = pdf_out_printf(
                    &out,
                    "trailer\n<< /Size %zu /Root 1 0 R /Info %zu 0 R "
                    "/ID [%s %s] >>\nstartxref\n%zu\n%%%%EOF\n",
                    object_total + 1u, info_object, file_id, file_id,
                    xref_offset);
            } else {
                ok = pdf_out_printf(
                    &out,
                    "trailer\n<< /Size %zu /Root 1 0 R /ID [%s %s] >>\n"
                    "startxref\n%zu\n%%%%EOF\n",
                    object_total + 1u, file_id, file_id, xref_offset);
            }
        }
    }
    if (!ok) {
        free(out.bytes.data);
        free(out.offsets);
        return 0;
    }
    free(out.offsets);
    *output = out.bytes;
    if (out_pages) *out_pages = page_count ? page_count : 1u;
    return 1;
}

static int pdf_render_html(const char *html, size_t html_size,
                           double page_width, double page_height,
                           const char *font_path, int pdfa,
                           QwPdfText *output, size_t *out_pages,
                           QwConvertReport *report)
{
    QwPdfLines lines;
    QwPdfFont *font = NULL;
    size_t pages = 0u;
    size_t missing = 0u;
    if (!html || !output) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (html_size > QW_PDF_MAX_TEXT_BYTES) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (page_width < 72.0 || page_width > 5000.0 ||
        page_height < 72.0 || page_height > 5000.0) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (pdfa && !font_path) {
        if (report)
            snprintf(report->note, sizeof(report->note),
                     "PDF/A needs an embedded font: pass pdf_font_path");
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    if (font_path) {
        const char *font_reason = NULL;
        font = qw_pdf_font_load_ex(font_path, &font_reason);
        if (!font) {
            if (report) {
                snprintf(report->note, sizeof(report->note),
                         "Cannot embed font %s: %s", font_path,
                         font_reason ? font_reason : "unsupported font");
            }
            return QW_STATUS_ERR_UNSUPPORTED;
        }
    }
    memset(&lines, 0, sizeof(lines));
    if (!pdf_parse_html(html, html_size, &lines)) {
        pdf_lines_free(&lines);
        qw_pdf_font_free(font);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (font) {
        size_t item_index;
        for (item_index = 0u; item_index < lines.count; ++item_index) {
            const QwPdfText *text = &lines.items[item_index];
            size_t position = 0u;
            while (position < text->size) {
                unsigned int code = 0u;
                size_t length = pdf_decode_utf8(
                    (const unsigned char *)text->data + position,
                    text->size - position, &code);
                if (!length) break;
                if (qw_pdf_font_gid(font, code) == 0u) missing += 1u;
                position += length;
            }
        }
    }
    if (!pdf_emit_document(&lines, page_width, page_height, font, pdfa,
                           output, &pages)) {
        pdf_lines_free(&lines);
        qw_pdf_font_free(font);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    qw_pdf_font_free(font);
    if (report) {
        if (missing) {
            snprintf(report->note, sizeof(report->note),
                     "Rendered %zu layout line(s) into %zu PDF page(s); %zu "
                     "character(s) are not covered by the embedded font and "
                     "use .notdef",
                     lines.count, pages, missing);
        } else {
            snprintf(report->note, sizeof(report->note),
                     "Rendered %zu layout line(s) into %zu PDF page(s) with the internal Quirewise HTML/PDF writer",
                     lines.count, pages);
        }
        if (!font) {
            size_t probe;
            int non_ascii = 0;
            size_t note_length;
            for (probe = 0u; probe < html_size; ++probe) {
                if ((unsigned char)html[probe] >= 0x80u) {
                    non_ascii = 1;
                    break;
                }
            }
            note_length = strlen(report->note);
            if (non_ascii && note_length + 64u < sizeof(report->note)) {
                snprintf(report->note + note_length,
                         sizeof(report->note) - note_length,
                         "; non-ASCII text uses the built-in STSong-Light "
                         "without embedding");
            }
        }
    }
    pdf_lines_free(&lines);
    if (out_pages) *out_pages = pages;
    return QW_STATUS_OK;
}

int qw_doc_convert_html_with_page_size(const char *html, size_t html_size,
                                         float page_width_points,
                                         float page_height_points,
                                         uint8_t **out_pdf, size_t *out_size,
                                         QwConvertReport *report)
{
    QwPdfText output;
    int rc;
    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    memset(&output, 0, sizeof(output));
    rc = pdf_render_html(html, html_size,
                         page_width_points > 0.0f ? page_width_points : 595.0,
                         page_height_points > 0.0f ? page_height_points : 842.0,
                         NULL, 0, &output, NULL, report);
    if (rc != QW_STATUS_OK) return rc;
    *out_pdf = (uint8_t *)output.data;
    *out_size = output.size;
    return QW_STATUS_OK;
}

int qw_doc_convert_html_with_font(const char *html, size_t html_size,
                                  float page_width_points,
                                  float page_height_points,
                                  const char *font_path,
                                  uint8_t **out_pdf, size_t *out_size,
                                  QwConvertReport *report)
{
    QwPdfText output;
    int rc;
    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    memset(&output, 0, sizeof(output));
    rc = pdf_render_html(html, html_size,
                         page_width_points > 0.0f ? page_width_points : 595.0,
                         page_height_points > 0.0f ? page_height_points : 842.0,
                         font_path, 0, &output, NULL, report);
    if (rc != QW_STATUS_OK) return rc;
    *out_pdf = (uint8_t *)output.data;
    *out_size = output.size;
    return QW_STATUS_OK;
}

int qw_doc_convert_html_pdfa(const char *html, size_t html_size,
                                 float page_width_points,
                                 float page_height_points,
                                 const char *font_path, int pdfa_level,
                                 uint8_t **out_pdf, size_t *out_size,
                                 QwConvertReport *report)
{
    QwPdfText output;
    int rc;
    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    memset(&output, 0, sizeof(output));
    rc = pdf_render_html(html, html_size,
                         page_width_points > 0.0f ? page_width_points : 595.0,
                         page_height_points > 0.0f ? page_height_points : 842.0,
                         font_path, pdfa_level, &output, NULL, report);
    if (rc != QW_STATUS_OK) return rc;
    *out_pdf = (uint8_t *)output.data;
    *out_size = output.size;
    return QW_STATUS_OK;
}

int qw_doc_convert_html(const char *html, size_t html_size,
                          uint8_t **out_pdf, size_t *out_size,
                          QwConvertReport *report)
{
    return qw_doc_convert_html_with_page_size(html, html_size, 595.0f, 842.0f,
                                                out_pdf, out_size, report);
}

int qw_doc_convert_html_to_stream(const char *html, size_t html_size,
                                    FILE *stream, size_t max_output_bytes,
                                    size_t *out_size, QwConvertReport *report)
{
    return qw_doc_convert_html_with_page_size_to_stream(
        html, html_size, 595.0f, 842.0f, stream, max_output_bytes, out_size, report);
}

int qw_doc_convert_html_with_page_size_to_stream(
    const char *html, size_t html_size, float page_width_points,
    float page_height_points, FILE *stream, size_t max_output_bytes,
    size_t *out_size, QwConvertReport *report)
{
    QwPdfText output;
    int rc;
    if (!stream) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (out_size) *out_size = 0u;
    memset(&output, 0, sizeof(output));
    rc = pdf_render_html(html, html_size,
                         page_width_points > 0.0f ? page_width_points : 595.0,
                         page_height_points > 0.0f ? page_height_points : 842.0,
                         NULL, 0, &output, NULL, report);
    if (rc != QW_STATUS_OK) return rc;
    if (max_output_bytes && output.size > max_output_bytes) {
        pdf_text_free(&output);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (output.size &&
        fwrite(output.data, 1u, output.size, stream) != output.size) {
        pdf_text_free(&output);
        return QW_STATUS_ERR_IO;
    }
    if (out_size) *out_size = output.size;
    pdf_text_free(&output);
    return QW_STATUS_OK;
}

int qw_doc_convert_html_file(const char *path, uint8_t **out_pdf,
                               size_t *out_size, QwConvertReport *report)
{
    FILE *file;
    long length;
    char *buffer;
    size_t read_size;
    int rc;
    if (!path || !out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_pdf = NULL;
    *out_size = 0u;
    file = fopen(path, "rb");
    if (!file) return QW_STATUS_ERR_IO;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return QW_STATUS_ERR_IO;
    }
    if ((unsigned long)length > QW_PDF_MAX_TEXT_BYTES) {
        fclose(file);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    buffer = (char *)malloc((size_t)length + 1u);
    if (!buffer) {
        fclose(file);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    read_size = fread(buffer, 1u, (size_t)length, file);
    fclose(file);
    if (read_size != (size_t)length) {
        free(buffer);
        return QW_STATUS_ERR_IO;
    }
    rc = qw_doc_convert_html(buffer, read_size, out_pdf, out_size, report);
    free(buffer);
    return rc;
}

int qw_doc_convert_html_file_to_stream(const char *path, FILE *stream,
                                         size_t max_output_bytes,
                                         size_t *out_size,
                                         QwConvertReport *report)
{
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    int rc;
    if (!path || !stream) return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = qw_doc_convert_html_file(path, &pdf, &pdf_size, report);
    if (rc != QW_STATUS_OK) return rc;
    if (max_output_bytes && pdf_size > max_output_bytes) {
        free(pdf);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (pdf_size && fwrite(pdf, 1u, pdf_size, stream) != pdf_size) {
        free(pdf);
        return QW_STATUS_ERR_IO;
    }
    if (out_size) *out_size = pdf_size;
    free(pdf);
    return QW_STATUS_OK;
}