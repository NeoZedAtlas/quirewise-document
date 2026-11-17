#if !defined(QUIREWISE_STANDALONE_ODP)
#include "../../core/format_registry.h"
#endif
#include "../../core/html_output.h"
#include "quirewise/odf_presentation.h"
#include "quirewise/xml.h"
#include "../archive/zip_native.h"

#include <quirewise/image.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ODP_FLAT_PROBE_BYTES = 8192,
    ODP_MAX_XML_BYTES = 32 * 1024 * 1024,
    ODP_MAX_FLAT_XML_BYTES = 64 * 1024 * 1024,
    ODP_MAX_HTML_BYTES = 64 * 1024 * 1024,
    ODP_MAX_SLIDES = 1000,
    ODP_MAX_XML_NODES = 2000000,
    ODP_MAX_XML_DEPTH = 128,
    ODP_MAX_SPACES_PER_RUN = 4096,
    ODP_MAX_IMAGES = 64,
    ODP_MAX_MEDIA_BYTES = 16 * 1024 * 1024
};

static int odp_bytes_contain(const uint8_t *data, size_t size,
                             const char *pattern, size_t pattern_size)
{
    size_t i;
    if (!data || !pattern || !pattern_size || pattern_size > size) return 0;
    for (i = 0u; i <= size - pattern_size; ++i) {
        if (memcmp(data + i, pattern, pattern_size) == 0) return 1;
    }
    return 0;
}

static int odp_flat_probe(const void *data, size_t size)
{
    static const char office_ns[] =
        "urn:oasis:names:tc:opendocument:xmlns:office:1.0";
    static const char mime_attr[] = "office:mimetype";
    static const char presentation_mime[] =
        "application/vnd.oasis.opendocument.presentation";
    static const char template_mime[] =
        "application/vnd.oasis.opendocument.presentation-template";
    const uint8_t *bytes = (const uint8_t *)data;
    size_t probe_size = size < ODP_FLAT_PROBE_BYTES ? size : ODP_FLAT_PROBE_BYTES;
    size_t i;
    size_t root_end = 0u;
    if (!bytes || !size || size > ODP_MAX_FLAT_XML_BYTES ||
        (size >= 4u && bytes[0] == 'P' && bytes[1] == 'K' &&
         ((bytes[2] == 3u && bytes[3] == 4u) ||
          (bytes[2] == 5u && bytes[3] == 6u) ||
          (bytes[2] == 7u && bytes[3] == 8u)))) return 0;
    for (i = 0u; i < probe_size; ++i) {
        if (bytes[i] == '<' && i + 1u < probe_size &&
            bytes[i + 1u] != '?' && bytes[i + 1u] != '!' &&
            bytes[i + 1u] != '/') {
            size_t j;
            for (j = i + 1u; j < probe_size; ++j) {
                if (bytes[j] == '>') {
                    root_end = j + 1u;
                    break;
                }
            }
            break;
        }
    }
    if (!root_end) return 0;
    return odp_bytes_contain(bytes, root_end, office_ns, sizeof(office_ns) - 1u) &&
           odp_bytes_contain(bytes, root_end, mime_attr, sizeof(mime_attr) - 1u) &&
           (odp_bytes_contain(bytes, root_end, presentation_mime,
                              sizeof(presentation_mime) - 1u) ||
            odp_bytes_contain(bytes, root_end, template_mime,
                              sizeof(template_mime) - 1u));
}

typedef struct OdpReader {
    QwZipArchive *zip;
    QwTextBuffer html;
    size_t visited_nodes;
    size_t slide_count;
    size_t image_count;
} OdpReader;

struct QwOdpDocument {
    QwTextBuffer html;
    size_t slide_count;
};

static const char *odp_local_name(const char *qualified_name)
{
    const char *colon;
    if (!qualified_name) return "";
    colon = strrchr(qualified_name, ':');
    return colon ? colon + 1u : qualified_name;
}

static int odp_is_tag(QwXmlNode *node, const char *local_name)
{
    const char *tag = qw_xml_tag(node);
    return tag && strcmp(odp_local_name(tag), local_name) == 0;
}

static int odp_html_literal(OdpReader *reader, const char *text)
{
    return reader && qw_html_output_literal(&reader->html, text,
                                             ODP_MAX_HTML_BYTES, NULL);
}

static int odp_html_escape(OdpReader *reader, const char *text)
{
    return reader && qw_html_output_escape(&reader->html, text,
                                            ODP_MAX_HTML_BYTES, NULL);
}

