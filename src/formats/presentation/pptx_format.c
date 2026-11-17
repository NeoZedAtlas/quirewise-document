#include "../../core/format_registry.h"
#include "../../core/html_output.h"
#include "../../core/xml_parser.h"

#include <quirewise/image.h>
#include "../archive/zip_native.h"
#include "pptx_native.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PPTX_MAX_XML_PART_BYTES = 32u * 1024u * 1024u,
    PPTX_MAX_TOTAL_XML_BYTES = 128u * 1024u * 1024u,
    PPTX_MAX_HTML_BYTES = 64u * 1024u * 1024u,
    PPTX_MAX_XML_NODES = 2000000u,
    PPTX_MAX_XML_DEPTH = 128u,
    PPTX_MAX_SLIDES = 1000u,
    PPTX_MAX_PATH_BYTES = 1024u,
    PPTX_MAX_SLIDE_RELS = 512u,
    PPTX_MAX_IMAGES_PER_SLIDE = 64u,
    PPTX_MAX_MEDIA_BYTES = 16u * 1024u * 1024u
};

typedef struct PptxRelationship {
    char *id;
    char *target;
} PptxRelationship;

typedef struct PptxReader {
    QwZipArchive *zip;
    QwTextBuffer html;
    size_t visited_nodes;
    size_t total_xml_bytes;
    size_t slide_count;
    size_t slide_image_count;
    int output_limit_hit;
    const char *error;
    const char *slide_path;
    PptxRelationship *slide_rels;
    size_t slide_rel_count;
} PptxReader;

static int pptx_html_literal(PptxReader *reader, const char *text)
{
    return reader && qw_html_output_literal(&reader->html, text,
        PPTX_MAX_HTML_BYTES, &reader->output_limit_hit);
}

static int pptx_html_escape(PptxReader *reader, const char *text)
{
    return reader && qw_html_output_escape(&reader->html, text,
        PPTX_MAX_HTML_BYTES, &reader->output_limit_hit);
}

static const char *pptx_local_name(const char *qualified_name)
{
    const char *colon;
    if (!qualified_name) return "";
    colon = strrchr(qualified_name, ':');
    return colon ? colon + 1u : qualified_name;
}

static int pptx_is_tag(QwXmlNode *node, const char *local_name)
{
    const char *tag = qw_xml_tag(node);
    return tag && strcmp(pptx_local_name(tag), local_name) == 0;
}

static int pptx_copy_string(char **out, const char *value)
{
    size_t size;
    char *copy;
    if (!out || !value) return 0;
    size = strlen(value);
    copy = (char *)malloc(size + 1u);
    if (!copy) return 0;
    memcpy(copy, value, size + 1u);
    *out = copy;
    return 1;
}

static int pptx_path_is_safe(const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;
    const char *segment;
    if (!path || !*path || *path == '/' || *path == '\\') return 0;
    segment = path;
    for (;;) {
        unsigned char ch = *cursor;
        if (ch == '\0' || ch == '/') {
            size_t segment_size = (size_t)((const char *)cursor - segment);
            if (!segment_size ||
                (segment_size == 1u && segment[0] == '.') ||
                (segment_size == 2u && segment[0] == '.' &&
                 segment[1] == '.')) return 0;
            if (ch == '\0') return 1;
            segment = (const char *)cursor + 1u;
        } else if (ch == '\\' || ch == '%' || ch == '?' || ch == '#' ||
                   ch == ':' || ch < 0x20u || ch == 0x7fu) {
            return 0;
        }
        ++cursor;
    }
}

static int pptx_resolve_target(const char *source_part, const char *target,
                               char *out_path, size_t out_capacity)
{
    char combined[PPTX_MAX_PATH_BYTES * 2u];
    char *segments[PPTX_MAX_PATH_BYTES / 2u];
    size_t combined_size = 0u;
    size_t depth = 0u;
    size_t i;
    const char *target_start = target;
    int absolute;
    if (!source_part || !target || !out_path || out_capacity == 0u)
        return 0;
    absolute = target[0] == '/';
    if (absolute) {
        ++target_start;
    } else {
        const char *slash = strrchr(source_part, '/');
        if (slash) {
            combined_size = (size_t)(slash - source_part + 1u);
            if (combined_size >= sizeof(combined)) return 0;
            memcpy(combined, source_part, combined_size);
        }
    }
    if (strlen(target_start) >= sizeof(combined) - combined_size) return 0;
    memcpy(combined + combined_size, target_start,
           strlen(target_start) + 1u);
    for (i = 0u; combined[i]; ++i) {
        unsigned char ch = (unsigned char)combined[i];
        if (ch == '\\' || ch == '%' || ch == '?' || ch == '#' ||
            ch == ':' || ch < 0x20u || ch == 0x7fu) return 0;
    }
    {
        char *cursor = combined;
        while (*cursor) {
            char *start;
            size_t segment_size;
            while (*cursor == '/') ++cursor;
            if (!*cursor) break;
            start = cursor;
            while (*cursor && *cursor != '/') ++cursor;
            segment_size = (size_t)(cursor - start);
            if (segment_size == 1u && start[0] == '.') continue;
            if (segment_size == 2u && start[0] == '.' &&
                start[1] == '.') {
                if (!depth) return 0;
                --depth;
                continue;
            }
            if (depth >= sizeof(segments) / sizeof(segments[0])) return 0;
            if (*cursor) *cursor++ = '\0';
            segments[depth++] = start;
        }
    }
    if (!depth) return 0;
    {
        size_t output_size = 0u;
        for (i = 0u; i < depth; ++i) {
            size_t segment_size = strlen(segments[i]);
            if (output_size && output_size + 1u >= out_capacity) return 0;
            if (output_size) out_path[output_size++] = '/';
            if (segment_size > out_capacity - output_size - 1u) return 0;
            memcpy(out_path + output_size, segments[i], segment_size);
            output_size += segment_size;
        }
        out_path[output_size] = '\0';
    }
    return pptx_path_is_safe(out_path);
}

