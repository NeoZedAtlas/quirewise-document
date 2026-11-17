/* PDF -> DOCX/EPUB: layout-preserving page images vs editable text mode. */
#include <quirewise/convert.h>
#include <quirewise/pdf.h>

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

    puts("PASS: PDF -> DOCX/EPUB reflow mode");
    return 0;
}
