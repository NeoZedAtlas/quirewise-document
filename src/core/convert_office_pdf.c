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