static void *odp_extract_xml(OdpReader *reader, const char *name,
                             size_t *out_size)
{
    size_t index;
    size_t extracted_size = 0u;
    QwZipEntryInfo stat;
    void *contents;
    if (out_size) *out_size = 0u;
    if (!reader || !reader->zip || !out_size) return NULL;
    index = qw_zip_reader_locate_file(reader->zip, name, NULL,
                                      QW_ZIP_CASE_SENSITIVE);
    if (index == (size_t)-1 ||
        !qw_zip_reader_file_stat(reader->zip, index, &stat) ||
        !stat.uncompressed_size || stat.uncompressed_size > ODP_MAX_XML_BYTES ||
        !qw_zip_reader_is_file_supported(reader->zip, index)) return NULL;
    contents = qw_zip_reader_extract_to_heap(reader->zip, index,
                                              &extracted_size, 0u);
    if (!contents || extracted_size != stat.uncompressed_size ||
        extracted_size > ODP_MAX_XML_BYTES) {
        free(contents);
        return NULL;
    }
    *out_size = extracted_size;
    return contents;
}

static QwXmlNode *odp_parse_xml(const uint8_t *data, size_t size,
                                QwXmlError *error)
{
    return qw_xml_parse(data, size, error);
}

static QwXmlNode *odp_find_descendant(OdpReader *reader, QwXmlNode *node,
                                   const char *local_name, size_t depth)
{
    QwXmlNode *found;
    if (!reader || depth > ODP_MAX_XML_DEPTH) return NULL;
    for (; node; node = qw_xml_next(node)) {
        if (++reader->visited_nodes > ODP_MAX_XML_NODES) return NULL;
        if (odp_is_tag(node, local_name)) return node;
        found = odp_find_descendant(reader, qw_xml_down(node), local_name,
                                    depth + 1u);
        if (found || reader->visited_nodes > ODP_MAX_XML_NODES) return found;
    }
    return NULL;
}

static int odp_parse_space_count(const char *text, size_t *out_count)
{
    unsigned long value;
    char *end = NULL;
    if (!out_count) return 0;
    *out_count = 1u;
    if (!text || !*text) return 1;
    if (*text < '0' || *text > '9') return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || !end || *end || value == 0u ||
        value > ODP_MAX_SPACES_PER_RUN) return 0;
    *out_count = (size_t)value;
    return 1;
}

static int odp_append_spaces(OdpReader *reader, size_t count)
{
    size_t i;
    for (i = 0u; i < count; ++i) {
        if (!odp_html_literal(reader, " ")) return 0;
    }
    return 1;
}


static int odp_url_is_safe(const char *href)
{
    size_t i;
    if (!href) return 0;
    for (i = 0u; href[i]; ++i) {
        if (href[i] == ' ' || href[i] == '\n' || href[i] == '\r' ||
            href[i] == '\t' || href[i] == '"' || href[i] == '\'')
            return 0;
    }
    if (strncmp(href, "http://", 7u) == 0) return 1;
    if (strncmp(href, "https://", 8u) == 0) return 1;
    if (strncmp(href, "mailto:", 7u) == 0) return 1;
    if (strncmp(href, "ftp://", 6u) == 0) return 1;
    return 0;
}

static int odp_render_inline(OdpReader *reader, QwXmlNode *node, size_t depth)
{
    if (depth > ODP_MAX_XML_DEPTH) return 0;
    for (; node; node = qw_xml_next(node)) {
        const char *text;
        if (++reader->visited_nodes > ODP_MAX_XML_NODES) return 0;
        text = qw_xml_text(node);
        if (text) {
            if (!odp_html_escape(reader, text)) return 0;
        } else if (odp_is_tag(node, "line-break")) {
            if (!odp_html_literal(reader, "<br>")) return 0;
        } else if (odp_is_tag(node, "tab")) {
            if (!odp_html_literal(reader, "&nbsp;&nbsp;&nbsp;")) return 0;
        } else if (odp_is_tag(node, "s")) {
            size_t count;
            if (!odp_parse_space_count(qw_xml_att(node, "text:c"), &count) ||
                !odp_append_spaces(reader, count)) return 0;
        } else if (odp_is_tag(node, "a")) {
            const char *href = qw_xml_att(node, "xlink:href");
            if (href && odp_url_is_safe(href)) {
                if (!odp_html_literal(reader, "<a href=\"") ||
                    !odp_html_escape(reader, href) ||
                    !odp_html_literal(reader, "\">") ||
                    !odp_render_inline(reader, qw_xml_down(node), depth + 1u) ||
                    !odp_html_literal(reader, "</a>"))
                    return 0;
            } else if (!odp_render_inline(reader, qw_xml_down(node),
                                          depth + 1u)) {
                return 0;
            }
        } else if (!odp_render_inline(reader, qw_xml_down(node), depth + 1u)) {
            return 0;
        }
    }
    return 1;
}


