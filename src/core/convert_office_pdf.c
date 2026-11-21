/* Layout-preserving PDF -> DOCX/EPUB reconstruction.
 *
 * The PDF pages are rasterized with the package's own renderer at 120 dpi,
 * encoded as baseline JPEG (the image package already writes it) and embedded
 * one per page. DOCX gets a WordprocessingML drawing per page plus the image
 * relationships; EPUB gets one XHTML page with every page image. This keeps
 * the visual layout (text, rules, figures) instead of rebuilding only the text
 * layer; the output is therefore not editable text. Inputs larger than
 * OFFICE_PDF_MAX_PAGES are truncated with a report note.
 */

#include "quirewise/convert.h"
#include "quirewise/image.h"
#include "quirewise/pdf.h"

#include "../backends/document_backend.h"
#include "../formats/archive/zip_native.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OFFICE_PDF_MAX_PAGES 24u
#define OFFICE_PDF_DPI 120u
#define OFFICE_PDF_XML_LIMIT ((size_t)16u * 1024u * 1024u)
#define OFFICE_PDF_EMU_PER_INCH 914400u

typedef struct OfficePdfBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} OfficePdfBuffer;

typedef struct OfficePdfPage {
    uint8_t *jpeg;
    size_t jpeg_size;
    uint32_t width;
    uint32_t height;
} OfficePdfPage;

