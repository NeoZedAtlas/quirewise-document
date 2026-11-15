/* XPS/OpenXPS layout adapter: FixedPage text -> HTML -> PDF.
 *
 * The package reader locates the fixed-document sequence, document and page
 * parts. This adapter flattens the page glyph runs into paragraph text so the
 * unified PDF route can render an XPS document; vector paths, images and
 * precise positioning are not reproduced.
 */

#include "format_registry.h"
#include "xps_package.h"

#include "../core/text_buffer.h"

#include <quirewise/image.h>
#include <quirewise/xml.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    XPS_LAYOUT_MAX_HTML_BYTES = 32u * 1024u * 1024u,
    XPS_LAYOUT_MAX_NODES = 500000u,
    XPS_LAYOUT_MAX_PAGES = 512u,
    XPS_LAYOUT_MAX_IMAGES = 64u,
    XPS_LAYOUT_MAX_IMAGE_BYTES = 32u * 1024u * 1024u
};

typedef struct XpsLayout {
    QwTextBuffer html;
    size_t page_count;
    size_t glyph_count;
    size_t image_count;
    size_t image_bytes;
} XpsLayout;

static void xps_layout_dispose(void *context)
{
    XpsLayout *layout = (XpsLayout *)context;
    if (!layout) return;
    free(layout->html.data);
    free(layout);
}

static int xps_layout_append_literal(XpsLayout *layout, const char *text)
{
    return qw_text_buffer_append(&layout->html, text, strlen(text));
}

static int xps_layout_append_escaped(XpsLayout *layout, const char *text)
{
    size_t index = 0u;
    size_t start = 0u;
    while (text[index]) {
        const char *entity = NULL;
        unsigned char ch = (unsigned char)text[index];
        if (ch == '&') entity = "&amp;";
        else if (ch == '<') entity = "&lt;";
        else if (ch == '>') entity = "&gt;";
        else if (ch == '"') entity = "&quot;";
        if (entity) {
            if (index > start &&
                !qw_text_buffer_append(&layout->html, text + start,
                                       index - start))
                return 0;
            if (!qw_text_buffer_append(&layout->html, entity, strlen(entity)))
                return 0;
            start = index + 1u;
        }
        ++index;
    }
    if (index > start)
        return qw_text_buffer_append(&layout->html, text + start,
                                     index - start);
    return 1;
}

/* Collect the UnicodeString of every Glyphs element on one page. */
static int xps_collect_glyphs(QwXmlNode *node, QwTextBuffer *text,
                              size_t *visited, size_t *glyph_count)
{
    for (; node; node = qw_xml_next(node)) {
        if (++*visited > XPS_LAYOUT_MAX_NODES) return 0;
        if (qw_xml_node_kind(node) == QW_XML_NODE_ELEMENT &&
            qw_xml_is_tag(node, "Glyphs")) {
            const char *value = qw_xml_att(node, "UnicodeString");
            if (value && value[0]) {
                if (!qw_text_buffer_append(text, value, strlen(value)) ||
                    !qw_text_buffer_append(text, " ", 1u))
                    return 0;
                ++*glyph_count;
            }
        }
        if (!xps_collect_glyphs(qw_xml_down(node), text, visited,
                                glyph_count))
            return 0;
    }
    return 1;
}

static int xps_layout_append_base64(XpsLayout *layout, const uint8_t *data,
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
            if (!qw_text_buffer_append(&layout->html, chunk, used)) return 0;
            used = 0u;
        }
    }
    if (used && !qw_text_buffer_append(&layout->html, chunk, used)) return 0;
    return 1;
}

/* Path.Fill ImageBrush references are embedded as PNG data URIs. Positions and
 * clipping are not reproduced: images are emitted before the page text. */