static void *pptx_extract_xml(PptxReader *reader, const char *path,
                              size_t *out_size)
{
    size_t index;
    size_t extracted_size = 0u;
    QwZipEntryInfo stat;
    void *contents;
    if (out_size) *out_size = 0u;
    if (!reader || !reader->zip || !out_size ||
        !pptx_path_is_safe(path)) return NULL;
    index = qw_zip_reader_locate_file(reader->zip, path, NULL,
                                      QW_ZIP_CASE_SENSITIVE);
    if (index == (size_t)-1 ||
        !qw_zip_reader_file_stat(reader->zip, index, &stat) ||
        !stat.uncompressed_size ||
        stat.uncompressed_size > PPTX_MAX_XML_PART_BYTES ||
        stat.uncompressed_size > PPTX_MAX_TOTAL_XML_BYTES -
            reader->total_xml_bytes ||
        !qw_zip_reader_is_file_supported(reader->zip, index)) return NULL;
    contents = qw_zip_reader_extract_to_heap(reader->zip, index,
                                              &extracted_size, 0u);
    if (!contents || extracted_size != stat.uncompressed_size ||
        extracted_size > PPTX_MAX_XML_PART_BYTES ||
        extracted_size > PPTX_MAX_TOTAL_XML_BYTES -
            reader->total_xml_bytes) {
        free(contents);
        return NULL;
    }
    reader->total_xml_bytes += extracted_size;
    *out_size = extracted_size;
    return contents;
}

static QwXmlNode *pptx_parse_xml(const uint8_t *data, size_t size)
{
    return qw_xml_parse(data, size, NULL);
}

static QwXmlNode *pptx_parse_part(PptxReader *reader, const char *path)
{
    size_t size = 0u;
    void *contents = pptx_extract_xml(reader, path, &size);
    QwXmlNode *xml = NULL;
    if (!contents) return NULL;
    xml = pptx_parse_xml((const uint8_t *)contents, size);
    free(contents);
    return xml;
}

static QwXmlNode *pptx_find_descendant(PptxReader *reader, QwXmlNode *node,
                                    const char *local_name, size_t depth)
{
    QwXmlNode *found;
    if (!reader || depth > PPTX_MAX_XML_DEPTH) return NULL;
    for (; node; node = qw_xml_next(node)) {
        if (++reader->visited_nodes > PPTX_MAX_XML_NODES) return NULL;
        if (pptx_is_tag(node, local_name)) return node;
        found = pptx_find_descendant(reader, qw_xml_down(node), local_name,
                                     depth + 1u);
        if (found || reader->visited_nodes > PPTX_MAX_XML_NODES) return found;
    }
    return NULL;
}

static int pptx_has_suffix(const char *value, const char *suffix)
{
    size_t value_size;
    size_t suffix_size;
    if (!value || !suffix) return 0;
    value_size = strlen(value);
    suffix_size = strlen(suffix);
    return value_size >= suffix_size &&
           memcmp(value + value_size - suffix_size, suffix, suffix_size) == 0;
}

static void pptx_free_relationships(PptxRelationship *relationships,
                                   size_t count)
{
    size_t i;
    for (i = 0u; i < count; ++i) {
        free(relationships[i].id);
        free(relationships[i].target);
    }
    free(relationships);
}

static const char *pptx_relationship_target(
    const PptxRelationship *relationships, size_t count, const char *id)
{
    size_t i;
    if (!id) return NULL;
    for (i = 0u; i < count; ++i) {
        if (strcmp(relationships[i].id, id) == 0)
            return relationships[i].target;
    }
    return NULL;
}


static int pptx_html_base64(PptxReader *reader, const uint8_t *data,
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
                                       PPTX_MAX_HTML_BYTES,
                                       &reader->output_limit_hit))
                return 0;
            used = 0u;
        }
    }
    if (used && !qw_html_output_append(&reader->html, chunk, used,
                                       PPTX_MAX_HTML_BYTES,
                                       &reader->output_limit_hit))
        return 0;
    return 1;
}

