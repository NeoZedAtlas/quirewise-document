/* Minimal HWPX (OWPML) writer.
 *
 * The package is the smallest one the document package's own HWPX reader
 * accepts: META-INF/container.xml pointing at Contents/content.hpf, a
 * manifest with one section item, a spine itemref and Contents/section0.xml
 * whose <p>/<t> structure the reader maps back to HTML paragraphs.
 *
 * Input is the readers' HTML intermediate. Block tags become one OWPML
 * paragraph each; table cells are flattened with " | " separators; images
 * become their alt text. Everything is stored uncompressed in the ZIP
 * container.
 */

#include "quirewise/hwpx.h"

#include "../archive/zip_native.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWPX_WRITE_LIMIT ((size_t)64u * 1024u * 1024u)

typedef struct HwpxBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} HwpxBuffer;

typedef struct HwpxTag {
    char name[16];
    int closing;
} HwpxTag;

static int hwpx_buf_reserve(HwpxBuffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *grown;

    if (buffer->failed) return 0;
    if (buffer->size > HWPX_WRITE_LIMIT ||
        additional > HWPX_WRITE_LIMIT - buffer->size) {
        buffer->failed = 1;
        return 0;
    }
    needed = buffer->size + additional;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 4096u;
    while (capacity < needed) {
        if (capacity > HWPX_WRITE_LIMIT / 2u) {
            capacity = HWPX_WRITE_LIMIT;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed) {
        buffer->failed = 1;
        return 0;
    }
    grown = (uint8_t *)realloc(buffer->data, capacity);
    if (!grown) {
        buffer->failed = 1;
        return 0;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int hwpx_buf_append(HwpxBuffer *buffer, const void *data, size_t size)
{
    if (!size) return 1;
    if (!hwpx_buf_reserve(buffer, size)) return 0;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static int hwpx_buf_text(HwpxBuffer *buffer, const char *text)
{
    return text && text[0] ? hwpx_buf_append(buffer, text, strlen(text)) : 1;
}

static int hwpx_xml_escape(HwpxBuffer *buffer, const uint8_t *text,
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
        if (!hwpx_buf_append(buffer, text + start, index - start) ||
            !hwpx_buf_text(buffer, replacement))
            return 0;
        start = index + 1u;
    }
    return hwpx_buf_append(buffer, text + start, size - start);
}

static size_t hwpx_decode_entity(const uint8_t *data, size_t size,
                                 char out[8], size_t *out_size)
{
    size_t index = 1u;
    uint32_t codepoint = 0u;
    int digits = 0;

    if (index >= size) return 0u;
    if (data[index] == '#') {
        index += 1u;
        if (index < size && (data[index] == 'x' || data[index] == 'X')) {
            index += 1u;
            while (index < size) {
                int value = -1;
                if (data[index] >= '0' && data[index] <= '9')
                    value = data[index] - '0';
                else if (data[index] >= 'a' && data[index] <= 'f')
                    value = data[index] - 'a' + 10;
                else if (data[index] >= 'A' && data[index] <= 'F')
                    value = data[index] - 'A' + 10;
                if (value < 0) break;
                codepoint = codepoint * 16u + (uint32_t)value;
                ++index;
                ++digits;
            }
        } else {
            while (index < size && data[index] >= '0' && data[index] <= '9') {
                codepoint = codepoint * 10u + (uint32_t)(data[index] - '0');
                ++index;
                ++digits;
            }
        }
        if (!digits || index >= size || data[index] != ';') return 0u;
        ++index;
        if (codepoint <= 0x7fu) {
            out[0] = (char)codepoint;
            *out_size = 1u;
        } else if (codepoint <= 0x7ffu) {
            out[0] = (char)(0xc0u | (codepoint >> 6));
            out[1] = (char)(0x80u | (codepoint & 0x3fu));
            *out_size = 2u;
        } else if (codepoint <= 0xffffu) {
            out[0] = (char)(0xe0u | (codepoint >> 12));
            out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
            out[2] = (char)(0x80u | (codepoint & 0x3fu));
            *out_size = 3u;
        } else {
            return 0u;
        }
        return index;
    }
    {
        static const struct {
            const char *name;
            char value;
        } named[] = {
            { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' },
            { "quot;", '"' }, { "apos;", '\'' }, { "nbsp;", ' ' }
        };
        size_t entry;

        for (entry = 0u; entry < sizeof(named) / sizeof(named[0]); ++entry) {
            size_t name_size = strlen(named[entry].name);
            if (index + name_size <= size &&
                memcmp(data + index, named[entry].name, name_size) == 0) {
                out[0] = named[entry].value;
                *out_size = 1u;
                return index + name_size;
            }
        }
    }
    return 0u;
}

static size_t hwpx_find_tag_end(const uint8_t *data, size_t size,
                                size_t start)
{
    size_t index;

    for (index = start; index < size; ++index) {
        if (data[index] == '>') return index;
    }
    return size;
}

static size_t hwpx_parse_tag(const uint8_t *data, size_t size, size_t start,
                             HwpxTag *tag)
{
    size_t index = start + 1u;
    size_t name_start;

    memset(tag, 0, sizeof(*tag));
    if (index >= size) return 0u;
    if (data[index] == '!' || data[index] == '?') {
        return hwpx_find_tag_end(data, size, index) + 1u - start;
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
                  ? index - name_start
                  : sizeof(tag->name) - 1u] = '\0';
    if (!tag->name[0]) return 0u;
    {
        size_t end = hwpx_find_tag_end(data, size, index);
        if (end >= size) return 0u;
        return end + 1u - start;
    }
}

static size_t hwpx_skip_element(const uint8_t *data, size_t size,
                                size_t start, const char *name)
{
    size_t name_size = strlen(name);
    size_t index = start;

    for (; index + name_size + 3u <= size; ++index) {
        if (data[index] == '<' && data[index + 1u] == '/') {
            size_t offset;
            int matches = 1;
            for (offset = 0u; offset < name_size; ++offset) {
                char value = (char)data[index + 2u + offset];
                if (value >= 'A' && value <= 'Z')
                    value = (char)(value - 'A' + 'a');
                if (value != name[offset]) {
                    matches = 0;
                    break;
                }
            }
            if (matches) {
                size_t end = hwpx_find_tag_end(data, size, index);
                return end < size ? end + 1u : size;
            }
        }
    }
    return size;
}

static int hwpx_is_block_tag(const char *name)
{
    return strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
           strcmp(name, "li") == 0 || strcmp(name, "tr") == 0 ||
           strcmp(name, "blockquote") == 0 ||
           (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]);
}

static int hwpx_emit_paragraph(HwpxBuffer *section, const HwpxBuffer *text)
{
    if (!text->size) return 1;
    return hwpx_buf_text(section, "<hs:p><hs:t>") &&
           hwpx_buf_append(section, text->data, text->size) &&
           hwpx_buf_text(section, "</hs:t></hs:p>\n");
}

int qw_hwpx_create(const void *html, size_t html_size, uint8_t **out_package,
                   size_t *out_size)
{
    static const char container_xml[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\""
        " version=\"1.0\"><rootfiles><rootfile full-path=\"Contents/content."
        "hpf\" media-type=\"application/hwpml-package+xml\"/></rootfiles>"
        "</container>";
    static const char content_hpf[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<hpf:HWPackage xmlns:hpf=\"http://www.hancom.co.kr/schema/2011/hpf\""
        " version=\"1.0\"><hpf:manifest>"
        "<hpf:item id=\"header\" href=\"header.xml\" "
        "media-type=\"application/xml\"/>"
        "<hpf:item id=\"section0\" href=\"section0.xml\" "
        "media-type=\"application/xml\"/></hpf:manifest><hpf:spine>"
        "<hpf:itemref idref=\"section0\"/></hpf:spine></hpf:HWPackage>";
    static const char header_xml[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<hh:head xmlns:hh=\"http://www.hancom.co.kr/hwpml/2011/head\"/>";
    HwpxBuffer section;
    HwpxBuffer paragraph;
    const uint8_t *data = (const uint8_t *)html;
    size_t index = 0u;
    size_t section_start = 0u;
    QwZipWriter writer;
    void *archive = NULL;
    size_t archive_size = 0u;
    int status = (int)QW_HWPX_ERR_OUT_OF_MEMORY;

    if (out_package) *out_package = NULL;
    if (out_size) *out_size = 0u;
    if (!html || !html_size || !out_package || !out_size)
        return QW_HWPX_ERR_INVALID_ARGUMENT;
    if (html_size > HWPX_WRITE_LIMIT) return QW_HWPX_ERR_RESOURCE_LIMIT;
    memset(&section, 0, sizeof(section));
    memset(&paragraph, 0, sizeof(paragraph));
    if (!hwpx_buf_text(&section,
                       "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<hs:sec xmlns:hs=\"http://www.hancom.co.kr/hwpml/2011/"
                       "section\">") ||
        !hwpx_buf_text(&section, "\n"))
        goto out_of_memory;
    section_start = section.size;
    while (index < html_size) {
        if (data[index] != '<') {
            size_t start = index;
            while (index < html_size && data[index] != '<') ++index;
            while (start < index) {
                char decoded[8];
                size_t decoded_size = 0u;
                size_t consumed;

                if (data[start] == '&') {
                    consumed = hwpx_decode_entity(data + start, index - start,
                                                  decoded, &decoded_size);
                    if (consumed) {
                        if (!hwpx_xml_escape(&paragraph,
                                             (const uint8_t *)decoded,
                                             decoded_size))
                            goto out_of_memory;
                        start += consumed;
                        continue;
                    }
                }
                /* Copy UTF-8 code point boundaries with XML escaping. */
                {
                    size_t length = 1u;
                    if (data[start] >= 0xc0u && start + 1u < index) {
                        unsigned char lead = data[start];
                        if (lead >= 0xf0u) length = 4u;
                        else if (lead >= 0xe0u) length = 3u;
                        else if (lead >= 0xc0u) length = 2u;
                        if (start + length > index) length = 1u;
                    }
                    if (!hwpx_xml_escape(&paragraph, data + start, length))
                        goto out_of_memory;
                    start += length;
                }
            }
            continue;
        }
        {
            HwpxTag tag;
            size_t consumed = hwpx_parse_tag(data, html_size, index, &tag);
            if (!consumed) {
                if (!hwpx_xml_escape(&paragraph, (const uint8_t *)"<", 1u))
                    goto out_of_memory;
                ++index;
                continue;
            }
            index += consumed;
            if (!tag.name[0]) continue;
            if (strcmp(tag.name, "head") == 0) {
                if (!tag.closing)
                    index = hwpx_skip_element(data, html_size, index, "head");
                continue;
            }
            if (strcmp(tag.name, "script") == 0 ||
                strcmp(tag.name, "style") == 0 ||
                strcmp(tag.name, "title") == 0) {
                if (!tag.closing)
                    index = hwpx_skip_element(data, html_size, index,
                                              tag.name);
                continue;
            }
            if (strcmp(tag.name, "br") == 0) {
                if (!hwpx_emit_paragraph(&section, &paragraph))
                    goto out_of_memory;
                paragraph.size = 0u;
                continue;
            }
            if (strcmp(tag.name, "td") == 0 || strcmp(tag.name, "th") == 0) {
                if (tag.closing && !hwpx_buf_text(&paragraph, " | "))
                    goto out_of_memory;
                continue;
            }
            if (hwpx_is_block_tag(tag.name)) {
                if (!hwpx_emit_paragraph(&section, &paragraph))
                    goto out_of_memory;
                paragraph.size = 0u;
            }
        }
    }
    if (!hwpx_emit_paragraph(&section, &paragraph) ||
        !hwpx_buf_text(&section, "</hs:sec>\n"))
        goto out_of_memory;
    free(paragraph.data);
    paragraph.data = NULL;
    if (section.size <= section_start) {
        status = QW_HWPX_ERR_FORMAT;
        goto cleanup;
    }
    memset(&writer, 0, sizeof(writer));
    if (!qw_zip_writer_init_heap(&writer, 0u, 0u)) goto out_of_memory;
    if (!qw_zip_writer_add_mem(&writer, "mimetype", "application/hwp+zip", 20u,
                               QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_add_mem(&writer, "META-INF/container.xml", container_xml,
                               strlen(container_xml), QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_add_mem(&writer, "Contents/content.hpf", content_hpf,
                               strlen(content_hpf), QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_add_mem(&writer, "Contents/header.xml", header_xml,
                               strlen(header_xml), QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_add_mem(&writer, "Contents/section0.xml", section.data,
                               section.size, QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_finalize_heap_archive(&writer, &archive,
                                             &archive_size) ||
        !archive) {
        qw_zip_writer_end(&writer);
        goto out_of_memory;
    }
    qw_zip_writer_end(&writer);
    free(section.data);
    *out_package = (uint8_t *)archive;
    *out_size = archive_size;
    return (int)QW_HWPX_OK;

out_of_memory:
    status = QW_HWPX_ERR_OUT_OF_MEMORY;
cleanup:
    free(paragraph.data);
    free(section.data);
    return (int)status;
}

void qw_hwpx_buffer_free(void *buffer)
{
    free(buffer);
}