static int xps_emit_page_images(XpsLayout *layout, QwXpsPackage *package,
                                size_t page_index, QwXmlNode *node,
                                size_t *visited)
{
    for (; node; node = qw_xml_next(node)) {
        if (++*visited > XPS_LAYOUT_MAX_NODES) return 0;
        if (qw_xml_node_kind(node) == QW_XML_NODE_ELEMENT &&
            qw_xml_is_tag(node, "ImageBrush")) {
            const char *source = qw_xml_att(node, "ImageSource");
            if (source && source[0] &&
                layout->image_count < XPS_LAYOUT_MAX_IMAGES) {
                uint8_t *resource = NULL;
                size_t resource_size = 0u;
                if (qw_xps_package_read_page_resource(
                        package, page_index, source, &resource,
                        &resource_size) == QW_STATUS_OK &&
                    resource && resource_size &&
                    resource_size <= XPS_LAYOUT_MAX_IMAGE_BYTES -
                                        layout->image_bytes) {
                    QwImage image;
                    uint8_t *png = NULL;
                    size_t png_size = 0u;
                    memset(&image, 0, sizeof(image));
                    if (qw_image_decode(resource, resource_size, &image) ==
                            QW_IMAGE_OK &&
                        image.pixels &&
                        qw_image_encode(&image, QW_IMAGE_ENCODE_PNG, &png,
                                        &png_size) == QW_IMAGE_OK &&
                        png && png_size) {
                        unsigned width =
                            image.width > 480u ? 480u : image.width;
                        char width_text[16];
                        int width_length;
                        if (width < 8u) width = 8u;
                        width_length = snprintf(width_text,
                                                sizeof(width_text), "%u",
                                                width);
                        qw_image_free(&image);
                        if (width_length > 0 &&
                            (size_t)width_length < sizeof(width_text) &&
                            xps_layout_append_literal(
                                layout, "<figure><img alt=\"xps image\" "
                                        "width=\"") &&
                            qw_text_buffer_append(&layout->html, width_text,
                                                  (size_t)width_length) &&
                            xps_layout_append_literal(
                                layout, "\" src=\"data:image/png;base64,") &&
                            xps_layout_append_base64(layout, png,
                                                     png_size) &&
                            xps_layout_append_literal(
                                layout, "\"/></figure>")) {
                            layout->image_count += 1u;
                            layout->image_bytes += resource_size;
                        }
                        free(png);
                    } else {
                        qw_image_free(&image);
                    }
                    free(resource);
                }
            }
        }
        if (!xps_emit_page_images(layout, package, page_index,
                                  qw_xml_down(node), visited))
            return 0;
    }
    return 1;
}
static int xps_build_page_html(XpsLayout *layout, QwXpsPackage *package,
                               size_t page_index, const uint8_t *page_xml,
                               size_t page_xml_size, size_t *visited)
{
    QwXmlError error;
    QwXmlNode *xml;
    memset(&error, 0, sizeof(error));
    xml = qw_xml_parse(page_xml, page_xml_size, &error);
    QwTextBuffer text;
    char heading[64];

    memset(&text, 0, sizeof(text));
    text.limit = 8u * 1024u * 1024u;
    if (!xml) return 1; /* unparsable page: keep going, text is best effort */
    if (!xps_emit_page_images(layout, package, page_index, qw_xml_root(xml),
                              visited) ||
        !xps_collect_glyphs(qw_xml_root(xml), &text, visited,
                            &layout->glyph_count)) {
        qw_xml_drop(xml);
        free(text.data);
        return 0;
    }
    qw_xml_drop(xml);
    if (!text.size) {
        free(text.data);
        return 1;
    }
    (void)snprintf(heading, sizeof(heading), "<h2>Page %zu</h2>",
                   page_index + 1u);
    if (!xps_layout_append_literal(layout, "<section class=\"xps-page\">") ||
        !xps_layout_append_literal(layout, heading) ||
        !xps_layout_append_literal(layout, "<p>") ||
        !xps_layout_append_escaped(layout, text.data) ||
        !xps_layout_append_literal(layout, "</p></section>")) {
        free(text.data);
        return 0;
    }
    free(text.data);
    return 1;
}