/* Normalise "a/./b/../c" into "a/c". */
static int pptx_normalize_part(const char *combined, char *out,
                               size_t out_capacity)
{
    size_t out_length = 0u;
    const char *cursor = combined;

    if (!combined || !out || out_capacity < 2u) return 0;
    while (*cursor) {
        const char *slash = strchr(cursor, '/');
        size_t length = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (length == 0u || (length == 1u && cursor[0] == '.')) {
            /* skip */
        } else if (length == 2u && cursor[0] == '.' && cursor[1] == '.') {
            while (out_length > 0u && out[out_length - 1u] != '/') --out_length;
            if (out_length > 0u) --out_length;
        } else {
            if (out_length > 0u && out_length + 1u < out_capacity)
                out[out_length++] = '/';
            if (out_length + length + 1u > out_capacity) return 0;
            memcpy(out + out_length, cursor, length);
            out_length += length;
        }
        if (!slash) break;
        cursor = slash + 1u;
    }
    if (out_length == 0u) return 0;
    out[out_length] = '\0';
    return 1;
}

static int pptx_resolve_media_path(const char *slide_path,
                                   const char *target, char *out,
                                   size_t out_capacity)
{
    char combined[PPTX_MAX_PATH_BYTES * 2u];
    const char *slash;

    if (!target || !target[0]) return 0;
    if (target[0] == '/') {
        if (strlen(target + 1u) + 1u > sizeof(combined)) return 0;
        memcpy(combined, target + 1u, strlen(target + 1u) + 1u);
        return pptx_normalize_part(combined, out, out_capacity);
    }
    slash = slide_path ? strrchr(slide_path, '/') : NULL;
    if (slash) {
        size_t directory = (size_t)(slash - slide_path + 1u);
        if (directory + strlen(target) + 1u > sizeof(combined)) return 0;
        memcpy(combined, slide_path, directory);
        memcpy(combined + directory, target, strlen(target) + 1u);
    } else {
        if (strlen(target) + 1u > sizeof(combined)) return 0;
        memcpy(combined, target, strlen(target) + 1u);
    }
    return pptx_normalize_part(combined, out, out_capacity);
}

static void *pptx_extract_media(PptxReader *reader, const char *path,
                                size_t *out_size)
{
    size_t index;
    size_t extracted_size = 0u;
    QwZipEntryInfo stat;
    void *contents;

    if (out_size) *out_size = 0u;
    if (!reader || !reader->zip || !path || !out_size ||
        !pptx_path_is_safe(path))
        return NULL;
    index = qw_zip_reader_locate_file(reader->zip, path, NULL,
                                      QW_ZIP_CASE_SENSITIVE);
    if (index == (size_t)-1 ||
        !qw_zip_reader_file_stat(reader->zip, index, &stat) ||
        !stat.uncompressed_size || stat.uncompressed_size > PPTX_MAX_MEDIA_BYTES ||
        !qw_zip_reader_is_file_supported(reader->zip, index))
        return NULL;
    contents = qw_zip_reader_extract_to_heap(reader->zip, index,
                                              &extracted_size, 0u);
    if (!contents || extracted_size != stat.uncompressed_size ||
        extracted_size > PPTX_MAX_MEDIA_BYTES) {
        free(contents);
        return NULL;
    }
    *out_size = extracted_size;
    return contents;
}

static int pptx_load_slide_rels(PptxReader *reader, const char *slide_path,
                                PptxRelationship **out_rels,
                                size_t *out_count)
{
    char rels_path[PPTX_MAX_PATH_BYTES];
    const char *slash = slide_path ? strrchr(slide_path, '/') : NULL;
    size_t directory = slash ? (size_t)(slash - slide_path + 1u) : 0u;
    const char *base = slash ? slash + 1u : slide_path;
    QwXmlNode *xml;
    QwXmlNode *root;
    QwXmlNode *node;
    PptxRelationship *rels = NULL;
    size_t count = 0u;

    *out_rels = NULL;
    *out_count = 0u;
    if (!slide_path || !base || !base[0]) return 1;
    if (directory + 6u + strlen(base) + 6u >= sizeof(rels_path)) return 1;
    memcpy(rels_path, slide_path, directory);
    memcpy(rels_path + directory, "_rels/", 6u);
    memcpy(rels_path + directory + 6u, base, strlen(base));
    memcpy(rels_path + directory + 6u + strlen(base), ".rels", 6u);
    rels_path[directory + 6u + strlen(base) + 5u] = '\0';
    xml = pptx_parse_part(reader, rels_path);
    if (!xml) return 1; /* no slide relationships is not an error */
    root = qw_xml_root(xml);
    for (node = qw_xml_down(root); node; node = qw_xml_next(node)) {
        const char *id;
        const char *target;
        if (!pptx_is_tag(node, "Relationship")) continue;
        id = qw_xml_att(node, "Id");
        target = qw_xml_att(node, "Target");
        if (!id || !target || count >= PPTX_MAX_SLIDE_RELS) continue;
        {
            PptxRelationship *grown =
                (PptxRelationship *)realloc(rels, (count + 1u) * sizeof(*grown));
            if (!grown) {
                pptx_free_relationships(rels, count);
                qw_xml_drop(xml);
                return 0;
            }
            rels = grown;
            memset(&rels[count], 0, sizeof(rels[count]));
            if (!pptx_copy_string(&rels[count].id, id) ||
                !pptx_copy_string(&rels[count].target, target)) {
                pptx_free_relationships(rels, count + 1u);
                qw_xml_drop(xml);
                return 0;
            }
            ++count;
        }
    }
    qw_xml_drop(xml);
    *out_rels = rels;
    *out_count = count;
    return 1;
}