static int odp_html_base64(OdpReader *reader, const uint8_t *data,
                           size_t size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char chunk[260];
    size_t used = 0u;
    size_t index;

    for (index = 0u; index < size; index += 3u) {
        uint32_t value = (uint32_t)data[index] << 16;
        if (index + 1u < size) value |= (uint32_t)data[index + 1u] << 8;
        if (index + 2u < size) value |= (uint32_t)data[index + 2u];
        chunk[used++] = alphabet[(value >> 18) & 0x3fu];
        chunk[used++] = alphabet[(value >> 12) & 0x3fu];
        chunk[used++] = index + 1u < size
                            ? alphabet[(value >> 6) & 0x3fu] : '=';
        chunk[used++] = index + 2u < size ? alphabet[value & 0x3fu] : '=';
        if (used >= 256u) {
            if (!qw_html_output_append(&reader->html, chunk, used,
                                       ODP_MAX_HTML_BYTES, NULL))
                return 0;
            used = 0u;
        }
    }
    if (used && !qw_html_output_append(&reader->html, chunk, used,
                                       ODP_MAX_HTML_BYTES, NULL))
        return 0;
    return 1;
}

static int odp_base64_value(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

static uint8_t *odp_base64_decode(const char *text, size_t size,
                                  size_t *out_size)
{
    uint8_t *out;
    size_t capacity;
    size_t written = 0u;
    uint32_t accumulator = 0u;
    int bits = 0;
    size_t index;

    if (!text || !size) return NULL;
    capacity = (size / 4u + 1u) * 3u;
    if (capacity > ODP_MAX_MEDIA_BYTES) return NULL;
    out = (uint8_t *)malloc(capacity);
    if (!out) return NULL;
    for (index = 0u; index < size; ++index) {
        int value = odp_base64_value((unsigned char)text[index]);
        if (value < 0) {
            if (text[index] == '=') break;
            if (text[index] == ' ' || text[index] == '\t' ||
                text[index] == '\r' || text[index] == '\n')
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

static void *odp_extract_media(OdpReader *reader, const char *name,
                               size_t *out_size)
{
    size_t index;
    size_t extracted_size = 0u;
    QwZipEntryInfo stat;
    void *contents;

    if (out_size) *out_size = 0u;
    if (!reader || !reader->zip || !name || !out_size || strstr(name, "..") ||
        name[0] == '/')
        return NULL;
    index = qw_zip_reader_locate_file(reader->zip, name, NULL,
                                      QW_ZIP_CASE_SENSITIVE);
    if (index == (size_t)-1 ||
        !qw_zip_reader_file_stat(reader->zip, index, &stat) ||
        !stat.uncompressed_size || stat.uncompressed_size > ODP_MAX_MEDIA_BYTES ||
        !qw_zip_reader_is_file_supported(reader->zip, index))
        return NULL;
    contents = qw_zip_reader_extract_to_heap(reader->zip, index,
                                              &extracted_size, 0u);
    if (!contents || extracted_size != stat.uncompressed_size ||
        extracted_size > ODP_MAX_MEDIA_BYTES) {
        free(contents);
        return NULL;
    }
    *out_size = extracted_size;
    return contents;
}

static double odp_length_points(const char *value)
{
    char *end = NULL;
    double number;

    if (!value || !value[0]) return 0.0;
    number = strtod(value, &end);
    if (end == value || number <= 0.0) return 0.0;
    if (strcmp(end, "cm") == 0) return number * 72.0 / 2.54;
    if (strcmp(end, "mm") == 0) return number * 72.0 / 25.4;
    if (strcmp(end, "in") == 0) return number * 72.0;
    if (strcmp(end, "pc") == 0) return number * 12.0;
    if (strcmp(end, "pt") == 0) return number;
    if (strcmp(end, "px") == 0) return number * 0.75;
    return number;
}


static unsigned odp_image_channels(QwImagePixelFormat format)
{
    return format == QW_IMAGE_PIXEL_GRAY8 ? 1u : 3u;
}

static int odp_parse_clip_rect(const char *clip, double values[4])
{
    size_t index = 0u;
    int count = 0;
    if (!clip || strncmp(clip, "rect(", 5u) != 0) return 0;
    index = 5u;
    while (count < 4) {
        char *end = NULL;
        double value = strtod(clip + index, &end);
        if (end == clip + index) return 0;
        if (value < 0.0) value = 0.0;
        values[count++] = value;
        index = (size_t)(end - clip);
        if (strncmp(clip + index, "cm", 2u) == 0) {
            values[count - 1u] *= 72.0 / 2.54;
            index += 2u;
        } else if (strncmp(clip + index, "mm", 2u) == 0) {
            values[count - 1u] *= 72.0 / 25.4;
            index += 2u;
        } else if (strncmp(clip + index, "in", 2u) == 0) {
            values[count - 1u] *= 72.0;
            index += 2u;
        } else if (strncmp(clip + index, "pc", 2u) == 0) {
            values[count - 1u] *= 12.0;
            index += 2u;
        } else if (strncmp(clip + index, "pt", 2u) == 0) {
            index += 2u;
        } else if (strncmp(clip + index, "px", 2u) == 0) {
            values[count - 1u] *= 0.75;
            index += 2u;
        }
        while (clip[index] == ' ' || clip[index] == '\t') ++index;
        if (count < 4) {
            if (clip[index] != ',') return 0;
            ++index;
            while (clip[index] == ' ' || clip[index] == '\t') ++index;
        }
    }
    return 1;
}

static int odp_apply_clip(QwImage *image, QwXmlNode *frame, QwXmlNode *image_node,
                          double display_width, double display_height)
{
    const char *clip = frame ? qw_xml_att(frame, "fo:clip") : NULL;
    double values[4];
    double left_points;
    double top_points;
    double right_points;
    double bottom_points;
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
    uint32_t width;
    uint32_t height;
    unsigned channels;
    uint8_t *pixels;
    uint32_t row;
    if (!clip && image_node) clip = qw_xml_att(image_node, "fo:clip");
    if (!clip || display_width <= 0.0 || display_height <= 0.0) return 1;
    if (!odp_parse_clip_rect(clip, values)) return 1;
    top_points = values[0];
    right_points = values[1];
    bottom_points = values[2];
    left_points = values[3];
    left = (uint32_t)((double)image->width * left_points / display_width);
    top = (uint32_t)((double)image->height * top_points / display_height);
    right = image->width -
        (uint32_t)((double)image->width * right_points / display_width);
    bottom = image->height -
        (uint32_t)((double)image->height * bottom_points / display_height);
    if (right <= left || bottom <= top) return 1;
    width = right - left;
    height = bottom - top;
    channels = odp_image_channels(image->format);
    pixels = (uint8_t *)malloc((size_t)width * channels * height);
    if (!pixels) return 0;
    for (row = 0u; row < height; ++row) {
        memcpy(pixels + (size_t)row * width * channels,
               image->pixels + (size_t)(row + top) * image->stride +
                   (size_t)left * channels,
               (size_t)width * channels);
    }
    free(image->pixels);
    image->pixels = pixels;
    image->width = width;
    image->height = height;
    image->stride = (size_t)width * channels;
    image->size = image->stride * height;
    return 1;
}

static int odp_html_number_literal(OdpReader *reader, int value)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%d", value);
    if (length <= 0 || (size_t)length >= sizeof(text)) return 0;
    return odp_html_literal(reader, text);
}

static int odp_rotation_degrees(QwXmlNode *frame, QwXmlNode *node)
{
    const char *transform = frame ? qw_xml_att(frame, "draw:transform") : NULL;
    const char *cursor;
    double value;
    char *end = NULL;
    if (!transform || strncmp(transform, "rotate(", 7u) != 0) {
        transform = frame ? qw_xml_att(frame, "svg:transform") : NULL;
        if (!transform || strncmp(transform, "rotate(", 7u) != 0)
            transform = node ? qw_xml_att(node, "draw:transform") : NULL;
    }
    if (!transform || strncmp(transform, "rotate(", 7u) != 0) return 0;
    cursor = transform + 7u;
    value = strtod(cursor, &end);
    if (end == cursor) return 0;
    if (end && (end[0] == 'r' || end[0] == 'R')) value = value * 180.0 / 3.14159265358979323846;
    {
        long degrees = (long)(value >= 0.0 ? value + 0.5 : value - 0.5);
        degrees %= 360L;
        if (degrees < 0) degrees += 360L;
        return (int)degrees;
    }
}

static int odp_emit_picture(OdpReader *reader, QwXmlNode *node,
                            QwXmlNode *frame, double display_width,
                            double display_height)
{
    static const char omitted[] =
        "<p class=\"omitted\">[embedded object omitted]</p>";
    const char *href = qw_xml_att(node, "xlink:href");
    void *media = NULL;
    size_t media_size = 0u;
    uint8_t *decoded = NULL;
    size_t decoded_size = 0u;
    QwImage image;
    uint8_t *png = NULL;
    size_t png_size = 0u;
    unsigned width;
    unsigned height;
    int rotation = odp_rotation_degrees(frame, node);

    if (reader->image_count >= ODP_MAX_IMAGES)
        return odp_html_literal(reader, omitted);
    if (!href || strstr(href, "://")) {
        /* Flat FODP embeds the payload in office:binary-data. */
        QwXmlNode *binary = NULL;
        QwXmlNode *child;
        for (child = qw_xml_down(node); child; child = qw_xml_next(child)) {
            if (odp_is_tag(child, "binary-data")) {
                binary = child;
                break;
            }
        }
        if (!binary) return odp_html_literal(reader, omitted);
        {
            const char *text = qw_xml_text(binary);
            if (!text) return odp_html_literal(reader, omitted);
            decoded = odp_base64_decode(text, strlen(text), &decoded_size);
            if (!decoded) return odp_html_literal(reader, omitted);
        }
        media = decoded;
        media_size = decoded_size;
    } else {
        media = odp_extract_media(reader, href, &media_size);
        if (!media) return odp_html_literal(reader, omitted);
    }
    memset(&image, 0, sizeof(image));
    if (qw_image_decode(media, media_size, &image) != QW_IMAGE_OK ||
        !image.pixels) {
        free(decoded);
        if (media != decoded) free(media);
        return odp_html_literal(reader, omitted);
    }
    if (!odp_apply_clip(&image, frame, node, display_width, display_height)) {
        qw_image_free(&image);
        free(decoded);
        if (media != decoded) free(media);
        return odp_html_literal(reader, omitted);
    }
    if (qw_image_encode(&image, QW_IMAGE_ENCODE_PNG, &png, &png_size) !=
            QW_IMAGE_OK ||
        !png || !png_size) {
        qw_image_free(&image);
        free(decoded);
        if (media != decoded) free(media);
        free(png);
        return odp_html_literal(reader, omitted);
    }
    width = display_width > 0.0 ? (unsigned)(display_width + 0.5)
                                : (image.width > 720u ? 720u : image.width);
    if (width < 8u) width = 8u;
    if (width > 720u) width = 720u;
    height = display_height > 0.0 ? (unsigned)(display_height + 0.5) : 0u;
    if (height && height < 8u) height = 8u;
    if (height > 720u) height = 720u;
    qw_image_free(&image);
    free(decoded);
    if (media != decoded) free(media);
    ++reader->image_count;
    {
        char width_text[16];
        char height_text[16];
        size_t height_length = 0u;
        int length = snprintf(width_text, sizeof(width_text), "%u", width);
        if (height) {
            int written = snprintf(height_text, sizeof(height_text), "%u",
                                   height);
            if (written <= 0 || (size_t)written >= sizeof(height_text))
                return 0;
            height_length = (size_t)written;
        }
        if (length <= 0 || (size_t)length >= sizeof(width_text) ||
            !odp_html_literal(reader, "<p class=\"picture\"") ||
            (rotation &&
             (!odp_html_literal(reader, " style=\"transform:rotate(") ||
              !odp_html_number_literal(reader, rotation) ||
              !odp_html_literal(reader, "deg)\""))) ||
            !odp_html_literal(reader, "><img alt=\"image\" width=\"") ||
            !odp_html_literal(reader, width_text) ||
            (height != 0u &&
             (!odp_html_literal(reader, "\" height=\"") ||
              !odp_html_literal(reader, height_text))) ||
            !odp_html_literal(reader, "\" src=\"data:image/png;base64,") ||
            !odp_html_base64(reader, png, png_size) ||
            !odp_html_literal(reader, "\"/></p>")) {
            free(png);
            return 0;
        }
    }
    free(png);
    return 1;
}
static int odp_render_blocks(OdpReader *reader, QwXmlNode *node, size_t depth)
{
    if (depth > ODP_MAX_XML_DEPTH) return 0;
    for (; node; node = qw_xml_next(node)) {
        const char *tag;
        if (++reader->visited_nodes > ODP_MAX_XML_NODES) return 0;
        if (odp_is_tag(node, "notes") || odp_is_tag(node, "page")) continue;
        if (odp_is_tag(node, "p") || odp_is_tag(node, "h")) {
            tag = odp_is_tag(node, "h") ? "h3" : "p";
            if (!odp_html_literal(reader, "<") ||
                !odp_html_literal(reader, tag) ||
                !odp_html_literal(reader, ">") ||
                !odp_render_inline(reader, qw_xml_down(node), depth + 1u) ||
                !odp_html_literal(reader, "</") ||
                !odp_html_literal(reader, tag) ||
                !odp_html_literal(reader, ">")) return 0;
        } else if (odp_is_tag(node, "list")) {
            if (!odp_html_literal(reader, "<ul>") ||
                !odp_render_blocks(reader, qw_xml_down(node), depth + 1u) ||
                !odp_html_literal(reader, "</ul>")) return 0;
        } else if (odp_is_tag(node, "list-item")) {
            if (!odp_html_literal(reader, "<li>") ||
                !odp_render_blocks(reader, qw_xml_down(node), depth + 1u) ||
                !odp_html_literal(reader, "</li>")) return 0;
        } else if (odp_is_tag(node, "frame")) {
            QwXmlNode *child;
            QwXmlNode *image = NULL;
            for (child = qw_xml_down(node); child;
                 child = qw_xml_next(child)) {
                if (odp_is_tag(child, "image")) {
                    image = child;
                    break;
                }
            }
            if (image) {
                double width = odp_length_points(qw_xml_att(node, "svg:width"));
                double height_points =
                    odp_length_points(qw_xml_att(node, "svg:height"));
                if (!odp_emit_picture(reader, image, node, width, height_points))
                    return 0;
            } else {
                int rotation = odp_rotation_degrees(node, NULL);
                if (rotation) {
                    if (!odp_html_literal(reader,
                                          "<div class=\"textbox\" style=\"transform:rotate(") ||
                        !odp_html_number_literal(reader, rotation) ||
                        !odp_html_literal(reader, "deg)\">") ||
                        !odp_render_blocks(reader, qw_xml_down(node),
                                           depth + 1u) ||
                        !odp_html_literal(reader, "</div>"))
                        return 0;
                } else if (!odp_render_blocks(reader, qw_xml_down(node),
                                               depth + 1u)) {
                    return 0;
                }
            }
        } else if (odp_is_tag(node, "image")) {
            if (!odp_emit_picture(reader, node, NULL, 0.0, 0.0)) return 0;
        } else if (odp_is_tag(node, "object")) {
            if (!odp_html_literal(reader, "<p class=\"omitted\">[embedded object omitted]</p>"))
                return 0;
        } else if (!odp_render_blocks(reader, qw_xml_down(node), depth + 1u)) {
            return 0;
        }
    }
    return 1;
}


static int odp_has_text_node(QwXmlNode *node)
{
    for (; node; node = qw_xml_next(node)) {
        const char *text = qw_xml_text(node);
        if (text) {
            while (*text == ' ' || *text == '\t' || *text == '\r' ||
                   *text == '\n')
                ++text;
            if (*text) return 1;
        }
        if (odp_has_text_node(qw_xml_down(node))) return 1;
    }
    return 0;
}

/* draw:page carries its speaker notes in a presentation:notes child; the main
 * block renderer skips that subtree, so render it explicitly. */
static int odp_render_notes(OdpReader *reader, QwXmlNode *slide)
{
    QwXmlNode *node;
    QwXmlNode *notes = NULL;

    for (node = qw_xml_down(slide); node; node = qw_xml_next(node)) {
        if (odp_is_tag(node, "notes")) {
            notes = node;
            break;
        }
    }
    if (!notes || !odp_has_text_node(qw_xml_down(notes))) return 1;
    if (!odp_html_literal(reader, "<section class=\"notes\"><h3>Notes</h3>") ||
        !odp_render_blocks(reader, qw_xml_down(notes), 0u) ||
        !odp_html_literal(reader, "</section>"))
        return 0;
    return 1;
}
static int odp_render_slide(OdpReader *reader, QwXmlNode *slide)
{
    const char *name = qw_xml_att(slide, "draw:name");
    size_t slide_number = reader->slide_count + 1u;
    if (reader->slide_count >= ODP_MAX_SLIDES) return 0;
    ++reader->slide_count;
    if (!odp_html_literal(reader, "<section class=\"slide\"><header><h2>Slide "))
        return 0;
    {
        char number[24];
        (void)snprintf(number, sizeof(number), "%zu", slide_number);
        if (!odp_html_escape(reader, number)) return 0;
    }
    if (name && *name && (!odp_html_literal(reader, " — ") ||
                          !odp_html_escape(reader, name))) return 0;
    if (!odp_html_literal(reader, "</h2></header><div class=\"slide-content\">"))
        return 0;
    if (!odp_render_blocks(reader, qw_xml_down(slide), 0u) ||
        !odp_html_literal(reader, "</div>") ||
        !odp_render_notes(reader, slide) ||
        !odp_html_literal(reader, "</section>"))
        return 0;
    return 1;
}

static void odp_set_error(char *error, size_t error_capacity,
                          const char *message)
{
    if (error && error_capacity) {
        (void)snprintf(error, error_capacity, "%s",
                       message ? message : "OpenDocument presentation parsing failed");
    }
}

static QwOdpStatus odp_build_html(const void *data, size_t size,
                                  QwOdpFormat format,
                                  const QwFormatInputResourceProfile *profile,
                                  OdpReader *reader,
                                  char *error, size_t error_capacity)
{
    QwZipArchive zip;
    void *content_data = NULL;
    size_t content_size = 0u;
    QwXmlNode *xml = NULL;
    QwXmlNode *presentation = NULL;
    QwXmlNode *slide;
    QwXmlError xml_error;
    int archive_open = 0;
    QwOdpStatus rc = QW_ODP_ERR_FORMAT;
    if (!reader) return QW_ODP_ERR_INVALID_ARGUMENT;
    memset(reader, 0, sizeof(*reader));
    memset(&zip, 0, sizeof(zip));
    if (!data || !size || format < QW_ODP_FORMAT_ODP ||
        format > QW_ODP_FORMAT_FODP) return QW_ODP_ERR_INVALID_ARGUMENT;
    if (format == QW_ODP_FORMAT_FODP) {
        if (size > ODP_MAX_FLAT_XML_BYTES) {
            odp_set_error(error, error_capacity,
                          "FODP XML exceeds the parser limit");
            return QW_ODP_ERR_RESOURCE_LIMIT;
        }
        if (!odp_flat_probe(data, size)) {
            odp_set_error(error, error_capacity,
                          "FODP is not a recognized flat presentation XML file");
            return QW_ODP_ERR_FORMAT;
        }
        content_data = malloc(size);
        if (!content_data) {
            odp_set_error(error, error_capacity,
                          "Cannot allocate FODP input buffer");
            return QW_ODP_ERR_OUT_OF_MEMORY;
        }
        memcpy(content_data, data, size);
        content_size = size;
    } else {
        if (profile) {
            archive_open = qw_zip_reader_init_mem_with_profile(
                &zip, data, size, 0u, profile);
        } else {
            archive_open = qw_zip_reader_init_mem(&zip, data, size, 0u);
        }
        if (!archive_open) {
            odp_set_error(error, error_capacity,
                          "OpenDocument presentation package could not be opened as ZIP");
            return QW_ODP_ERR_FORMAT;
        }
        reader->zip = &zip;
        content_data = odp_extract_xml(reader, "content.xml", &content_size);
        if (!content_data) {
            odp_set_error(error, error_capacity,
                          "OpenDocument presentation content.xml is missing, unsupported, or too large");
            rc = QW_ODP_ERR_FORMAT;
            goto cleanup;
        }
    }

    xml = odp_parse_xml((const uint8_t *)content_data, content_size,
                        &xml_error);
    if (!xml) {
        odp_set_error(error, error_capacity, xml_error.message);
        rc = xml_error.code == QW_XML_ERROR_RESOURCE_LIMIT
                 ? QW_ODP_ERR_RESOURCE_LIMIT : QW_ODP_ERR_DECODE;
        goto cleanup;
    }
    presentation = odp_find_descendant(reader, qw_xml_root(xml),
                                       "presentation", 0u);
    if (!presentation) {
        odp_set_error(error, error_capacity,
                      "OpenDocument content has no presentation body");
        rc = QW_ODP_ERR_FORMAT;
        goto cleanup;
    }
    reader->html.limit = (size_t)ODP_MAX_HTML_BYTES + 1u;
    if (!odp_html_literal(reader,
            "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
            "@page{size:landscape;margin:12mm}body{font-family:sans-serif;"
            "font-size:18pt;color:#151515;line-height:1.35}"
            ".slide{break-after:page;page-break-after:always;min-height:90vh;"
            "display:flex;flex-direction:column;justify-content:center;"
            "padding:1rem 2rem}header{border-bottom:1px solid #aaa;"
            "margin-bottom:1.2rem}h2{font-size:12pt;font-weight:400;color:#555}"
            "h3{font-size:30pt}p{margin:.35em 0}li{margin:.3em 0}"
            ".omitted{font-size:10pt;color:#666}ul{margin-top:.25em}"
            "</style></head><body>")) {
        odp_set_error(error, error_capacity,
                      "Presentation HTML header exceeds output limits");
        rc = QW_ODP_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    for (slide = qw_xml_down(presentation); slide; slide = qw_xml_next(slide)) {
        if (odp_is_tag(slide, "page") && !odp_render_slide(reader, slide)) {
            odp_set_error(error, error_capacity,
                          "OpenDocument presentation exceeds conversion resource limits");
            rc = QW_ODP_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
    }
    if (!reader->slide_count) {
        odp_set_error(error, error_capacity,
                      "OpenDocument presentation has no slides");
        rc = QW_ODP_ERR_FORMAT;
        goto cleanup;
    }
    if (!odp_html_literal(reader, "</body></html>")) {
        odp_set_error(error, error_capacity,
                      "Presentation HTML output exceeds the size limit");
        rc = QW_ODP_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    rc = QW_ODP_OK;

cleanup:
    qw_xml_drop(xml);
    free(content_data);
    if (archive_open) qw_zip_reader_end(&zip);
    return rc;
}

#if !defined(QUIREWISE_STANDALONE_ODP)
static int odp_open_layout(const void *data, size_t size,
                           QwInputFormat input_format,
                           const QwConvertOptions *options,
                           QwLayoutDocument *out_document,
                           QwConvertReport *report)
{
    OdpReader reader;
    QwOdpStatus rc;
    QwOdpFormat format;
    char error[160] = {0};
    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    memset(&reader, 0, sizeof(reader));
    if (input_format == QW_FORMAT_ODP) format = QW_ODP_FORMAT_ODP;
    else if (input_format == QW_FORMAT_OTP) format = QW_ODP_FORMAT_OTP;
    else if (input_format == QW_FORMAT_FODP) format = QW_ODP_FORMAT_FODP;
    else return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = odp_build_html(data, size, format,
        format == QW_ODP_FORMAT_FODP ? NULL :
            QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
        &reader, error, sizeof(error));
    if (rc != QW_ODP_OK) {
        free(reader.html.data);
        if (report) (void)snprintf(report->note, sizeof(report->note),
                                   "OpenDocument presentation parse: %.120s",
                                   error);
        switch (rc) {
        case QW_ODP_ERR_INVALID_ARGUMENT: return QW_STATUS_ERR_INVALID_ARGUMENT;
        case QW_ODP_ERR_OUT_OF_MEMORY: return QW_STATUS_ERR_OUT_OF_MEMORY;
        case QW_ODP_ERR_UNSUPPORTED: return QW_STATUS_ERR_UNSUPPORTED;
        case QW_ODP_ERR_RESOURCE_LIMIT: return QW_STATUS_ERR_RESOURCE_LIMIT;
        case QW_ODP_ERR_DECODE: return QW_STATUS_ERR_DECODE;
        default: return QW_STATUS_ERR_FORMAT;
        }
    }
    out_document->html_utf8 = (const uint8_t *)reader.html.data;
    out_document->html_size = reader.html.size;
    out_document->context = reader.html.data;
    out_document->dispose = free;
    if (report) {
        if (input_format == QW_FORMAT_OTP) {
            (void)snprintf(report->note, sizeof(report->note),
                "OTP template slides opened as landscape layout HTML; source page size, master pages, shapes, images, animation and template behavior are not preserved");
        } else if (input_format == QW_FORMAT_FODP) {
            (void)snprintf(report->note, sizeof(report->note),
                "FODP flat presentation pages and text opened as landscape layout HTML; source page size, master pages, shapes, images, animation and styles are not preserved");
        } else {
            (void)snprintf(report->note, sizeof(report->note),
                "ODP slide text and lists opened as landscape layout HTML; source page size, master pages, shapes, images, animation and styles are not preserved");
        }
    }
    return QW_STATUS_OK;
}
#endif

QwOdpStatus qw_odp_parse(QwOdpFormat format, const void *data, size_t size,
                         QwOdpDocument **out_document)
{
    OdpReader reader;
    QwOdpDocument *document;
    QwOdpStatus status;
    if (!out_document) return QW_ODP_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    memset(&reader, 0, sizeof(reader));
    status = odp_build_html(data, size, format, NULL, &reader, NULL, 0u);
    if (status != QW_ODP_OK) {
        free(reader.html.data);
        return status;
    }
    document = (QwOdpDocument *)calloc(1u, sizeof(*document));
    if (!document) {
        free(reader.html.data);
        return QW_ODP_ERR_OUT_OF_MEMORY;
    }
    document->html = reader.html;
    document->slide_count = reader.slide_count;
    *out_document = document;
    return QW_ODP_OK;
}

const uint8_t *qw_odp_document_html(const QwOdpDocument *document,
                                   size_t *out_size)
{
    if (out_size) *out_size = document ? document->html.size : 0u;
    return document ? (const uint8_t *)document->html.data : NULL;
}

size_t qw_odp_document_slide_count(const QwOdpDocument *document)
{
    return document ? document->slide_count : 0u;
}

void qw_odp_document_free(QwOdpDocument *document)
{
    if (!document) return;
    free(document->html.data);
    free(document);
}

#if !defined(QUIREWISE_STANDALONE_ODP)
const QwFormatDescriptor qw_format_odp = {
    .id = QW_FORMAT_ODP,
    .name = "ODP",
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE |
                    QW_FORMAT_CAP_ARCHIVE_PROBE,
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = odp_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
    .convert_to_pdf = NULL
};

const QwFormatDescriptor qw_format_otp = {
    .id = QW_FORMAT_OTP,
    .name = "OTP",
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE |
                    QW_FORMAT_CAP_ARCHIVE_PROBE,
    .probe = NULL,
    .open_memory = NULL,
    .open_layout = odp_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
    .convert_to_pdf = NULL
};

const QwFormatDescriptor qw_format_fodp = {
    .id = QW_FORMAT_FODP,
    .name = "FODP",
    .capabilities = QW_FORMAT_CAP_MULTI_PAGE,
    .probe = odp_flat_probe,
    .open_memory = NULL,
    .open_layout = odp_open_layout,
    .open_audio = NULL,
    .max_input_bytes = ODP_MAX_FLAT_XML_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL
};
#endif