static int pdf_buffer_reserve(OfficePdfBuffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *grown;

    if (buffer->size > OFFICE_PDF_XML_LIMIT ||
        additional > OFFICE_PDF_XML_LIMIT - buffer->size)
        return 0;
    needed = buffer->size + additional;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 4096u;
    while (capacity < needed) {
        if (capacity > OFFICE_PDF_XML_LIMIT / 2u) {
            capacity = OFFICE_PDF_XML_LIMIT;
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

static int pdf_buffer_append(OfficePdfBuffer *buffer, const void *data,
                             size_t size)
{
    if (!size) return 1;
    if (!pdf_buffer_reserve(buffer, size)) return 0;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 1;
}

static int pdf_buffer_text(OfficePdfBuffer *buffer, const char *text)
{
    return text && text[0] ? pdf_buffer_append(buffer, text, strlen(text)) : 1;
}

static int pdf_buffer_escape(OfficePdfBuffer *buffer, const uint8_t *text,
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
        if (!pdf_buffer_append(buffer, text + start, index - start) ||
            !pdf_buffer_text(buffer, replacement))
            return 0;
        start = index + 1u;
    }
    return pdf_buffer_append(buffer, text + start, size - start);
}

static int pdf_buffer_printf(OfficePdfBuffer *buffer, const char *format, ...)
{
    char stack[512];
    va_list arguments;
    int written;

    va_start(arguments, format);
    written = vsnprintf(stack, sizeof(stack), format, arguments);
    va_end(arguments);
    if (written < 0) return 0;
    if ((size_t)written < sizeof(stack))
        return pdf_buffer_append(buffer, stack, (size_t)written);
    {
        char *heap = (char *)malloc((size_t)written + 1u);
        int copied;
        if (!heap) return 0;
        va_start(arguments, format);
        copied = vsnprintf(heap, (size_t)written + 1u, format, arguments);
        va_end(arguments);
        if (copied == written) {
            int ok = pdf_buffer_append(buffer, heap, (size_t)written);
            free(heap);
            return ok;
        }
        free(heap);
    }
    return 0;
}

static void office_pdf_pages_free(OfficePdfPage *pages, size_t count)
{
    size_t index;

    if (!pages) return;
    for (index = 0u; index < count; ++index) free(pages[index].jpeg);
    free(pages);
}

/* Render and JPEG-encode every page. The caller owns the returned array. */
static int office_pdf_render(const uint8_t *data, size_t size,
                             const char *password, OfficePdfPage **out_pages,
                             size_t *out_count, QwConvertReport *report)
{
    QwPdfDocument *document = NULL;
    size_t page_count = 0u;
    size_t index;
    OfficePdfPage *pages;
    int status;

    *out_pages = NULL;
    *out_count = 0u;
    status = password
                 ? qw_pdf_open_with_password(data, size, password, &document,
                                             &page_count, report)
                 : qw_pdf_open(data, size, &document, &page_count, report);
    if (status != QW_STATUS_OK) return status;
    if (!page_count) {
        qw_pdf_close(document);
        return QW_STATUS_ERR_FORMAT;
    }
    if (page_count > OFFICE_PDF_MAX_PAGES) {
        page_count = OFFICE_PDF_MAX_PAGES;
        if (report && !report->note[0])
            snprintf(report->note, sizeof(report->note),
                     "only the first %u PDF pages are embedded",
                     (unsigned)OFFICE_PDF_MAX_PAGES);
    }
    pages = (OfficePdfPage *)calloc(page_count, sizeof(*pages));
    if (!pages) {
        qw_pdf_close(document);
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    }
    for (index = 0u; index < page_count; ++index) {
        QwPageImage page;
        QwImage image;
        QwImage decoded;
        const QwImage *source = &image;
        uint8_t *jpeg = NULL;
        size_t jpeg_size = 0u;
        int decoded_ready = 0;

        memset(&page, 0, sizeof(page));
        memset(&decoded, 0, sizeof(decoded));
        status = qw_pdf_render_page(document, index, OFFICE_PDF_DPI, &page,
                                    report);
        if (status != QW_STATUS_OK) goto failure;
        if (page.kind == QW_PAGE_IMAGE_RGB24 && page.data) {
            memset(&image, 0, sizeof(image));
            image.pixels = (uint8_t *)(uintptr_t)page.data;
            image.size = page.size;
            image.stride = (size_t)page.width * 3u;
            image.width = page.width;
            image.height = page.height;
            image.format = QW_IMAGE_PIXEL_RGB24;
        } else if (page.data && page.size &&
                   qw_image_decode(page.data, page.size, &decoded) ==
                       QW_IMAGE_OK) {
            source = &decoded;
            decoded_ready = 1;
        } else {
            qw_pdf_page_free(&page);
            status = QW_STATUS_ERR_FORMAT;
            goto failure;
        }
        if (qw_image_encode(source, QW_IMAGE_ENCODE_JPEG, &jpeg,
                            &jpeg_size) != QW_IMAGE_OK ||
            !jpeg || !jpeg_size) {
            free(jpeg);
            if (decoded_ready) qw_image_free(&decoded);
            qw_pdf_page_free(&page);
            status = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto failure;
        }
        pages[index].jpeg = jpeg;
        pages[index].jpeg_size = jpeg_size;
        pages[index].width = source->width;
        pages[index].height = source->height;
        if (decoded_ready) qw_image_free(&decoded);
        qw_pdf_page_free(&page);
    }
    qw_pdf_close(document);
    *out_pages = pages;
    *out_count = page_count;
    return QW_STATUS_OK;

failure:
    qw_pdf_close(document);
    office_pdf_pages_free(pages, page_count);
    return status;
}

static uint32_t office_pdf_emu(uint32_t pixels)
{
    return (uint32_t)(((uint64_t)pixels * OFFICE_PDF_EMU_PER_INCH) /
                      OFFICE_PDF_DPI);
}

static int office_zip_add(QwZipWriter *writer, const char *name,
                          const void *data, size_t size)
{
    return qw_zip_writer_add_mem(writer, name, data, size,
                                 QW_ZIP_NO_COMPRESSION);
}

static int office_zip_finish(QwZipWriter *writer, uint8_t **out,
                             size_t *out_size)
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
    return 1;
}

int qw_doc_office_docx_from_pdf(const uint8_t *data, size_t size,
                                const char *password, uint8_t **out_data,
                                size_t *out_size, QwConvertReport *report)
{
    static const char content_types_prefix[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/"
        "content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/"
        "vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"png\" ContentType=\"image/png\"/>"
        "<Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/"
        "vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"
        "\"/></Types>";
    static const char package_rels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
        "2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://"
        "schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "officeDocument\" Target=\"word/document.xml\"/></Relationships>";
    static const char document_prefix[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
        "wordprocessingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats."
        "org/officeDocument/2006/relationships\" xmlns:wp=\"http://schemas."
        "openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\""
        "><w:body>";
    OfficePdfPage *pages = NULL;
    size_t page_count = 0u;
    size_t index;
    OfficePdfBuffer document;
    OfficePdfBuffer relationships;
    QwZipWriter writer;
    uint32_t last_width = 0u;
    uint32_t last_height = 0u;
    int status;
    int zip_open = 0;

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    status = office_pdf_render(data, size, password, &pages, &page_count,
                               report);
    if (status != QW_STATUS_OK) return status;
    memset(&document, 0, sizeof(document));
    memset(&relationships, 0, sizeof(relationships));
    if (!pdf_buffer_text(&document, document_prefix)) goto out_of_memory;
    for (index = 0u; index < page_count; ++index) {
        uint32_t cx = office_pdf_emu(pages[index].width);
        uint32_t cy = office_pdf_emu(pages[index].height);
        unsigned page_number = (unsigned)(index + 1u);

        last_width = pages[index].width;
        last_height = pages[index].height;
        if (!pdf_buffer_printf(
                &document,
                "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r>"
                "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" "
                "distR=\"0\"><wp:extent cx=\"%u\" cy=\"%u\"/><wp:docPr "
                "id=\"%u\" name=\"Page %u\"/><a:graphic><a:graphicData "
                "uri=\"http://schemas.openxmlformats.org/drawingml/2006/"
                "picture\"><pic:pic><pic:nvPicPr><pic:cNvPr id=\"%u\" "
                "name=\"page%u.jpg\"/><pic:cNvPicPr/></pic:nvPicPr>"
                "<pic:blipFill><a:blip r:embed=\"rId%u\"/><a:stretch>"
                "<a:fillRect/></a:stretch></pic:blipFill><pic:spPr>"
                "<a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%u\" cy=\"%u\"/>"
                "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>"
                "</pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline>"
                "</w:drawing></w:r></w:p>",
                cx, cy, page_number, page_number, page_number, page_number,
                page_number, cx, cy))
            goto out_of_memory;
        if (!pdf_buffer_printf(
                &relationships,
                "<Relationship Id=\"rId%u\" Type=\"http://schemas."
                "openxmlformats.org/officeDocument/2006/relationships/image\" "
                "Target=\"media/page%u.jpg\"/>",
                page_number, page_number))
            goto out_of_memory;
    }
    if (page_count) {
        uint32_t width_twips = (uint32_t)(((uint64_t)last_width * 1440u) /
                                          OFFICE_PDF_DPI);
        uint32_t height_twips = (uint32_t)(((uint64_t)last_height * 1440u) /
                                           OFFICE_PDF_DPI);
        if (!pdf_buffer_printf(
                &document,
                "<w:sectPr><w:pgSz w:w=\"%u\" w:h=\"%u\"/><w:pgMar "
                "w:top=\"0\" w:right=\"0\" w:bottom=\"0\" w:left=\"0\"/>"
                "</w:sectPr>",
                width_twips, height_twips))
            goto out_of_memory;
    }
    if (!pdf_buffer_text(&document, "</w:body></w:document>")) goto out_of_memory;
    if (!qw_zip_writer_init_heap(&writer, 0u, 0u)) goto out_of_memory;
    zip_open = 1;
    if (!office_zip_add(&writer, "[Content_Types].xml", content_types_prefix,
                        strlen(content_types_prefix)) ||
        !office_zip_add(&writer, "_rels/.rels", package_rels,
                        strlen(package_rels)) ||
        !office_zip_add(&writer, "word/document.xml", document.data,
                        document.size) ||
        !office_zip_add(&writer, "word/_rels/document.xml.rels",
                        relationships.data, relationships.size))
        goto out_of_memory;
    for (index = 0u; index < page_count; ++index) {
        char name[40];
        snprintf(name, sizeof(name), "word/media/page%u.jpg",
                 (unsigned)(index + 1u));
        if (!office_zip_add(&writer, name, pages[index].jpeg,
                            pages[index].jpeg_size))
            goto out_of_memory;
    }
    if (!office_zip_finish(&writer, out_data, out_size)) goto out_of_memory;
    zip_open = 0;
    free(document.data);
    free(relationships.data);
    office_pdf_pages_free(pages, page_count);
    if (report) {
        report->format = QW_FORMAT_DOCX;
        report->output_format = QW_FORMAT_DOCX;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "layout-preserving DOCX: %u page image(s) at %u dpi (visual "
                 "layout only, not editable text)",
                 (unsigned)page_count, (unsigned)OFFICE_PDF_DPI);
    }
    return QW_STATUS_OK;

out_of_memory:
    if (zip_open) qw_zip_writer_end(&writer);
    free(document.data);
    free(relationships.data);
    office_pdf_pages_free(pages, page_count);
    return QW_STATUS_ERR_OUT_OF_MEMORY;
}

int qw_doc_office_epub_from_pdf(const uint8_t *data, size_t size,
                                const char *password, uint8_t **out_data,
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
        "dc/elements/1.1/\"><dc:title>Quirewise PDF pages</dc:title>"
        "<dc:identifier id=\"bookid\">urn:uuid:quirewise-pdf-pages</dc:"
        "identifier><dc:language>und</dc:language></metadata><manifest>"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/"
        "x-dtbncx+xml\"/><item id=\"chapter\" href=\"chapter1.xhtml\" "
        "media-type=\"application/xhtml+xml\"/><item id=\"text\" "
        "href=\"chapter2.xhtml\" media-type=\"application/xhtml+xml\"/>";
    static const char toc_ncx[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\""
        "><head><meta name=\"dtb:uid\" content=\"urn:uuid:quirewise-pdf-pages\""
        "/><meta name=\"dtb:depth\" content=\"1\"/></head><docTitle>"
        "<text>Quirewise PDF pages</text></docTitle><navMap><navPoint "
        "id=\"chapter\" playOrder=\"1\"><navLabel><text>Pages</text></navLabel>"
        "<content src=\"chapter1.xhtml\"/></navPoint></navMap></ncx>";
    static const char chapter_prefix[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE html><html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
        "<title>Quirewise PDF pages</title><style>body{margin:0;padding:0}"
        "div.page{margin:0;padding:0}img{display:block;width:100%;height:auto}"
        "</style></head><body>";
    OfficePdfPage *pages = NULL;
    size_t page_count = 0u;
    size_t index;
    OfficePdfBuffer opf;
    OfficePdfBuffer chapter;
    OfficePdfBuffer text_chapter;
    uint8_t *text_bytes = NULL;
    size_t text_size = 0u;
    QwZipWriter writer;
    char entry[64];
    int status;
    int zip_open = 0;

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    status = office_pdf_render(data, size, password, &pages, &page_count,
                               report);
    if (status != QW_STATUS_OK) return status;
    memset(&opf, 0, sizeof(opf));
    memset(&chapter, 0, sizeof(chapter));
    memset(&text_chapter, 0, sizeof(text_chapter));
    {
        QwPdfDocument *text_document = NULL;
        size_t text_pages = 0u;

        if ((password ? qw_pdf_open_with_password(data, size, password,
                                                   &text_document, &text_pages,
                                                   NULL)
                      : qw_pdf_open(data, size, &text_document, &text_pages,
                                    NULL)) == QW_STATUS_OK &&
            text_document) {
            (void)qw_pdf_extract_text(text_document, &text_bytes, &text_size, NULL);
            qw_pdf_close(text_document);
        }
    }
    if (!pdf_buffer_text(&opf, content_opf_prefix)) goto out_of_memory;
    if (!pdf_buffer_text(&chapter, chapter_prefix)) goto out_of_memory;
    if (!pdf_buffer_text(&text_chapter,
                         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<!DOCTYPE html><html xmlns=\"http://www.w3.org/1999/"
                         "xhtml\"><head><title>Extracted PDF text</title>"
                         "</head><body>"))
        goto out_of_memory;
    {
        size_t text_index = 0u;
        size_t paragraph_count = 0u;

        while (text_index < text_size) {
            size_t line_start = text_index;
            size_t line_end;

            while (text_index < text_size &&
                   text_bytes[text_index] != '\n' &&
                   text_bytes[text_index] != '\r')
                ++text_index;
            line_end = text_index;
            while (text_index < text_size &&
                   (text_bytes[text_index] == '\n' ||
                    text_bytes[text_index] == '\r'))
                ++text_index;
            if (line_end == line_start) continue;
            if (!pdf_buffer_text(&text_chapter, "<p>") ||
                !pdf_buffer_escape(&text_chapter, text_bytes + line_start,
                                   line_end - line_start) ||
                !pdf_buffer_text(&text_chapter, "</p>"))
                goto out_of_memory;
            ++paragraph_count;
        }
        if (!paragraph_count &&
            !pdf_buffer_text(&text_chapter,
                             "<p>(No extractable text in this PDF.)</p>"))
            goto out_of_memory;
    }
    if (!pdf_buffer_text(&text_chapter, "</body></html>")) goto out_of_memory;
    for (index = 0u; index < page_count; ++index) {
        unsigned page_number = (unsigned)(index + 1u);
        if (!pdf_buffer_printf(
                &opf,
                "<item id=\"page%u\" href=\"page%u.jpg\" "
                "media-type=\"image/jpeg\"/>",
                page_number, page_number))
            goto out_of_memory;
        if (!pdf_buffer_printf(
                &chapter,
                "<div class=\"page\"><img src=\"page%u.jpg\" alt=\"Page %u\"/>"
                "</div>",
                page_number, page_number))
            goto out_of_memory;
    }
    if (!pdf_buffer_text(&opf, "</manifest><spine toc=\"ncx\">"
                               "<itemref idref=\"chapter\"/>"
                               "<itemref idref=\"text\"/></spine>"
                               "</package>") ||
        !pdf_buffer_text(&chapter, "</body></html>"))
        goto out_of_memory;
    if (!qw_zip_writer_init_heap(&writer, 0u, 0u)) goto out_of_memory;
    zip_open = 1;
    if (!office_zip_add(&writer, "mimetype", "application/epub+zip", 20u) ||
        !office_zip_add(&writer, "META-INF/container.xml", container_xml,
                        strlen(container_xml)) ||
        !office_zip_add(&writer, "OEBPS/content.opf", opf.data, opf.size) ||
        !office_zip_add(&writer, "OEBPS/toc.ncx", toc_ncx, strlen(toc_ncx)) ||
        !office_zip_add(&writer, "OEBPS/chapter1.xhtml", chapter.data,
                        chapter.size) ||
        !office_zip_add(&writer, "OEBPS/chapter2.xhtml", text_chapter.data,
                        text_chapter.size))
        goto out_of_memory;
    for (index = 0u; index < page_count; ++index) {
        snprintf(entry, sizeof(entry), "OEBPS/page%u.jpg",
                 (unsigned)(index + 1u));
        if (!office_zip_add(&writer, entry, pages[index].jpeg,
                            pages[index].jpeg_size))
            goto out_of_memory;
    }
    if (!office_zip_finish(&writer, out_data, out_size)) goto out_of_memory;
    zip_open = 0;
    free(opf.data);
    free(chapter.data);
    free(text_chapter.data);
    qw_pdf_free(text_bytes);
    office_pdf_pages_free(pages, page_count);
    if (report) {
        report->format = QW_FORMAT_EPUB;
        report->output_format = QW_FORMAT_EPUB;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "layout-preserving EPUB: %u page image(s) at %u dpi (visual "
                 "layout only, not editable text)",
                 (unsigned)page_count, (unsigned)OFFICE_PDF_DPI);
    }
    return QW_STATUS_OK;

out_of_memory:
    if (zip_open) qw_zip_writer_end(&writer);
    free(opf.data);
    free(chapter.data);
    free(text_chapter.data);
    qw_pdf_free(text_bytes);
    office_pdf_pages_free(pages, page_count);
    return QW_STATUS_ERR_OUT_OF_MEMORY;
}


/* Editable-text PDF -> DOCX. The package keeps the same shape as the
 * layout-preserving writer, but the body holds one paragraph per extracted
 * line and the package has no media parts. Layout is not reconstructed; the
 * text layer is what survives. */

static int office_pdf_is_bold(const char *font)
{
    return font && (strstr(font, "Bold") || strstr(font, "bold"));
}

static int office_pdf_is_italic(const char *font)
{
    return font && (strstr(font, "Italic") || strstr(font, "italic") ||
                    strstr(font, "Oblique") || strstr(font, "oblique"));
}

/* WordprocessingML run size is half-points; return "" when the size is unknown. */
static const char *office_pdf_size_tag(double size)
{
    static char tag[48];
    int half_points;
    if (size < 1.0 || size > 200.0) return "";
    half_points = (int)(size * 2.0 + 0.5);
    snprintf(tag, sizeof(tag), "<w:sz w:val=\"%d\"/>", half_points);
    return tag;
}

/* Count image XObjects so the text-only note can say they were skipped. */
static size_t office_pdf_image_count(const uint8_t *data, size_t size)
{
    static const char needle[] = "/Subtype /Image";
    size_t count = 0u;
    size_t position;
    if (!data || size < sizeof(needle) - 1u) return 0u;
    for (position = 0u; position + sizeof(needle) - 1u <= size; ++position) {
        if (memcmp(data + position, needle, sizeof(needle) - 1u) == 0) {
            ++count;
            position += sizeof(needle) - 2u;
        }
    }
    return count;
}

/* Two lines share a table layout when their runs start at the same x. */
#define OFFICE_PDF_MAX_COLUMNS 16u

typedef struct OfficePdfTable {
    double columns[OFFICE_PDF_MAX_COLUMNS];
    size_t column_count;
} OfficePdfTable;

static size_t office_pdf_column_index(const OfficePdfTable *table, double x)
{
    size_t index;
    for (index = 0u; index < table->column_count; ++index) {
        double delta = table->columns[index] - x;
        if (delta < 0.0) delta = -delta;
        if (delta <= 2.0) return index;
    }
    return SIZE_MAX;
}

/* Collect the column origins of consecutive lines that look like table rows. */
static size_t office_pdf_table_block(const QwPdfLine *lines, size_t line_count,
                                     size_t start, OfficePdfTable *table)
{
    size_t end = start;
    memset(table, 0, sizeof(*table));
    while (end < line_count && lines[end].run_count >= 2u) {
        size_t run_index;
        int fits = 1;
        for (run_index = 0u; run_index < lines[end].run_count; ++run_index) {
            if (office_pdf_column_index(table, lines[end].runs[run_index].x) ==
                SIZE_MAX) {
                if (table->column_count >= OFFICE_PDF_MAX_COLUMNS) {
                    fits = 0;
                    break;
                }
                table->columns[table->column_count++] =
                    lines[end].runs[run_index].x;
            }
        }
        if (!fits) break;
        ++end;
    }
    return end;
}

/* Number of grid columns covered by the run starting at `column`. */
static size_t office_pdf_cell_span(const OfficePdfTable *table, size_t column,
                                   size_t next_column)
{
    size_t span;
    if (column == SIZE_MAX) return 1u;
    if (next_column == SIZE_MAX || next_column <= column)
        next_column = table->column_count;
    span = next_column - column;
    if (column + span > table->column_count)
        span = table->column_count - column;
    return span ? span : 1u;
}

/* Encode one extracted image as PNG media and emit the DOCX drawing. */
static int office_docx_emit_image(OfficePdfBuffer *body,
                                  OfficePdfBuffer *relationships,
                                  const QwPdfExtractedImage *image,
                                  uint8_t **media_data, size_t *media_size,
                                  size_t *media_count)
{
    QwImage encoded;
    uint8_t *png = NULL;
    size_t png_size = 0u;
    unsigned emu_w;
    unsigned emu_h;
    if (*media_count >= 64u) return 1;
    memset(&encoded, 0, sizeof(encoded));
    encoded.pixels = image->rgb;
    encoded.stride = image->width * 3u;
    encoded.size = encoded.stride * image->height;
    encoded.width = (uint32_t)image->width;
    encoded.height = (uint32_t)image->height;
    encoded.format = QW_IMAGE_PIXEL_RGB24;
    if (qw_image_encode(&encoded, QW_IMAGE_ENCODE_PNG, &png, &png_size) !=
        QW_IMAGE_OK)
        return 1;
    media_data[*media_count] = png;
    media_size[*media_count] = png_size;
    ++*media_count;
    emu_w = (unsigned)(image->w * 12700.0);
    emu_h = (unsigned)(image->h * 12700.0);
    if (emu_w == 0u) emu_w = 12700u;
    if (emu_h == 0u) emu_h = 12700u;
    if (!pdf_buffer_printf(
            body,
            "<w:p><w:r><w:drawing><wp:inline distT=\"0\" "
            "distB=\"0\" distL=\"0\" distR=\"0\">"
            "<wp:extent cx=\"%u\" cy=\"%u\"/><wp:docPr "
            "id=\"%zu\" name=\"Image %zu\"/><a:graphic>"
            "<a:graphicData uri=\"http://schemas.openxmlformats"
            ".org/drawingml/2006/picture\"><pic:pic>"
            "<pic:nvPicPr><pic:cNvPr id=\"%zu\" "
            "name=\"image%zu.png\"/><pic:cNvPicPr/>"
            "</pic:nvPicPr><pic:blipFill><a:blip "
            "r:embed=\"rId%zu\"/><a:stretch><a:fillRect/>"
            "</a:stretch></pic:blipFill><pic:spPr><a:xfrm>"
            "<a:off x=\"0\" y=\"0\"/><a:ext cx=\"%u\" "
            "cy=\"%u\"/></a:xfrm><a:prstGeom prst=\"rect\">"
            "<a:avLst/></a:prstGeom></pic:spPr></pic:pic>"
            "</a:graphicData></a:graphic></wp:inline>"
            "</w:drawing></w:r></w:p>",
            emu_w, emu_h, *media_count, *media_count, *media_count,
            *media_count, *media_count, emu_w, emu_h) ||
        !pdf_buffer_printf(
            relationships,
            "<Relationship Id=\"rId%zu\" Type=\"http://schemas"
            ".openxmlformats.org/officeDocument/2006/"
            "relationships/image\" Target=\"media/image%zu"
            ".png\"/>",
            *media_count, *media_count))
        return 0;
    return 1;
}

/* Detect a two-column page: line origins cluster left and right of a split. */
static int office_pdf_column_split(const QwPdfLine *lines, size_t line_count,
                                   double *split)
{
    double min_x;
    double max_x;
    size_t left = 0u;
    size_t right = 0u;
    size_t index;
    if (line_count < 4u) return 0;
    min_x = lines[0].x;
    max_x = lines[0].x;
    for (index = 1u; index < line_count; ++index) {
        if (lines[index].x < min_x) min_x = lines[index].x;
        if (lines[index].x > max_x) max_x = lines[index].x;
    }
    if (max_x - min_x < 120.0) return 0;
    *split = (min_x + max_x) / 2.0;
    for (index = 0u; index < line_count; ++index) {
        if (lines[index].x < *split) ++left;
        else ++right;
    }
    if (left < 2u || right < 2u) return 0;
    return 1;
}

/* Stable insertion sort: left column first, top-to-bottom, then right. */
static void office_pdf_sort_columns(QwPdfLine *lines, size_t line_count,
                                    double split)
{
    size_t index;
    for (index = 1u; index < line_count; ++index) {
        QwPdfLine current = lines[index];
        int key = current.x < split ? 0 : 1;
        size_t position = index;
        while (position > 0u) {
            const QwPdfLine *previous = &lines[position - 1u];
            int previous_key = previous->x < split ? 0 : 1;
            if (previous_key < key ||
                (previous_key == key && previous->y >= current.y))
                break;
            lines[position] = *previous;
            --position;
        }
        lines[position] = current;
    }
}

/* Map a point from unrotated PDF space into the displayed orientation. */
static void office_pdf_rotate_point(int rotation, double page_width,
                                    double page_height, double *x, double *y)
{
    double px = *x;
    double py = *y;
    switch (rotation) {
    case 90:
        *x = py;
        *y = page_width - px;
        break;
    case 180:
        *x = page_width - px;
        *y = page_height - py;
        break;
    case 270:
        *x = page_height - py;
        *y = px;
        break;
    default:
        break;
    }
}

/* WordprocessingML run font, sanitised; empty when the PDF name is unusable. */
static const char *office_pdf_font_tag(const char *font)
{
    static char tag[192];
    char name[64];
    size_t length = 0u;
    if (!font || !font[0]) return "";
    if (strlen(font) > 7u && font[6] == '+') font += 7;
    while (font[length] && length + 1u < sizeof(name)) {
        char value = font[length];
        name[length] = (isalnum((unsigned char)value) || value == '-' ||
                        value == '_' || value == ' ')
                           ? value
                           : '_';
        ++length;
    }
    name[length] = '\0';
    if (!length) return "";
    snprintf(tag, sizeof(tag),
             "<w:rFonts w:ascii=\"%s\" w:hAnsi=\"%s\"/>", name, name);
    return tag;
}

/* CSS font-family declaration for EPUB spans, or "". */
static const char *office_pdf_font_css(const char *font)
{
    static char css[128];
    char name[64];
    size_t length = 0u;
    if (!font || !font[0]) return "";
    if (strlen(font) > 7u && font[6] == '+') font += 7;
    while (font[length] && length + 1u < sizeof(name)) {
        char value = font[length];
        name[length] = (isalnum((unsigned char)value) || value == '-' ||
                        value == '_' || value == ' ')
                           ? value
                           : '_';
        ++length;
    }
    name[length] = '\0';
    if (!length) return "";
    snprintf(css, sizeof(css), "font-family:%s;", name);
    return css;
}

/* True when the text ends a sentence and must not join the next line. */
static int office_pdf_ends_sentence(const char *text)
{
    size_t length;
    const unsigned char *bytes = (const unsigned char *)text;
    if (!text) return 1;
    length = strlen(text);
    while (length > 0u && bytes[length - 1u] == ' ') --length;
    if (length == 0u) return 1;
    if (bytes[length - 1u] == '.' || bytes[length - 1u] == '!' ||
        bytes[length - 1u] == '?')
        return 1;
    if (length >= 3u) {
        if (memcmp(bytes + length - 3u, "\xE3\x80\x82", 3u) == 0 ||
            memcmp(bytes + length - 3u, "\xEF\xBC\x81", 3u) == 0 ||
            memcmp(bytes + length - 3u, "\xEF\xBC\x9F", 3u) == 0)
            return 1;
    }
    return 0;
}

/* Lines of one wrapped paragraph share the margin, leading and size. */
static int office_pdf_mergeable(const QwPdfLine *previous, const QwPdfLine *next)
{
    double gap;
    double size;
    if (!previous->run_count || !next->run_count) return 0;
    gap = previous->y - next->y;
    size = previous->runs[0].size;
    if (gap <= 0.0 || gap > size * 2.0 + 2.0) return 0;
    if (next->x < previous->x - 6.0 || next->x > previous->x + 6.0) return 0;
    if (next->runs[0].size - size > 1.5 || size - next->runs[0].size > 1.5)
        return 0;
    return !office_pdf_ends_sentence(
        previous->runs[previous->run_count - 1u].text);
}

/* True when an image sits vertically between two text lines. */
static int office_pdf_image_between(const QwPdfExtractedImage *images,
                                    size_t count, double top, double bottom)
{
    size_t index;
    for (index = 0u; index < count; ++index) {
        if (images[index].y > bottom && images[index].y < top) return 1;
    }
    return 0;
}

/* Join with a space only when both sides are ASCII. */
static int office_pdf_join_space(const QwPdfLine *previous,
                                 const QwPdfLine *next)
{
    const char *a;
    const char *b;
    size_t a_length;
    if (!previous->run_count || !next->run_count) return 0;
    a = previous->runs[previous->run_count - 1u].text;
    b = next->runs[0].text;
    if (!a || !b || !a[0] || !b[0]) return 0;
    a_length = strlen(a);
    return (unsigned char)a[a_length - 1u] < 0x80u &&
           (unsigned char)b[0] < 0x80u;
}

/* Width of one grid column in twips, from the detected column origins. */
static int office_pdf_column_twips(const OfficePdfTable *table, size_t column,
                                   int page_w)
{
    double width;
    int twips;
    if (column >= table->column_count) return 1200;
    if (column + 1u < table->column_count)
        width = table->columns[column + 1u] - table->columns[column];
    else
        width = 0.0;
    if (width <= 0.0) width = 120.0;
    if (column + 1u == table->column_count) {
        twips = page_w - (int)(table->columns[column] * 20.0) - 1120;
        if (twips < 400) twips = (int)(width * 20.0);
    } else {
        twips = (int)(width * 20.0);
    }
    return twips < 200 ? 200 : twips;
}

/* Total width of a cell spanning `span` grid columns. */
static int office_pdf_cell_twips(const OfficePdfTable *table, size_t column,
                                 size_t span, int page_w)
{
    int total = 0;
    size_t index;
    for (index = 0u; index < span; ++index)
        total += office_pdf_column_twips(table, column + index, page_w);
    return total;
}

int qw_doc_office_docx_text_from_pdf(const uint8_t *data, size_t size,
                                     const char *password,
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
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/"
        "vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"
        "\"/></Types>";
    static const char package_rels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
        "2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://"
        "schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "officeDocument\" Target=\"word/document.xml\"/></Relationships>";
    static const char document_rels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
        "2006/relationships\"/>";
    static const char document_rels_prefix[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
        "2006/relationships\">";
    static const char document_prefix[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
        "wordprocessingml/2006/main\"><w:body>";
    QwPdfDocument *document = NULL;
    size_t page_count = 0u;
    OfficePdfBuffer body;
    OfficePdfBuffer relationships;
    uint8_t *media_data[64];
    size_t media_size[64];
    size_t media_count = 0u;
    QwZipWriter writer;
    int status;
    int zip_open = 0;
    size_t index;
    double previous_size = 0.0;
    int page_w = 11900;
    int page_h = 16840;
    size_t image_count;

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    status = password
                 ? qw_pdf_open_with_password(data, size, password, &document,
                                             &page_count, report)
                 : qw_pdf_open(data, size, &document, &page_count, report);
    if (status != QW_STATUS_OK) return status;
    image_count = office_pdf_image_count(data, size);
    {
        double media[4];
        double crop[4];
        int rotation = 0;
        if (page_count &&
            qw_pdf_page_geometry(document, 0u, media, crop, &rotation) ==
                QW_STATUS_OK &&
            media[2] > media[0] && media[3] > media[1]) {
            page_w = (int)((media[2] - media[0]) * 20.0 + 0.5);
            page_h = (int)((media[3] - media[1]) * 20.0 + 0.5);
            if (rotation == 90 || rotation == 270) {
                int swap = page_w;
                page_w = page_h;
                page_h = swap;
            }
        }
    }
    memset(&body, 0, sizeof(body));
    memset(&relationships, 0, sizeof(relationships));
    if (!pdf_buffer_text(&body, document_prefix)) goto out_of_memory;
    if (!pdf_buffer_text(&relationships, document_rels_prefix))
        goto out_of_memory;
    for (index = 0u; index < page_count; ++index) {
        QwPdfLine *page_lines = NULL;
        size_t line_count = 0u;
        size_t line_index;
        double base_x = 0.0;
        QwPdfExtractedImage *page_images = NULL;
        size_t page_image_total = 0u;
        size_t next_image = 0u;
        status = qw_doc_pdf_extract_page_lines(document, index, &page_lines,
                                               &line_count);
        if (status != QW_STATUS_OK) goto out_of_memory;
        {
            size_t image_index;
            status = qw_doc_pdf_extract_page_images(document, index,
                                                    &page_images,
                                                    &page_image_total);
            if (status != QW_STATUS_OK) {
                qw_doc_pdf_lines_free(page_lines, line_count);
                goto out_of_memory;
            }
            for (image_index = 0u; image_index < page_image_total;
                 ++image_index) {
                size_t other;
                for (other = image_index + 1u; other < page_image_total;
                     ++other) {
                    if (page_images[other].y + page_images[other].h >
                        page_images[image_index].y +
                            page_images[image_index].h) {
                        QwPdfExtractedImage swap = page_images[image_index];
                        page_images[image_index] = page_images[other];
                        page_images[other] = swap;
                    }
                }
            }
        }
        {
            double media[4];
            double crop[4];
            int rotation = 0;
            if (qw_pdf_page_geometry(document, index, media, crop, &rotation) ==
                    QW_STATUS_OK &&
                rotation != 0) {
                double page_width = media[2] - media[0];
                double page_height = media[3] - media[1];
                size_t line_index2;
                size_t image_index2;
                for (line_index2 = 0u; line_index2 < line_count;
                     ++line_index2) {
                    size_t run_index2;
                    office_pdf_rotate_point(rotation, page_width, page_height,
                                            &page_lines[line_index2].x,
                                            &page_lines[line_index2].y);
                    for (run_index2 = 0u;
                         run_index2 < page_lines[line_index2].run_count;
                         ++run_index2) {
                        office_pdf_rotate_point(
                            rotation, page_width, page_height,
                            &page_lines[line_index2].runs[run_index2].x,
                            &page_lines[line_index2].runs[run_index2].y);
                    }
                }
                for (image_index2 = 0u; image_index2 < page_image_total;
                     ++image_index2) {
                    office_pdf_rotate_point(
                        rotation, page_width, page_height,
                        &page_images[image_index2].x,
                        &page_images[image_index2].y);
                    if (rotation == 90 || rotation == 270) {
                        double swap = page_images[image_index2].w;
                        page_images[image_index2].w =
                            page_images[image_index2].h;
                        page_images[image_index2].h = swap;
                    }
                }
            }
        }
        {
            double column_split = 0.0;
            if (page_image_total == 0u &&
                office_pdf_column_split(page_lines, line_count,
                                        &column_split))
                office_pdf_sort_columns(page_lines, line_count, column_split);
        }
        if (index &&
            !pdf_buffer_text(&body,
                             "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>")) {
            qw_doc_pdf_lines_free(page_lines, line_count);
            goto out_of_memory;
        }
        for (line_index = 0u; line_index < line_count; ++line_index) {
            if (line_index == 0u || page_lines[line_index].x < base_x)
                base_x = page_lines[line_index].x;
        }
        line_index = 0u;
        while (line_index < line_count) {
            size_t block_end = line_index + 1u;
            OfficePdfTable table;
            int indent = (int)((page_lines[line_index].x - base_x) * 20.0);
            while (next_image < page_image_total &&
                   page_images[next_image].y + page_images[next_image].h >=
                       page_lines[line_index].y) {
                if (!office_docx_emit_image(&body, &relationships,
                                            &page_images[next_image],
                                            media_data, media_size,
                                            &media_count)) {
                    qw_doc_pdf_images_free(page_images, page_image_total);
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                ++next_image;
            }
            int spacing = 0;
            if (indent < 0) indent = 0;
            if (indent > 20000) indent = 20000;
            if (page_lines[line_index].run_count >= 2u) {
                block_end = office_pdf_table_block(page_lines, line_count,
                                                   line_index, &table);
            }
            if (block_end - line_index >= 2u) {
                size_t row;
                if (!pdf_buffer_text(
                        &body,
                        "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/>"
                        "<w:tblBorders><w:top w:val=\"single\" w:sz=\"4\" "
                        "w:color=\"auto\"/><w:left w:val=\"single\" w:sz=\"4\" "
                        "w:color=\"auto\"/><w:bottom w:val=\"single\" w:sz=\"4\" "
                        "w:color=\"auto\"/><w:right w:val=\"single\" w:sz=\"4\" "
                        "w:color=\"auto\"/><w:insideH w:val=\"single\" w:sz=\"4\" "
                        "w:color=\"auto\"/><w:insideV w:val=\"single\" w:sz=\"4\" "
                        "w:color=\"auto\"/></w:tblBorders></w:tblPr>")) {
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                {
                    size_t column;
                    if (!pdf_buffer_text(&body, "<w:tblGrid>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                    for (column = 0u; column < table.column_count; ++column) {
                        if (!pdf_buffer_printf(
                                &body, "<w:gridCol w:w=\"%d\"/>",
                                office_pdf_column_twips(&table, column,
                                                        page_w))) {
                            qw_doc_pdf_lines_free(page_lines, line_count);
                            goto out_of_memory;
                        }
                    }
                    if (!pdf_buffer_text(&body, "</w:tblGrid>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                }
                for (row = line_index; row < block_end; ++row) {
                    size_t cell;
                    if (!pdf_buffer_text(&body, "<w:tr>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                    for (cell = 0u; cell < page_lines[row].run_count; ++cell) {
                        const QwPdfRun *run = &page_lines[row].runs[cell];
                        size_t column = office_pdf_column_index(&table, run->x);
                        size_t next_column =
                            cell + 1u < page_lines[row].run_count
                                ? office_pdf_column_index(
                                      &table,
                                      page_lines[row].runs[cell + 1u].x)
                                : table.column_count;
                        size_t span =
                            office_pdf_cell_span(&table, column, next_column);
                        if (!pdf_buffer_text(&body, "<w:tc>") ||
                            !pdf_buffer_text(&body, "<w:tcPr>") ||
                            (span > 1u &&
                             !pdf_buffer_printf(
                                 &body,
                                 "<w:gridSpan w:val=\"%zu\"/>", span)) ||
                            !pdf_buffer_printf(
                                &body,
                                "<w:tcW w:w=\"%d\" w:type=\"dxa\"/>",
                                office_pdf_cell_twips(&table, column, span,
                                                      page_w)) ||
                            !pdf_buffer_text(&body, "</w:tcPr>") ||
                            !pdf_buffer_text(&body, "<w:p><w:r><w:rPr>") ||
                            !pdf_buffer_text(&body,
                                             office_pdf_font_tag(run->font)) ||
                            !pdf_buffer_text(
                                &body,
                                office_pdf_is_bold(run->font) ? "<w:b/>" : "") ||
                            !pdf_buffer_text(&body,
                                             "</w:rPr><w:t xml:space=\"preserve\">") ||
                            !pdf_buffer_text(&body, run->text) ||
                            !pdf_buffer_text(
                                &body, "</w:t></w:r></w:p></w:tc>")) {
                            qw_doc_pdf_lines_free(page_lines, line_count);
                            goto out_of_memory;
                        }
                    }
                    if (!pdf_buffer_text(&body, "</w:tr>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                }
                if (!pdf_buffer_text(&body, "</w:tbl>")) {
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                previous_size = page_lines[block_end - 1u].run_count
                                    ? page_lines[block_end - 1u].runs[0].size
                                    : 0.0;
                line_index = block_end;
                continue;
            }
            if (line_index > 0) {
                double gap = page_lines[line_index - 1u].y -
                             page_lines[line_index].y;
                double expected =
                    (previous_size > 1.0 ? previous_size : 12.0) * 1.5;
                if (gap > expected + 6.0) {
                    spacing = (int)((gap - expected) * 20.0);
                    if (spacing > 20000) spacing = 20000;
                }
            }
            {
                size_t run_index;
                size_t merge_end = line_index + 1u;
                while (merge_end < line_count &&
                       office_pdf_mergeable(&page_lines[merge_end - 1u],
                                            &page_lines[merge_end]) &&
                       !office_pdf_image_between(
                           page_images, page_image_total,
                           page_lines[merge_end - 1u].y,
                           page_lines[merge_end].y) &&
                       office_pdf_table_block(page_lines, line_count,
                                               merge_end, &table) ==
                           merge_end) {
                    ++merge_end;
                }
                if (spacing > 0) {
                    if (!pdf_buffer_printf(
                            &body,
                            "<w:p><w:pPr><w:spacing w:before=\"%d\"/>"
                            "<w:ind w:left=\"%d\"/></w:pPr>",
                            spacing, indent)) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                } else if (!pdf_buffer_printf(
                               &body,
                               "<w:p><w:pPr><w:ind w:left=\"%d\"/></w:pPr>",
                               indent)) {
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                for (run_index = 0u;
                     run_index < page_lines[line_index].run_count;
                     ++run_index) {
                    const QwPdfRun *run =
                        &page_lines[line_index].runs[run_index];
                    const char *vertical = "";
                    double base_size = page_lines[line_index].runs[0].size;
                    if (base_size > 1.0 && run->size > 0.1) {
                        double offset = run->y - page_lines[line_index].y;
                        if (offset > base_size * 0.25)
                            vertical = "<w:vertAlign w:val=\"superscript\"/>";
                        else if (-offset > base_size * 0.25)
                            vertical = "<w:vertAlign w:val=\"subscript\"/>";
                    }
                    if (!pdf_buffer_printf(
                            &body, "<w:r><w:rPr>%s%s%s%s%s</w:rPr>"
                                   "<w:t xml:space=\"preserve\">",
                            office_pdf_font_tag(run->font),
                            office_pdf_is_bold(run->font) ? "<w:b/>" : "",
                            office_pdf_is_italic(run->font) ? "<w:i/>" : "",
                            office_pdf_size_tag(run->size), vertical) ||
                        !pdf_buffer_text(&body, run->text) ||
                        !pdf_buffer_text(&body, "</w:t></w:r>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                }
                {
                    size_t merged_line;
                    for (merged_line = line_index + 1u; merged_line < merge_end;
                         ++merged_line) {
                        if (office_pdf_join_space(
                                &page_lines[merged_line - 1u],
                                &page_lines[merged_line]) &&
                            !pdf_buffer_text(
                                &body,
                                "<w:r><w:t xml:space=\"preserve\"> </w:t>"
                                "</w:r>")) {
                            qw_doc_pdf_lines_free(page_lines, line_count);
                            goto out_of_memory;
                        }
                        for (run_index = 0u;
                             run_index < page_lines[merged_line].run_count;
                             ++run_index) {
                            const QwPdfRun *run =
                                &page_lines[merged_line].runs[run_index];
                            const char *vertical = "";
                            double base_size =
                                page_lines[merged_line].runs[0].size;
                            if (base_size > 1.0 && run->size > 0.1) {
                                double offset =
                                    run->y - page_lines[merged_line].y;
                                if (offset > base_size * 0.25)
                                    vertical =
                                        "<w:vertAlign w:val=\"superscript\"/>";
                                else if (-offset > base_size * 0.25)
                                    vertical =
                                        "<w:vertAlign w:val=\"subscript\"/>";
                            }
                            if (!pdf_buffer_printf(
                                    &body, "<w:r><w:rPr>%s%s%s%s%s</w:rPr>"
                                           "<w:t xml:space=\"preserve\">",
                                    office_pdf_font_tag(run->font),
                                    office_pdf_is_bold(run->font)
                                        ? "<w:b/>" : "",
                                    office_pdf_is_italic(run->font)
                                        ? "<w:i/>" : "",
                                    office_pdf_size_tag(run->size), vertical) ||
                                !pdf_buffer_text(&body, run->text) ||
                                !pdf_buffer_text(&body, "</w:t></w:r>")) {
                                qw_doc_pdf_lines_free(page_lines, line_count);
                                goto out_of_memory;
                            }
                        }
                    }
                }
                if (!pdf_buffer_text(&body, "</w:p>")) {
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                previous_size = page_lines[merge_end - 1u].run_count
                                    ? page_lines[merge_end - 1u].runs[0].size
                                    : 0.0;
                line_index = merge_end;
            }
        }
        while (next_image < page_image_total) {
            if (!office_docx_emit_image(&body, &relationships,
                                        &page_images[next_image], media_data,
                                        media_size, &media_count)) {
                qw_doc_pdf_images_free(page_images, page_image_total);
                qw_doc_pdf_lines_free(page_lines, line_count);
                goto out_of_memory;
            }
            ++next_image;
        }
        qw_doc_pdf_images_free(page_images, page_image_total);
        qw_doc_pdf_lines_free(page_lines, line_count);
    }
    qw_pdf_close(document);
    document = NULL;
    if (!pdf_buffer_printf(
            &body,
            "<w:sectPr><w:pgSz w:w=\"%d\" w:h=\"%d\"/>"
            "<w:pgMar w:top=\"1134\" w:right=\"1134\" w:bottom=\"1134\" "
            "w:left=\"1134\" w:header=\"0\" w:footer=\"0\" "
            "w:gutter=\"0\"/></w:sectPr>",
            page_w, page_h) ||
        !pdf_buffer_text(&body, "</w:body></w:document>"))
        goto out_of_memory;
    if (!pdf_buffer_text(&relationships, "</Relationships>"))
        goto out_of_memory;
    if (!qw_zip_writer_init_heap(&writer, 0u, 0u)) goto out_of_memory;
    zip_open = 1;
    if (!office_zip_add(&writer, "[Content_Types].xml", content_types,
                        strlen(content_types)) ||
        !office_zip_add(&writer, "_rels/.rels", package_rels,
                        strlen(package_rels)) ||
        !office_zip_add(&writer, "word/document.xml", body.data, body.size) ||
        !office_zip_add(&writer, "word/_rels/document.xml.rels",
                        relationships.data, relationships.size))
        goto out_of_memory;
    {
        size_t media_index;
        for (media_index = 0u; media_index < media_count; ++media_index) {
            char name[64];
            snprintf(name, sizeof(name), "word/media/image%zu.png",
                     media_index + 1u);
            if (!office_zip_add(&writer, name, media_data[media_index],
                                media_size[media_index]))
                goto out_of_memory;
        }
    }
    if (!office_zip_finish(&writer, out_data, out_size)) goto out_of_memory;
    zip_open = 0;
    free(body.data);
    if (report) {
        report->format = QW_FORMAT_PDF;
        report->output_format = QW_FORMAT_DOCX;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "editable-text DOCX: %zu page(s) extracted into paragraphs "
                 "with page breaks (text layer only, layout not reconstructed)",
                 page_count);
        if (media_count) {
            size_t used = strlen(report->note);
            snprintf(report->note + used, sizeof(report->note) - used,
                     "; %zu image(s) embedded", media_count);
        } else if (image_count) {
            size_t used = strlen(report->note);
            snprintf(report->note + used, sizeof(report->note) - used,
                     "; %zu image(s) are not included (unsupported format)",
                     image_count);
        }
    }
    return QW_STATUS_OK;

out_of_memory:
    if (zip_open) qw_zip_writer_end(&writer);
    if (document) qw_pdf_close(document);
    free(body.data);
    return QW_STATUS_ERR_OUT_OF_MEMORY;
}

/* Encode one extracted image as PNG media and emit the EPUB img element. */
static int office_epub_emit_image(OfficePdfBuffer *chapter,
                                  OfficePdfBuffer *manifest,
                                  const QwPdfExtractedImage *image,
                                  uint8_t **media_data, size_t *media_size,
                                  size_t *media_count)
{
    QwImage encoded;
    uint8_t *png = NULL;
    size_t png_size = 0u;
    if (*media_count >= 64u) return 1;
    memset(&encoded, 0, sizeof(encoded));
    encoded.pixels = image->rgb;
    encoded.stride = image->width * 3u;
    encoded.size = encoded.stride * image->height;
    encoded.width = (uint32_t)image->width;
    encoded.height = (uint32_t)image->height;
    encoded.format = QW_IMAGE_PIXEL_RGB24;
    if (qw_image_encode(&encoded, QW_IMAGE_ENCODE_PNG, &png, &png_size) !=
        QW_IMAGE_OK)
        return 1;
    media_data[*media_count] = png;
    media_size[*media_count] = png_size;
    ++*media_count;
    if (!pdf_buffer_printf(chapter,
                           "<p><img src=\"image%zu.png\" alt=\"\" "
                           "style=\"max-width:100%%\"/></p>", *media_count) ||
        !pdf_buffer_printf(manifest,
                           "<item id=\"image%zu\" href=\"image%zu.png\" "
                           "media-type=\"image/png\"/>", *media_count,
                           *media_count))
        return 0;
    return 1;
}

int qw_doc_office_epub_text_from_pdf(const uint8_t *data, size_t size,
                                     const char *password,
                                     uint8_t **out_data, size_t *out_size,
                                     QwConvertReport *report)
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
        "dc/elements/1.1/\"><dc:title>Quirewise PDF text</dc:title>"
        "<dc:identifier id=\"bookid\">urn:uuid:quirewise-pdf-text</dc:"
        "identifier><dc:language>und</dc:language></metadata><manifest>"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/"
        "x-dtbncx+xml\"/><item id=\"chapter\" href=\"chapter1.xhtml\" "
        "media-type=\"application/xhtml+xml\"/>";
    static const char content_opf_suffix[] =
        "</manifest><spine toc=\"ncx\"><itemref idref=\"chapter\"/>"
        "</spine></package>";
    static const char toc_ncx[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\""
        "><head><meta name=\"dtb:uid\" content=\"urn:uuid:quirewise-pdf-text\""
        "/><meta name=\"dtb:depth\" content=\"1\"/></head><docTitle>"
        "<text>Quirewise PDF text</text></docTitle><navMap><navPoint "
        "id=\"chapter\" playOrder=\"1\"><navLabel><text>Text</text></navLabel>"
        "<content src=\"chapter1.xhtml\"/></navPoint></navMap></ncx>";
    static const char chapter_prefix[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE html><html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
        "<title>Quirewise PDF text</title><style>.page{margin:0 0 2em 0}"
        "</style></head><body>";
    QwPdfDocument *document = NULL;
    size_t page_count = 0u;
    OfficePdfBuffer chapter;
    OfficePdfBuffer manifest;
    OfficePdfBuffer opf;
    uint8_t *media_data[64];
    size_t media_size[64];
    size_t media_count = 0u;
    QwZipWriter writer;
    int status;
    int zip_open = 0;
    size_t index;
    double previous_size = 0.0;
    size_t image_count;

    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    status = password
                 ? qw_pdf_open_with_password(data, size, password, &document,
                                             &page_count, report)
                 : qw_pdf_open(data, size, &document, &page_count, report);
    if (status != QW_STATUS_OK) return status;
    image_count = office_pdf_image_count(data, size);
    memset(&chapter, 0, sizeof(chapter));
    memset(&manifest, 0, sizeof(manifest));
    if (!pdf_buffer_text(&chapter, chapter_prefix)) goto out_of_memory;
    for (index = 0u; index < page_count; ++index) {
        QwPdfLine *page_lines = NULL;
        size_t line_count = 0u;
        size_t line_index;
        double base_x = 0.0;
        QwPdfExtractedImage *page_images = NULL;
        size_t page_image_total = 0u;
        size_t next_image = 0u;
        status = qw_doc_pdf_extract_page_lines(document, index, &page_lines,
                                               &line_count);
        if (status != QW_STATUS_OK) goto out_of_memory;
        {
            size_t image_index;
            status = qw_doc_pdf_extract_page_images(document, index,
                                                    &page_images,
                                                    &page_image_total);
            if (status != QW_STATUS_OK) {
                qw_doc_pdf_lines_free(page_lines, line_count);
                goto out_of_memory;
            }
            for (image_index = 0u; image_index < page_image_total;
                 ++image_index) {
                size_t other;
                for (other = image_index + 1u; other < page_image_total;
                     ++other) {
                    if (page_images[other].y + page_images[other].h >
                        page_images[image_index].y +
                            page_images[image_index].h) {
                        QwPdfExtractedImage swap = page_images[image_index];
                        page_images[image_index] = page_images[other];
                        page_images[other] = swap;
                    }
                }
            }
        }
        {
            double media[4];
            double crop[4];
            int rotation = 0;
            if (qw_pdf_page_geometry(document, index, media, crop, &rotation) ==
                    QW_STATUS_OK &&
                rotation != 0) {
                double page_width = media[2] - media[0];
                double page_height = media[3] - media[1];
                size_t line_index2;
                size_t image_index2;
                for (line_index2 = 0u; line_index2 < line_count;
                     ++line_index2) {
                    size_t run_index2;
                    office_pdf_rotate_point(rotation, page_width, page_height,
                                            &page_lines[line_index2].x,
                                            &page_lines[line_index2].y);
                    for (run_index2 = 0u;
                         run_index2 < page_lines[line_index2].run_count;
                         ++run_index2) {
                        office_pdf_rotate_point(
                            rotation, page_width, page_height,
                            &page_lines[line_index2].runs[run_index2].x,
                            &page_lines[line_index2].runs[run_index2].y);
                    }
                }
                for (image_index2 = 0u; image_index2 < page_image_total;
                     ++image_index2) {
                    office_pdf_rotate_point(
                        rotation, page_width, page_height,
                        &page_images[image_index2].x,
                        &page_images[image_index2].y);
                    if (rotation == 90 || rotation == 270) {
                        double swap = page_images[image_index2].w;
                        page_images[image_index2].w =
                            page_images[image_index2].h;
                        page_images[image_index2].h = swap;
                    }
                }
            }
        }
        {
            double column_split = 0.0;
            if (page_image_total == 0u &&
                office_pdf_column_split(page_lines, line_count,
                                        &column_split))
                office_pdf_sort_columns(page_lines, line_count, column_split);
        }
        if (!pdf_buffer_text(&chapter, "<div class=\"page\">")) {
            qw_doc_pdf_lines_free(page_lines, line_count);
            goto out_of_memory;
        }
        for (line_index = 0u; line_index < line_count; ++line_index) {
            if (line_index == 0u || page_lines[line_index].x < base_x)
                base_x = page_lines[line_index].x;
        }
        line_index = 0u;
        while (line_index < line_count) {
            size_t block_end = line_index + 1u;
            OfficePdfTable table;
            double indent = (page_lines[line_index].x - base_x);
            while (next_image < page_image_total &&
                   page_images[next_image].y + page_images[next_image].h >=
                       page_lines[line_index].y) {
                if (!office_epub_emit_image(&chapter, &manifest,
                                            &page_images[next_image],
                                            media_data, media_size,
                                            &media_count)) {
                    qw_doc_pdf_images_free(page_images, page_image_total);
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                ++next_image;
            }
            if (indent < 0.0) indent = 0.0;
            if (page_lines[line_index].run_count >= 2u) {
                block_end = office_pdf_table_block(page_lines, line_count,
                                                   line_index, &table);
            }
            if (block_end - line_index >= 2u) {
                size_t row;
                if (!pdf_buffer_text(&chapter,
                                     "<table border=\"1\" cellspacing=\"0\" "
                                     "cellpadding=\"4\">")) {
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                {
                    size_t column;
                    double widths[OFFICE_PDF_MAX_COLUMNS];
                    double total = 0.0;
                    for (column = 0u; column < table.column_count; ++column) {
                        widths[column] =
                            column + 1u < table.column_count
                                ? table.columns[column + 1u] -
                                      table.columns[column]
                                : (column ? widths[column - 1u] : 100.0);
                        total += widths[column];
                    }
                    if (total <= 0.0) total = 1.0;
                    if (!pdf_buffer_text(&chapter, "<colgroup>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                    for (column = 0u; column < table.column_count; ++column) {
                        if (!pdf_buffer_printf(
                                &chapter, "<col style=\"width:%.1f%%\"/>",
                                widths[column] * 100.0 / total)) {
                            qw_doc_pdf_lines_free(page_lines, line_count);
                            goto out_of_memory;
                        }
                    }
                    if (!pdf_buffer_text(&chapter, "</colgroup>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                }
                for (row = line_index; row < block_end; ++row) {
                    size_t cell;
                    if (!pdf_buffer_text(&chapter, "<tr>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                    for (cell = 0u; cell < page_lines[row].run_count; ++cell) {
                        const QwPdfRun *run = &page_lines[row].runs[cell];
                        size_t column = office_pdf_column_index(&table, run->x);
                        size_t next_column =
                            cell + 1u < page_lines[row].run_count
                                ? office_pdf_column_index(
                                      &table,
                                      page_lines[row].runs[cell + 1u].x)
                                : table.column_count;
                        size_t span =
                            office_pdf_cell_span(&table, column, next_column);
                        if (!pdf_buffer_text(&chapter, "<td") ||
                            (span > 1u &&
                             !pdf_buffer_printf(&chapter,
                                                " colspan=\"%zu\"", span)) ||
                            !pdf_buffer_text(&chapter, ">") ||
                            !pdf_buffer_text(&chapter, run->text) ||
                            !pdf_buffer_text(&chapter, "</td>")) {
                            qw_doc_pdf_lines_free(page_lines, line_count);
                            goto out_of_memory;
                        }
                    }
                    if (!pdf_buffer_text(&chapter, "</tr>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                }
                if (!pdf_buffer_text(&chapter, "</table>")) {
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                previous_size = page_lines[block_end - 1u].run_count
                                    ? page_lines[block_end - 1u].runs[0].size
                                    : 0.0;
                line_index = block_end;
                continue;
            }
            {
                size_t run_index;
                int spacing = 0;
                size_t merge_end = line_index + 1u;
                while (merge_end < line_count &&
                       office_pdf_mergeable(&page_lines[merge_end - 1u],
                                            &page_lines[merge_end]) &&
                       !office_pdf_image_between(
                           page_images, page_image_total,
                           page_lines[merge_end - 1u].y,
                           page_lines[merge_end].y) &&
                       office_pdf_table_block(page_lines, line_count,
                                               merge_end, &table) ==
                           merge_end) {
                    ++merge_end;
                }
                if (line_index > 0) {
                    double gap = page_lines[line_index - 1u].y -
                                 page_lines[line_index].y;
                    double expected =
                        (previous_size > 1.0 ? previous_size : 12.0) * 1.5;
                    if (gap > expected + 6.0) {
                        spacing = (int)((gap - expected) * 20.0);
                        if (spacing > 20000) spacing = 20000;
                    }
                }
                if (spacing > 0) {
                    if (!pdf_buffer_printf(
                            &chapter,
                            "<p style=\"margin-left:%.1fpt;margin-top:%.1fpt\">",
                            indent, (double)spacing / 20.0)) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                } else if (!pdf_buffer_printf(
                               &chapter,
                               "<p style=\"margin-left:%.1fpt\">", indent)) {
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                for (run_index = 0u;
                     run_index < page_lines[line_index].run_count;
                     ++run_index) {
                    const QwPdfRun *run =
                        &page_lines[line_index].runs[run_index];
                    const char *vertical_open = "";
                    const char *vertical_close = "";
                    double base_size = page_lines[line_index].runs[0].size;
                    if (base_size > 1.0 && run->size > 0.1) {
                        double offset = run->y - page_lines[line_index].y;
                        if (offset > base_size * 0.25) {
                            vertical_open = "<sup>";
                            vertical_close = "</sup>";
                        } else if (-offset > base_size * 0.25) {
                            vertical_open = "<sub>";
                            vertical_close = "</sub>";
                        }
                    }
                    if (!pdf_buffer_printf(&chapter,
                                           "<span style=\"%s%s%s\">",
                                           office_pdf_font_css(run->font),
                                           office_pdf_is_bold(run->font)
                                               ? "font-weight:bold;" : "",
                                           office_pdf_is_italic(run->font)
                                               ? "font-style:italic;" : "") ||
                        !pdf_buffer_text(&chapter, vertical_open) ||
                        !pdf_buffer_text(&chapter, run->text) ||
                        !pdf_buffer_text(&chapter, vertical_close) ||
                        !pdf_buffer_text(&chapter, "</span>")) {
                        qw_doc_pdf_lines_free(page_lines, line_count);
                        goto out_of_memory;
                    }
                }
                {
                    size_t merged_line;
                    for (merged_line = line_index + 1u; merged_line < merge_end;
                         ++merged_line) {
                        if (office_pdf_join_space(
                                &page_lines[merged_line - 1u],
                                &page_lines[merged_line]) &&
                            !pdf_buffer_text(&chapter, " ")) {
                            qw_doc_pdf_lines_free(page_lines, line_count);
                            goto out_of_memory;
                        }
                        for (run_index = 0u;
                             run_index < page_lines[merged_line].run_count;
                             ++run_index) {
                            const QwPdfRun *run =
                                &page_lines[merged_line].runs[run_index];
                            const char *vertical_open = "";
                            const char *vertical_close = "";
                            double base_size =
                                page_lines[merged_line].runs[0].size;
                            if (base_size > 1.0 && run->size > 0.1) {
                                double offset =
                                    run->y - page_lines[merged_line].y;
                                if (offset > base_size * 0.25) {
                                    vertical_open = "<sup>";
                                    vertical_close = "</sup>";
                                } else if (-offset > base_size * 0.25) {
                                    vertical_open = "<sub>";
                                    vertical_close = "</sub>";
                                }
                            }
                            if (!pdf_buffer_printf(
                                    &chapter, "<span style=\"%s%s%s\">",
                                    office_pdf_font_css(run->font),
                                    office_pdf_is_bold(run->font)
                                        ? "font-weight:bold;" : "",
                                    office_pdf_is_italic(run->font)
                                        ? "font-style:italic;" : "") ||
                                !pdf_buffer_text(&chapter, vertical_open) ||
                                !pdf_buffer_text(&chapter, run->text) ||
                                !pdf_buffer_text(&chapter, vertical_close) ||
                                !pdf_buffer_text(&chapter, "</span>")) {
                                qw_doc_pdf_lines_free(page_lines, line_count);
                                goto out_of_memory;
                            }
                        }
                    }
                }
                if (!pdf_buffer_text(&chapter, "</p>")) {
                    qw_doc_pdf_lines_free(page_lines, line_count);
                    goto out_of_memory;
                }
                previous_size = page_lines[merge_end - 1u].run_count
                                    ? page_lines[merge_end - 1u].runs[0].size
                                    : 0.0;
                line_index = merge_end;
            }
        }
        while (next_image < page_image_total) {
            if (!office_epub_emit_image(&chapter, &manifest,
                                        &page_images[next_image], media_data,
                                        media_size, &media_count)) {
                qw_doc_pdf_images_free(page_images, page_image_total);
                qw_doc_pdf_lines_free(page_lines, line_count);
                goto out_of_memory;
            }
            ++next_image;
        }
        qw_doc_pdf_images_free(page_images, page_image_total);
        qw_doc_pdf_lines_free(page_lines, line_count);
        if (!pdf_buffer_text(&chapter, "</div>")) goto out_of_memory;
    }
    qw_pdf_close(document);
    document = NULL;
    if (!pdf_buffer_text(&chapter, "</body></html>")) goto out_of_memory;
    if (!pdf_buffer_text(&manifest, content_opf_suffix)) goto out_of_memory;
    memset(&opf, 0, sizeof(opf));
    if (!pdf_buffer_text(&opf, content_opf_prefix) ||
        !pdf_buffer_append(&opf, manifest.data, manifest.size))
        goto out_of_memory;
    if (!qw_zip_writer_init_heap(&writer, 0u, 0u)) goto out_of_memory;
    zip_open = 1;
    if (!office_zip_add(&writer, "mimetype", "application/epub+zip", 20u) ||
        !office_zip_add(&writer, "META-INF/container.xml", container_xml,
                        strlen(container_xml)) ||
        !office_zip_add(&writer, "OEBPS/content.opf", opf.data,
                        opf.size) ||
        !office_zip_add(&writer, "OEBPS/toc.ncx", toc_ncx, strlen(toc_ncx)) ||
        !office_zip_add(&writer, "OEBPS/chapter1.xhtml", chapter.data,
                        chapter.size))
        goto out_of_memory;
    {
        size_t media_index;
        for (media_index = 0u; media_index < media_count; ++media_index) {
            char name[64];
            snprintf(name, sizeof(name), "OEBPS/image%zu.png",
                     media_index + 1u);
            if (!office_zip_add(&writer, name, media_data[media_index],
                                media_size[media_index]))
                goto out_of_memory;
        }
    }
    if (!office_zip_finish(&writer, out_data, out_size)) goto out_of_memory;
    zip_open = 0;
    free(chapter.data);
    if (report) {
        report->format = QW_FORMAT_PDF;
        report->output_format = QW_FORMAT_EPUB;
        report->output_size = *out_size;
        snprintf(report->note, sizeof(report->note),
                 "editable-text EPUB: %zu page(s) extracted into page sections "
                 "(text layer only, layout not reconstructed)",
                 page_count);
        if (media_count) {
            size_t used = strlen(report->note);
            snprintf(report->note + used, sizeof(report->note) - used,
                     "; %zu image(s) embedded", media_count);
        } else if (image_count) {
            size_t used = strlen(report->note);
            snprintf(report->note + used, sizeof(report->note) - used,
                     "; %zu image(s) are not included (unsupported format)",
                     image_count);
        }
    }
    return QW_STATUS_OK;

out_of_memory:
    if (zip_open) qw_zip_writer_end(&writer);
    if (document) qw_pdf_close(document);
    free(chapter.data);
    return QW_STATUS_ERR_OUT_OF_MEMORY;
}