static unsigned pptx_image_channels(QwImagePixelFormat format)
{
    return format == QW_IMAGE_PIXEL_GRAY8 ? 1u : 3u;
}

/* DrawingML srcRect values are fractions in 1/100000 units. */
static int pptx_apply_src_rect(PptxReader *reader, QwXmlNode *picture,
                               QwImage *image)
{
    QwXmlNode *rect = pptx_find_descendant(reader, qw_xml_down(picture),
                                           "srcRect", 0u);
    const char *values[4];
    long parts[4] = {0, 0, 0, 0};
    static const char *const names[4] = {"l", "t", "r", "b"};
    size_t index;
    unsigned channels;
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;
    uint32_t row;
    if (!rect || !image || !image->pixels) return 1;
    for (index = 0u; index < 4u; ++index) {
        values[index] = qw_xml_att(rect, names[index]);
        if (values[index] && values[index][0]) {
            char *end = NULL;
            long parsed = strtol(values[index], &end, 10);
            if (end && *end == '\0' && parsed >= 0) parts[index] = parsed;
        }
    }
    if (parts[0] <= 0 && parts[1] <= 0 && parts[2] <= 0 && parts[3] <= 0)
        return 1;
    left = (uint32_t)((uint64_t)image->width * (uint64_t)parts[0] / 100000u);
    top = (uint32_t)((uint64_t)image->height * (uint64_t)parts[1] / 100000u);
    right = image->width -
        (uint32_t)((uint64_t)image->width * (uint64_t)parts[2] / 100000u);
    bottom = image->height -
        (uint32_t)((uint64_t)image->height * (uint64_t)parts[3] / 100000u);
    if (right <= left || bottom <= top) return 1;
    width = right - left;
    height = bottom - top;
    channels = pptx_image_channels(image->format);
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

static int pptx_emit_picture(PptxReader *reader, QwXmlNode *picture,
                             size_t depth)
{
    QwXmlNode *blip;
    const char *embed;
    const char *target;
    char media_path[PPTX_MAX_PATH_BYTES];
    void *media = NULL;
    size_t media_size = 0u;
    QwImage image;
    uint8_t *png = NULL;
    size_t png_size = 0u;
    const char *alt = "image";
    long width = 0;
    long rotation = 0;
    static const char omitted[] =
        "<p class=\"omitted\">[image omitted]</p>";

    if (reader->slide_image_count >= PPTX_MAX_IMAGES_PER_SLIDE)
        return pptx_html_literal(reader, omitted);
    blip = pptx_find_descendant(reader, qw_xml_down(picture), "blip", 0u);
    embed = blip ? qw_xml_att(blip, "r:embed") : NULL;
    target = pptx_relationship_target(reader->slide_rels,
                                      reader->slide_rel_count, embed);
    if (!target ||
        !pptx_resolve_media_path(reader->slide_path, target, media_path,
                                 sizeof(media_path)))
        return pptx_html_literal(reader, omitted);
    media = pptx_extract_media(reader, media_path, &media_size);
    if (!media) return pptx_html_literal(reader, omitted);
    memset(&image, 0, sizeof(image));
    if (qw_image_decode(media, media_size, &image) != QW_IMAGE_OK ||
        !image.pixels) {
        free(media);
        return pptx_html_literal(reader, omitted);
    }
    if (!pptx_apply_src_rect(reader, picture, &image)) {
        qw_image_free(&image);
        free(media);
        return pptx_html_literal(reader, omitted);
    }
    if (qw_image_encode(&image, QW_IMAGE_ENCODE_PNG, &png, &png_size) !=
            QW_IMAGE_OK ||
        !png || !png_size) {
        qw_image_free(&image);
        free(media);
        free(png);
        return pptx_html_literal(reader, omitted);
    }
    {
        QwXmlNode *cnv = pptx_find_descendant(reader, qw_xml_down(picture),
                                              "cNvPr", 0u);
        if (cnv) {
            const char *description = qw_xml_att(cnv, "descr");
            const char *name = qw_xml_att(cnv, "name");
            if (description && description[0]) alt = description;
            else if (name && name[0]) alt = name;
        }
    }
    {
        QwXmlNode *xfrm = pptx_find_descendant(reader, qw_xml_down(picture),
                                               "xfrm", 0u);
        QwXmlNode *extent = xfrm
            ? pptx_find_descendant(reader, qw_xml_down(xfrm), "ext", 0u)
            : NULL;
        const char *cx = extent ? qw_xml_att(extent, "cx") : NULL;
        const char *rot = xfrm ? qw_xml_att(xfrm, "rot") : NULL;
        if (cx && cx[0]) width = strtol(cx, NULL, 10);
        if (rot && rot[0]) {
            long parsed = strtol(rot, NULL, 10);
            rotation = parsed / 60000L;
            rotation %= 360L;
            if (rotation < 0) rotation += 360L;
        }
    }
    if (width <= 0) width = 320 * 12700L; /* 320 pt fallback */
    width /= 12700L;
    if (width < 1) width = 1;
    if (width > 2000) width = 2000;
    ++reader->slide_image_count;
    qw_image_free(&image);
    free(media);
    {
        char width_text[32];
        char rotation_text[32];
        int rotation_length = snprintf(rotation_text, sizeof(rotation_text),
                                       "%ld", rotation);
        int width_length = snprintf(width_text, sizeof(width_text), "%ld",
                                    width);
        if (width_length <= 0 ||
            (size_t)width_length >= sizeof(width_text) ||
            (rotation && (rotation_length <= 0 ||
                          (size_t)rotation_length >= sizeof(rotation_text))) ||
            !pptx_html_literal(reader, "<p class=\"picture\"") ||
            (rotation &&
             (!pptx_html_literal(reader, " style=\"transform:rotate(") ||
              !pptx_html_literal(reader, rotation_text) ||
              !pptx_html_literal(reader, "deg)\""))) ||
            !pptx_html_literal(reader, "><img alt=\"") ||
            !pptx_html_escape(reader, alt) ||
            !pptx_html_literal(reader, "\" width=\"") ||
            !pptx_html_literal(reader, width_text) ||
            !pptx_html_literal(reader, "\" src=\"data:image/png;base64,") ||
            !pptx_html_base64(reader, png, png_size) ||
            !pptx_html_literal(reader, "\"/></p>")) {
            free(png);
            return 0;
        }
    }
    free(png);
    return 1;
}
static char **pptx_collect_slide_paths(PptxReader *reader, size_t *out_count)
{
    PptxRelationship *relationships = NULL;
    size_t relationship_count = 0u;
    char **paths = NULL;
    size_t path_count = 0u;
    QwXmlNode *rels_xml = NULL;
    QwXmlNode *presentation_xml = NULL;
    int success = 0;
    if (out_count) *out_count = 0u;
    if (!reader || !out_count) return NULL;
#define PPTX_PATH_FAIL(message) do { \
    reader->error = (message); \
    goto cleanup; \
} while (0)
    relationships = (PptxRelationship *)calloc(
        PPTX_MAX_SLIDES, sizeof(*relationships));
    paths = (char **)calloc(PPTX_MAX_SLIDES, sizeof(*paths));
    if (!relationships || !paths)
        PPTX_PATH_FAIL("Could not allocate presentation relationship table");

    rels_xml = pptx_parse_part(reader, "ppt/_rels/presentation.xml.rels");
    if (!rels_xml)
        PPTX_PATH_FAIL("Presentation relationships part is missing or too large");
    {
        QwXmlNode *root = qw_xml_root(rels_xml);
        QwXmlNode *node;
        if (!pptx_is_tag(root, "Relationships"))
            PPTX_PATH_FAIL("Presentation relationships part has an invalid root");
        for (node = qw_xml_down(root); node; node = qw_xml_next(node)) {
            const char *id;
            const char *target;
            const char *type;
            const char *target_mode;
            char resolved[PPTX_MAX_PATH_BYTES];
            size_t i;
            if (++reader->visited_nodes > PPTX_MAX_XML_NODES)
                PPTX_PATH_FAIL("Presentation relationships exceed XML node limits");
            if (!pptx_is_tag(node, "Relationship")) continue;
            type = qw_xml_att(node, "Type");
            target_mode = qw_xml_att(node, "TargetMode");
            if (!type || !pptx_has_suffix(type, "/slide") ||
                (target_mode && strcmp(target_mode, "External") == 0))
                continue;
            id = qw_xml_att(node, "Id");
            target = qw_xml_att(node, "Target");
            if (!id || !*id || !target ||
                !pptx_resolve_target("ppt/presentation.xml", target,
                                     resolved, sizeof(resolved)) ||
                relationship_count >= PPTX_MAX_SLIDES) {
                PPTX_PATH_FAIL("Presentation contains an invalid slide relationship");
            }
            for (i = 0u; i < relationship_count; ++i) {
                if (strcmp(relationships[i].id, id) == 0)
                    PPTX_PATH_FAIL("Presentation contains duplicate relationship IDs");
            }
            if (!pptx_copy_string(&relationships[relationship_count].id, id))
                PPTX_PATH_FAIL("Could not store a slide relationship");
            if (!pptx_copy_string(&relationships[relationship_count].target,
                                  resolved)) {
                free(relationships[relationship_count].id);
                relationships[relationship_count].id = NULL;
                PPTX_PATH_FAIL("Could not store a slide relationship target");
            }
            ++relationship_count;
        }
    }

    presentation_xml = pptx_parse_part(reader, "ppt/presentation.xml");
    if (!presentation_xml)
        PPTX_PATH_FAIL("Presentation definition part is missing or too large");
    {
        QwXmlNode *root = qw_xml_root(presentation_xml);
        QwXmlNode *node;
        QwXmlNode *slide_list;
        if (!pptx_is_tag(root, "presentation"))
            PPTX_PATH_FAIL("Presentation package has an invalid root element");
        slide_list = pptx_find_descendant(reader, qw_xml_down(root),
                                          "sldIdLst", 0u);
        if (!slide_list) PPTX_PATH_FAIL("Presentation has no slide list");
        for (node = qw_xml_down(slide_list); node; node = qw_xml_next(node)) {
            const char *relationship_id;
            const char *target;
            if (++reader->visited_nodes > PPTX_MAX_XML_NODES)
                PPTX_PATH_FAIL("Presentation slide list exceeds XML node limits");
            if (!pptx_is_tag(node, "sldId")) continue;
            relationship_id = qw_xml_att(node, "r:id");
            if (!relationship_id) relationship_id = qw_xml_att(node, "id");
            target = pptx_relationship_target(relationships,
                                              relationship_count,
                                              relationship_id);
            if (!target || path_count >= PPTX_MAX_SLIDES ||
                !pptx_copy_string(&paths[path_count], target)) {
                PPTX_PATH_FAIL("Presentation slide order has an unresolved relationship");
            }
            ++path_count;
        }
    }
    if (!path_count) PPTX_PATH_FAIL("Presentation contains no slides");
    success = 1;

cleanup:
    qw_xml_drop(rels_xml);
    qw_xml_drop(presentation_xml);
    pptx_free_relationships(relationships, relationship_count);
    if (!success) {
        size_t i;
        if (paths) {
            for (i = 0u; i < path_count; ++i) free(paths[i]);
        }
        free(paths);
        paths = NULL;
    } else {
        *out_count = path_count;
    }
#undef PPTX_PATH_FAIL
    return paths;
}