int qw_xps_native_open_layout(const void *data, size_t size,
                              QwInputFormat input_format,
                              const QwConvertOptions *options,
                              QwLayoutDocument *out_document,
                              QwConvertReport *report)
{
    QwXpsPackage *package = NULL;
    XpsLayout *layout;
    size_t page_count;
    size_t page_index;
    size_t visited = 0u;
    int rc;
    (void)options;

    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_XPS || !data || !size)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    rc = qw_xps_package_open(&package, data, size, report);
    if (rc != QW_STATUS_OK || !package) return rc != QW_STATUS_OK
        ? rc : QW_STATUS_ERR_FORMAT;
    layout = (XpsLayout *)calloc(1u, sizeof(*layout));
    if (!layout) {
        qw_xps_package_close(package);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    layout->html.limit = XPS_LAYOUT_MAX_HTML_BYTES;
    if (!xps_layout_append_literal(layout,
            "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
            "@page{size:A4;margin:18mm}body{font-family:sans-serif;"
            "font-size:11pt;line-height:1.3;color:#111}"
            "h2{font-size:9pt;color:#666;font-weight:400}"
            ".xps-page{page-break-after:always}"
            "</style></head><body>")) {
        xps_layout_dispose(layout);
        qw_xps_package_close(package);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    page_count = qw_xps_package_page_count(package);
    if (page_count > XPS_LAYOUT_MAX_PAGES) page_count = XPS_LAYOUT_MAX_PAGES;
    for (page_index = 0u; page_index < page_count; ++page_index) {
        uint8_t *page_xml = NULL;
        size_t page_xml_size = 0u;
        double width = 0.0;
        double height = 0.0;
        if (qw_xps_package_read_page(package, page_index, &page_xml,
                                     &page_xml_size, &width, &height,
                                     report) != QW_STATUS_OK)
            continue;
        (void)width;
        (void)height;
        if (!xps_build_page_html(layout, package, page_index, page_xml,
                                 page_xml_size, &visited)) {
            free(page_xml);
            xps_layout_dispose(layout);
            qw_xps_package_close(package);
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        }
        free(page_xml);
        ++layout->page_count;
        if (visited > XPS_LAYOUT_MAX_NODES) break;
    }
    if (!xps_layout_append_literal(layout, "</body></html>")) {
        xps_layout_dispose(layout);
        qw_xps_package_close(package);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    qw_xps_package_close(package);
    out_document->html_utf8 = (const uint8_t *)layout->html.data;
    out_document->html_size = layout->html.size;
    out_document->context = layout;
    out_document->dispose = xps_layout_dispose;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "XPS layout: %zu page(s), %zu glyph runs rendered as text; "
            "images, vector paths and exact positioning are not preserved",
            layout->page_count, layout->glyph_count);
    }
    return QW_STATUS_OK;
}

static int xps_probe(const void *data, size_t size)
{
    QwXpsPackage *package = NULL;
    int rc;
    if (!data || size < 4u ||
        memcmp(data, "PK\x03\x04", 4u) != 0)
        return 0;
    rc = qw_xps_package_open(&package, data, size, NULL);
    if (rc != QW_STATUS_OK) return 0;
    qw_xps_package_close(package);
    return 1;
}

const QwFormatDescriptor qw_format_xps = {
    .id = QW_FORMAT_XPS,
    .name = "XPS/OpenXPS fixed document",
    .capabilities = QW_FORMAT_CAP_CONTAINER | QW_FORMAT_CAP_MULTI_PAGE |
                    QW_FORMAT_CAP_ARCHIVE_PROBE,
    .probe = xps_probe,
    .open_memory = NULL,
    .open_layout = qw_xps_native_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DOCUMENT_ARCHIVE,
    .convert_to_pdf = NULL
};
