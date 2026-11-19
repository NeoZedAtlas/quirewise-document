/* PDF -> DOCX/EPUB: layout-preserving page images vs editable text mode. */
#include <quirewise/convert.h>
#include <quirewise/pdf.h>
#include <quirewise/image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static int contains(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}


static const uint8_t *find_bytes(const uint8_t *data, size_t size,
                                 const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    if (!needle_size || needle_size > size) return NULL;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0)
            return data + index;
    }
    return NULL;
}

int main(void)
{
    static const char *html =
        "<html><body><h1>Filee test 123</h1><p>a b e f g</p></body></html>";
    QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    uint8_t *docx = NULL;
    size_t docx_size = 0u;
    uint8_t *epub = NULL;
    size_t epub_size = 0u;

    CHECK(qw_convert_to_pdf_memory(html, strlen(html), &options, &pdf,
                                   &pdf_size, NULL) == QW_STATUS_OK);
    CHECK(pdf != NULL && pdf_size > 0u);

    /* Default mode keeps the page images. */
    options.pdf_office_reflow = 0;
    CHECK(qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_DOCX, &options,
                                      &docx, &docx_size, NULL) ==
          QW_STATUS_OK);
    CHECK(contains(docx, docx_size, "word/media/page1.jpg"));

    /* Reflow mode emits paragraphs and no media parts. */
    options.pdf_office_reflow = 1;
    free(docx);
    docx = NULL;
    docx_size = 0u;
    CHECK(qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_DOCX, &options,
                                      &docx, &docx_size, NULL) ==
          QW_STATUS_OK);
    CHECK(contains(docx, docx_size, "word/document.xml"));
    CHECK(!contains(docx, docx_size, "word/media/"));
    CHECK(contains(docx, docx_size, "Filee test 123"));
    CHECK(contains(docx, docx_size, "a b e f g"));

    /* The EPUB route follows the same option. */
    CHECK(qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_EPUB, &options,
                                      &epub, &epub_size, NULL) ==
          QW_STATUS_OK);
    CHECK(contains(epub, epub_size, "OEBPS/chapter1.xhtml"));
    CHECK(contains(epub, epub_size, "Filee test 123"));
    CHECK(!contains(epub, epub_size, "page1.jpg"));

    free(pdf);
    free(docx);
    free(epub);
    /* Two-page PDF (CSS page break): reflow keeps page boundaries. */
    {
        static const char *two_page_html =
            "<html><body><p>page one</p>"
            "<p style=\"page-break-before: always\">second page</p>"
            "</body></html>";
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        uint8_t *two_page_pdf = NULL;
        size_t two_page_size = 0u;
        uint8_t *two_docx = NULL;
        size_t two_docx_size = 0u;
        uint8_t *two_epub = NULL;
        size_t two_epub_size = 0u;

        CHECK(qw_convert_to_pdf_memory(two_page_html, strlen(two_page_html),
                                       &opts, &two_page_pdf, &two_page_size,
                                       NULL) == QW_STATUS_OK);
        opts.pdf_office_reflow = 1;
        CHECK(qw_convert_to_format_memory(two_page_pdf, two_page_size,
                                          QW_FORMAT_DOCX, &opts, &two_docx,
                                          &two_docx_size, NULL) == QW_STATUS_OK);
        CHECK(contains(two_docx, two_docx_size, "w:type=\"page\""));
        CHECK(contains(two_docx, two_docx_size, "second page"));
        CHECK(qw_convert_to_format_memory(two_page_pdf, two_page_size,
                                          QW_FORMAT_EPUB, &opts, &two_epub,
                                          &two_epub_size, NULL) == QW_STATUS_OK);
        CHECK(contains(two_epub, two_epub_size, "class=\"page\""));
        CHECK(contains(two_epub, two_epub_size, "second page"));
        free(two_page_pdf);
        free(two_docx);
        free(two_epub);
    }

    /* Positioned lines: the second line is indented by 48 pt. */
    {
        FILE *file = fopen(QUIREWISE_INDENTED_PDF, "rb");
        long length;
        uint8_t *bytes = NULL;
        size_t byte_count = 0u;
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        uint8_t *indented_docx = NULL;
        size_t indented_docx_size = 0u;
        uint8_t *indented_epub = NULL;
        size_t indented_epub_size = 0u;

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        bytes = (uint8_t *)malloc((size_t)length);
        CHECK(bytes != NULL);
        byte_count = fread(bytes, 1u, (size_t)length, file);
        fclose(file);
        CHECK(byte_count == (size_t)length);
        opts.pdf_office_reflow = 1;
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_DOCX,
                                          &opts, &indented_docx,
                                          &indented_docx_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(indented_docx, indented_docx_size, "Hello"));
        CHECK(contains(indented_docx, indented_docx_size, "World"));
        CHECK(contains(indented_docx, indented_docx_size,
                       "w:left=\"960\""));
        CHECK(contains(indented_docx, indented_docx_size,
                       "w:pgSz w:w=\"11900\" w:h=\"16840\""));
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_EPUB,
                                          &opts, &indented_epub,
                                          &indented_epub_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(indented_epub, indented_epub_size,
                       "margin-left:48.0pt"));
        free(bytes);
        free(indented_docx);
        free(indented_epub);
    }

    /* Font name and size become DOCX run properties. */
    {
        FILE *file = fopen(QUIREWISE_STYLES_PDF, "rb");
        long length;
        uint8_t *bytes = NULL;
        size_t byte_count = 0u;
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        uint8_t *styled_docx = NULL;
        size_t styled_docx_size = 0u;
        uint8_t *styled_epub = NULL;
        size_t styled_epub_size = 0u;

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        bytes = (uint8_t *)malloc((size_t)length);
        CHECK(bytes != NULL);
        byte_count = fread(bytes, 1u, (size_t)length, file);
        fclose(file);
        CHECK(byte_count == (size_t)length);
        opts.pdf_office_reflow = 1;
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_DOCX,
                                          &opts, &styled_docx,
                                          &styled_docx_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(styled_docx, styled_docx_size, "Plain"));
        CHECK(contains(styled_docx, styled_docx_size, "Bold"));
        CHECK(contains(styled_docx, styled_docx_size, "Italic"));
        CHECK(contains(styled_docx, styled_docx_size, "<w:b/>"));
        CHECK(contains(styled_docx, styled_docx_size, "<w:i/>"));
        CHECK(contains(styled_docx, styled_docx_size, "w:sz w:val=\"36\""));
        CHECK(contains(styled_docx, styled_docx_size, "w:sz w:val=\"24\""));
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_EPUB,
                                          &opts, &styled_epub,
                                          &styled_epub_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(styled_epub, styled_epub_size, "font-weight:bold"));
        CHECK(contains(styled_epub, styled_epub_size, "font-style:italic"));
        free(bytes);
        free(styled_docx);
        free(styled_epub);
    }

    /* One line with two fonts becomes two runs in one paragraph. */
    {
        FILE *file = fopen(QUIREWISE_INLINE_PDF, "rb");
        long length;
        uint8_t *bytes = NULL;
        size_t byte_count = 0u;
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        uint8_t *inline_docx = NULL;
        size_t inline_docx_size = 0u;
        uint8_t *inline_epub = NULL;
        size_t inline_epub_size = 0u;

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        bytes = (uint8_t *)malloc((size_t)length);
        CHECK(bytes != NULL);
        byte_count = fread(bytes, 1u, (size_t)length, file);
        fclose(file);
        CHECK(byte_count == (size_t)length);
        opts.pdf_office_reflow = 1;
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_DOCX,
                                          &opts, &inline_docx,
                                          &inline_docx_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(inline_docx, inline_docx_size, "plain "));
        CHECK(contains(inline_docx, inline_docx_size, "bold"));
        CHECK(contains(inline_docx, inline_docx_size, "<w:b/>"));
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_EPUB,
                                          &opts, &inline_epub,
                                          &inline_epub_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(inline_epub, inline_epub_size, "font-weight:bold"));
        CHECK(contains(inline_epub, inline_epub_size, "plain "));
        free(bytes);
        free(inline_docx);
        free(inline_epub);
    }

    /* A large vertical gap becomes paragraph spacing. */
    {
        FILE *file = fopen(QUIREWISE_SPACING_PDF, "rb");
        long length;
        uint8_t *bytes = NULL;
        size_t byte_count = 0u;
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        uint8_t *spaced_docx = NULL;
        size_t spaced_docx_size = 0u;
        uint8_t *spaced_epub = NULL;
        size_t spaced_epub_size = 0u;

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        bytes = (uint8_t *)malloc((size_t)length);
        CHECK(bytes != NULL);
        byte_count = fread(bytes, 1u, (size_t)length, file);
        fclose(file);
        CHECK(byte_count == (size_t)length);
        opts.pdf_office_reflow = 1;
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_DOCX,
                                          &opts, &spaced_docx,
                                          &spaced_docx_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(spaced_docx, spaced_docx_size, "Top"));
        CHECK(contains(spaced_docx, spaced_docx_size, "Bottom"));
        CHECK(contains(spaced_docx, spaced_docx_size,
                       "w:spacing w:before=\"1640\""));
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_EPUB,
                                          &opts, &spaced_epub,
                                          &spaced_epub_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(spaced_epub, spaced_epub_size, "margin-top:82.0pt"));
        free(bytes);
        free(spaced_docx);
        free(spaced_epub);
    }

    /* A smaller, raised run becomes superscript. */
    {
        FILE *file = fopen(QUIREWISE_SCRIPT_PDF, "rb");
        long length;
        uint8_t *bytes = NULL;
        size_t byte_count = 0u;
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        uint8_t *script_docx = NULL;
        size_t script_docx_size = 0u;
        uint8_t *script_epub = NULL;
        size_t script_epub_size = 0u;

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        bytes = (uint8_t *)malloc((size_t)length);
        CHECK(bytes != NULL);
        byte_count = fread(bytes, 1u, (size_t)length, file);
        fclose(file);
        CHECK(byte_count == (size_t)length);
        opts.pdf_office_reflow = 1;
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_DOCX,
                                          &opts, &script_docx,
                                          &script_docx_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(script_docx, script_docx_size, "x"));
        CHECK(contains(script_docx, script_docx_size, "2"));
        CHECK(contains(script_docx, script_docx_size,
                       "w:vertAlign w:val=\"superscript\""));
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_EPUB,
                                          &opts, &script_epub,
                                          &script_epub_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(script_epub, script_epub_size, "<sup>"));
        free(bytes);
        free(script_docx);
        free(script_epub);
    }

    /* Images are reported as skipped in the text-only output. */
    {
        FILE *file = fopen(QUIREWISE_IMAGE_PDF, "rb");
        long length;
        uint8_t *bytes = NULL;
        size_t byte_count = 0u;
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        QwConvertReport report;
        uint8_t *image_docx = NULL;
        size_t image_docx_size = 0u;

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        bytes = (uint8_t *)malloc((size_t)length);
        CHECK(bytes != NULL);
        byte_count = fread(bytes, 1u, (size_t)length, file);
        fclose(file);
        CHECK(byte_count == (size_t)length);
        opts.pdf_office_reflow = 1;
        memset(&report, 0, sizeof(report));
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_DOCX,
                                          &opts, &image_docx,
                                          &image_docx_size, &report) ==
              QW_STATUS_OK);
        CHECK(strstr(report.note, "image(s) embedded") != NULL);
        CHECK(contains(image_docx, image_docx_size, "word/media/image1.png"));
        free(bytes);
        free(image_docx);
    }

    /* Column-aligned rows become a DOCX table. */
    {
        FILE *file = fopen(QUIREWISE_TABLE_PDF, "rb");
        long length;
        uint8_t *bytes = NULL;
        size_t byte_count = 0u;
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        uint8_t *table_docx = NULL;
        size_t table_docx_size = 0u;

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        bytes = (uint8_t *)malloc((size_t)length);
        CHECK(bytes != NULL);
        byte_count = fread(bytes, 1u, (size_t)length, file);
        fclose(file);
        CHECK(byte_count == (size_t)length);
        opts.pdf_office_reflow = 1;
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_DOCX,
                                          &opts, &table_docx,
                                          &table_docx_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(table_docx, table_docx_size, "<w:tbl>"));
        CHECK(contains(table_docx, table_docx_size, "<w:tblBorders>"));
        CHECK(contains(table_docx, table_docx_size, "<w:tr>"));
        CHECK(contains(table_docx, table_docx_size, "Name"));
        CHECK(contains(table_docx, table_docx_size, "Qty"));
        CHECK(contains(table_docx, table_docx_size, "Apple"));
        CHECK(contains(table_docx, table_docx_size, "Pear"));
        {
            uint8_t *table_epub = NULL;
            size_t table_epub_size = 0u;
            CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_EPUB,
                                              &opts, &table_epub,
                                              &table_epub_size, NULL) ==
                  QW_STATUS_OK);
            CHECK(contains(table_epub, table_epub_size,
                           "<table border=\"1\""));
            CHECK(contains(table_epub, table_epub_size, "<tr>"));
            CHECK(contains(table_epub, table_epub_size, "<td>"));
            CHECK(contains(table_epub, table_epub_size, "Apple"));
            free(table_epub);
        }
        free(bytes);
        free(table_docx);
    }

    /* Images embedded in the PDF become PNG media in the DOCX. */
    {
        static const char *image_html =
            "<html><body><p>pic</p>"
            "<img src=\"data:image/png;base64,"
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8"
            "z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==\">"
            "</body></html>";
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        uint8_t *image_pdf = NULL;
        size_t image_pdf_size = 0u;
        uint8_t *media_docx = NULL;
        size_t media_docx_size = 0u;
        QwConvertReport media_report;

        CHECK(qw_convert_to_pdf_memory(image_html, strlen(image_html), &opts,
                                       &image_pdf, &image_pdf_size,
                                       NULL) == QW_STATUS_OK);
        opts.pdf_office_reflow = 1;
        memset(&media_report, 0, sizeof(media_report));
        CHECK(qw_convert_to_format_memory(image_pdf, image_pdf_size,
                                          QW_FORMAT_DOCX, &opts, &media_docx,
                                          &media_docx_size,
                                          &media_report) == QW_STATUS_OK);
        CHECK(contains(media_docx, media_docx_size, "word/media/image1.png"));
        CHECK(contains(media_docx, media_docx_size, "<w:drawing>"));
        CHECK(strstr(media_report.note, "image(s) embedded") != NULL);
        {
            uint8_t *media_epub = NULL;
            size_t media_epub_size = 0u;
            CHECK(qw_convert_to_format_memory(image_pdf, image_pdf_size,
                                              QW_FORMAT_EPUB, &opts, &media_epub,
                                              &media_epub_size, NULL) ==
                  QW_STATUS_OK);
            CHECK(contains(media_epub, media_epub_size, "OEBPS/image1.png"));
            CHECK(contains(media_epub, media_epub_size,
                           "img src=\"image1.png\""));
            free(media_epub);
        }
        free(image_pdf);
        free(media_docx);
    }

    /* Images keep their vertical position between paragraphs. */
    {
        static const char *order_html =
            "<html><body><p>before</p>"
            "<img src=\"data:image/png;base64,"
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8"
            "z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==\">"
            "<p>after</p></body></html>";
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        uint8_t *order_pdf = NULL;
        size_t order_pdf_size = 0u;
        uint8_t *order_docx = NULL;
        size_t order_docx_size = 0u;

        CHECK(qw_convert_to_pdf_memory(order_html, strlen(order_html), &opts,
                                       &order_pdf, &order_pdf_size,
                                       NULL) == QW_STATUS_OK);
        opts.pdf_office_reflow = 1;
        CHECK(qw_convert_to_format_memory(order_pdf, order_pdf_size,
                                          QW_FORMAT_DOCX, &opts, &order_docx,
                                          &order_docx_size, NULL) ==
              QW_STATUS_OK);
        {
            const uint8_t *before = find_bytes(order_docx, order_docx_size, "before");
            const uint8_t *drawing = find_bytes(order_docx, order_docx_size, "<w:drawing>");
            const uint8_t *after = find_bytes(order_docx, order_docx_size, "after");
            CHECK(before != NULL);
            CHECK(drawing != NULL);
            CHECK(after != NULL);
            CHECK(before < drawing);
            CHECK(drawing < after);
        }
        {
            uint8_t *order_epub = NULL;
            size_t order_epub_size = 0u;
            const uint8_t *before;
            const uint8_t *image;
            const uint8_t *after;
            CHECK(qw_convert_to_format_memory(order_pdf, order_pdf_size,
                                              QW_FORMAT_EPUB, &opts,
                                              &order_epub, &order_epub_size,
                                              NULL) == QW_STATUS_OK);
            before = find_bytes(order_epub, order_epub_size, "before");
            image = find_bytes(order_epub, order_epub_size, "<img src=");
            after = find_bytes(order_epub, order_epub_size, "after");
            CHECK(before != NULL);
            CHECK(image != NULL);
            CHECK(after != NULL);
            CHECK(before < image);
            CHECK(image < after);
            free(order_epub);
        }
        free(order_pdf);
        free(order_docx);
    }

    /* CMYK images decode to RGB and embed as PNG media. */
    {
        FILE *file = fopen(QUIREWISE_CMYK_PDF, "rb");
        long length;
        uint8_t *bytes = NULL;
        size_t byte_count = 0u;
        QwConvertOptions opts = QW_CONVERT_OPTIONS_INIT;
        QwConvertReport report;
        uint8_t *docx = NULL;
        size_t docx_size = 0u;

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        bytes = (uint8_t *)malloc((size_t)length);
        CHECK(bytes != NULL);
        byte_count = fread(bytes, 1u, (size_t)length, file);
        fclose(file);
        CHECK(byte_count == (size_t)length);
        opts.pdf_office_reflow = 1;
        memset(&report, 0, sizeof(report));
        CHECK(qw_convert_to_format_memory(bytes, byte_count, QW_FORMAT_DOCX,
                                          &opts, &docx, &docx_size,
                                          &report) == QW_STATUS_OK);
        CHECK(strstr(report.note, "image(s) embedded") != NULL);
        {
            static const uint8_t signature[8] = {0x89u, 'P', 'N', 'G',
                                                 0x0Du, 0x0Au, 0x1Au, 0x0Au};
            const uint8_t *png = NULL;
            size_t probe;
            for (probe = 0u; probe + 8u <= docx_size; ++probe) {
                if (memcmp(docx + probe, signature, 8u) == 0) {
                    png = docx + probe;
                    break;
                }
            }
            CHECK(png != NULL);
            if (png) {
                const uint8_t *end = NULL;
                for (probe = (size_t)(png - docx); probe + 8u <= docx_size;
                     ++probe) {
                    if (memcmp(docx + probe, "IEND", 4u) == 0) {
                        end = docx + probe + 8u;
                        break;
                    }
                }
                CHECK(end != NULL);
                if (end) {
                    QwImage decoded;
                    memset(&decoded, 0, sizeof(decoded));
                    CHECK(qw_image_decode(png, (size_t)(end - png),
                                          &decoded) == QW_IMAGE_OK);
                    CHECK(decoded.width == 1u);
                    CHECK(decoded.height == 1u);
                    CHECK(decoded.pixels != NULL);
                    if (decoded.pixels && decoded.format == QW_IMAGE_PIXEL_RGB24) {
                        CHECK(decoded.pixels[0] > 200u);
                        CHECK(decoded.pixels[1] < 60u);
                        CHECK(decoded.pixels[2] < 60u);
                    }
                    qw_image_free(&decoded);
                }
            }
        }
        free(bytes);
        free(docx);
    }

    puts("PASS: PDF -> DOCX/EPUB reflow mode");
    return 0;
}