static int pptx_render_inline(PptxReader *reader, QwXmlNode *node,
                              size_t depth)
{
    if (depth > PPTX_MAX_XML_DEPTH) return 0;
    for (; node; node = qw_xml_next(node)) {
        const char *text;
        if (++reader->visited_nodes > PPTX_MAX_XML_NODES) return 0;
        text = qw_xml_text(node);
        if (text) {
            if (!pptx_html_escape(reader, text)) return 0;
        } else if (pptx_is_tag(node, "br")) {
            if (!pptx_html_literal(reader, "<br>")) return 0;
        } else if (pptx_is_tag(node, "tab")) {
            if (!pptx_html_literal(reader, "&nbsp;&nbsp;&nbsp;")) return 0;
        } else if (!pptx_render_inline(reader, qw_xml_down(node), depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int pptx_render_nodes(PptxReader *reader, QwXmlNode *node, size_t depth)
{
    if (depth > PPTX_MAX_XML_DEPTH) return 0;
    for (; node; node = qw_xml_next(node)) {
        if (++reader->visited_nodes > PPTX_MAX_XML_NODES) return 0;
        if (pptx_is_tag(node, "p")) {
            if (!pptx_html_literal(reader, "<p>") ||
                !pptx_render_inline(reader, qw_xml_down(node), depth + 1u) ||
                !pptx_html_literal(reader, "</p>")) return 0;
        } else if (pptx_is_tag(node, "pic")) {
            if (!pptx_emit_picture(reader, node, depth + 1u)) return 0;
        } else if (pptx_is_tag(node, "oleObj")) {
            if (!pptx_html_literal(reader,
                "<p class=\"omitted\">[embedded object omitted]</p>")) return 0;
        } else if (!pptx_render_nodes(reader, qw_xml_down(node), depth + 1u)) {
            return 0;
        }
    }
    return 1;
}


static int pptx_has_text_node(QwXmlNode *node)
{
    for (; node; node = qw_xml_next(node)) {
        const char *text = qw_xml_text(node);
        if (text) {
            while (*text == ' ' || *text == '\t' || *text == '\r' ||
                   *text == '\n')
                ++text;
            if (*text) return 1;
        }
        if (pptx_has_text_node(qw_xml_down(node))) return 1;
    }
    return 0;
}

static int pptx_render_notes_blocks(PptxReader *reader, QwXmlNode *node,
                                    size_t depth)
{
    if (depth > PPTX_MAX_XML_DEPTH) return 0;
    for (; node; node = qw_xml_next(node)) {
        if (++reader->visited_nodes > PPTX_MAX_XML_NODES) return 0;
        if (pptx_is_tag(node, "p")) {
            if (!pptx_html_literal(reader, "<p>") ||
                !pptx_render_inline(reader, qw_xml_down(node), depth + 1u) ||
                !pptx_html_literal(reader, "</p>"))
                return 0;
        } else if (!pptx_render_notes_blocks(reader, qw_xml_down(node),
                                             depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

/* Speaker notes are a separate notesSlide part referenced by the slide rels. */
static int pptx_emit_notes(PptxReader *reader)
{
    size_t index;
    char notes_path[PPTX_MAX_PATH_BYTES];
    const char *target = NULL;
    QwXmlNode *xml;
    QwXmlNode *root;

    for (index = 0u; index < reader->slide_rel_count; ++index) {
        if (reader->slide_rels[index].target &&
            strstr(reader->slide_rels[index].target, "notesSlide")) {
            target = reader->slide_rels[index].target;
            break;
        }
    }
    if (!target ||
        !pptx_resolve_media_path(reader->slide_path, target, notes_path,
                                 sizeof(notes_path)))
        return 1;
    xml = pptx_parse_part(reader, notes_path);
    if (!xml) return 1; /* notes are optional */
    root = qw_xml_root(xml);
    if (!root || !pptx_has_text_node(qw_xml_down(root))) {
        qw_xml_drop(xml);
        return 1;
    }
    if (!pptx_html_literal(reader, "<section class=\"notes\"><h3>Notes</h3>") ||
        !pptx_render_notes_blocks(reader, qw_xml_down(root), 0u) ||
        !pptx_html_literal(reader, "</section>")) {
        qw_xml_drop(xml);
        return 0;
    }
    qw_xml_drop(xml);
    return 1;
}
static int pptx_render_slide_part(PptxReader *reader, const char *path,
                                  size_t slide_number)
{
    QwXmlNode *xml = NULL;
    QwXmlNode *root = NULL;
    QwXmlNode *shape_tree = NULL;
    PptxRelationship *slide_rels = NULL;
    size_t slide_rel_count = 0u;
    char number[24];
    int success = 0;
    (void)snprintf(number, sizeof(number), "%zu", slide_number);
    if (!pptx_load_slide_rels(reader, path, &slide_rels, &slide_rel_count)) {
        reader->error = "Slide relationships exceed conversion limits";
        goto cleanup;
    }
    reader->slide_rels = slide_rels;
    reader->slide_rel_count = slide_rel_count;
    reader->slide_path = path;
    reader->slide_image_count = 0u;
    xml = pptx_parse_part(reader, path);
    if (!xml) {
        reader->error = "Slide XML is missing, unsupported, or too large";
        goto cleanup;
    }
    root = qw_xml_root(xml);
    if (!pptx_is_tag(root, "sld")) {
        reader->error = "Slide part has an invalid root element";
        goto cleanup;
    }
    shape_tree = pptx_find_descendant(reader, qw_xml_down(root),
                                      "spTree", 0u);
    if (!shape_tree) {
        reader->error = "Slide has no shape tree";
        goto cleanup;
    }
    ++reader->slide_count;
    if (!pptx_html_literal(reader,
            "<section class=\"slide\"><header><h2>Slide ") ||
        !pptx_html_escape(reader, number) ||
        !pptx_html_literal(reader,
            "</h2></header><div class=\"slide-content\">") ||
        !pptx_render_nodes(reader, qw_xml_down(shape_tree), 0u) ||
        !pptx_html_literal(reader, "</div>") || !pptx_emit_notes(reader) ||
        !pptx_html_literal(reader, "</section>")) {
        reader->error = "Presentation HTML exceeds conversion resource limits";
        goto cleanup;
    }
    success = 1;

cleanup:
    pptx_free_relationships(slide_rels, slide_rel_count);
    if (reader->slide_rels == slide_rels) {
        reader->slide_rels = NULL;
        reader->slide_rel_count = 0u;
        reader->slide_path = NULL;
    }
    qw_xml_drop(xml);
    return success;
}

static int pptx_build_layout_html(const void *data, size_t size,
                                  PptxReader *reader,
                                  QwConvertReport *report)
{
    QwZipArchive zip;
    char **slide_paths = NULL;
    size_t slide_count = 0u;
    size_t i;
    int archive_open = 0;
    int rc = QW_STATUS_ERR_FORMAT;
    if (!reader || !data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(reader, 0, sizeof(*reader));
    memset(&zip, 0, sizeof(zip));
    if (!qw_zip_reader_init_mem_with_profile(
            &zip, data, size, 0u,
            QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE)) {
        if (report) (void)snprintf(report->note, sizeof(report->note),
            "PowerPoint Open XML package could not be opened as ZIP");
        return QW_STATUS_ERR_FORMAT;
    }
    archive_open = 1;
    reader->zip = &zip;
    slide_paths = pptx_collect_slide_paths(reader, &slide_count);
    if (!slide_paths) {
        if (report) (void)snprintf(report->note, sizeof(report->note),
            "PowerPoint Open XML presentation parse: %s",
            reader->error ? reader->error :
                "Presentation slide relationships could not be read");
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    reader->html.limit = (size_t)PPTX_MAX_HTML_BYTES + 1u;
    if (!pptx_html_literal(reader,
            "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
            "@page{size:landscape;margin:12mm}body{font-family:sans-serif;"
            "font-size:18pt;color:#151515;line-height:1.35}"
            ".slide{break-after:page;page-break-after:always;min-height:90vh;"
            "display:flex;flex-direction:column;justify-content:center;"
            "padding:1rem 2rem}header{border-bottom:1px solid #aaa;"
            "margin-bottom:1.2rem}h2{font-size:12pt;font-weight:400;color:#555}"
            "p{margin:.35em 0}.omitted{font-size:10pt;color:#666}"
            "</style></head><body>")) {
        if (report) (void)snprintf(report->note, sizeof(report->note),
            "PowerPoint Open XML presentation parse: Presentation HTML header exceeds output limits");
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    for (i = 0u; i < slide_count; ++i) {
        if (!pptx_render_slide_part(reader, slide_paths[i], i + 1u)) {
            if (report) (void)snprintf(report->note, sizeof(report->note),
                "PowerPoint Open XML presentation parse: %s",
                reader->error ? reader->error : "Could not render a presentation slide");
            rc = QW_STATUS_ERR_DECODE;
            goto cleanup;
        }
    }
    if (!pptx_html_literal(reader, "</body></html>")) {
        if (report) (void)snprintf(report->note, sizeof(report->note),
            "PowerPoint Open XML presentation parse: Presentation HTML exceeds output limits");
        rc = QW_STATUS_ERR_DECODE;
        goto cleanup;
    }
    rc = QW_STATUS_OK;

cleanup:
    if (slide_paths) {
        for (i = 0u; i < slide_count; ++i) free(slide_paths[i]);
        free(slide_paths);
    }
    if (archive_open) qw_zip_reader_end(&zip);
    if (rc != QW_STATUS_OK) {
        free(reader->html.data);
        memset(&reader->html, 0, sizeof(reader->html));
    }
    return rc;
}

static int pptx_open_layout(const void *data, size_t size,
                            QwInputFormat input_format,
                            const QwConvertOptions *options,
                            QwLayoutDocument *out_document,
                            QwConvertReport *report)
{
    PptxReader reader;
    int rc;
    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format < QW_FORMAT_PPTX || input_format > QW_FORMAT_PPSM)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = pptx_build_layout_html(data, size, &reader, report);
    if (rc != QW_STATUS_OK) return rc;
    out_document->html_utf8 = (const uint8_t *)reader.html.data;
    out_document->html_size = reader.html.size;
    out_document->context = reader.html.data;
    out_document->dispose = free;
    if (report) (void)snprintf(report->note, sizeof(report->note),
        "PowerPoint Open XML slide text opened as landscape layout HTML; source page size, masters, shape positions, fonts, images, notes, animation, charts and advanced styles are not preserved");
    return QW_STATUS_OK;
}

int qw_pptx_native_open_layout(const void *data, size_t size,
                               QwInputFormat input_format,
                               const QwConvertOptions *options,
                               QwLayoutDocument *out_document,
                               QwConvertReport *report)
{
    return pptx_open_layout(data, size, input_format, options, out_document,
                            report);
}

#define PPTX_DESCRIPTOR(symbol, format_id, label) \
    const QwFormatDescriptor symbol = { \
        .id = format_id, \
        .name = label, \
        .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE | \
                        QW_FORMAT_CAP_ARCHIVE_PROBE, \
        .probe = NULL, \
        .open_memory = NULL, \
        .open_layout = pptx_open_layout, \
        .open_audio = NULL, \
        .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT, \
        .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE, \
        .convert_to_pdf = NULL \
    }

PPTX_DESCRIPTOR(qw_format_pptx, QW_FORMAT_PPTX, "PPTX");
PPTX_DESCRIPTOR(qw_format_pptm, QW_FORMAT_PPTM, "PPTM");
PPTX_DESCRIPTOR(qw_format_potx, QW_FORMAT_POTX, "POTX");
PPTX_DESCRIPTOR(qw_format_potm, QW_FORMAT_POTM, "POTM");
PPTX_DESCRIPTOR(qw_format_ppsx, QW_FORMAT_PPSX, "PPSX");
PPTX_DESCRIPTOR(qw_format_ppsm, QW_FORMAT_PPSM, "PPSM");

#undef PPTX_DESCRIPTOR
