/* PDF -> DOCX/EPUB: layout-preserving page images vs editable text mode. */
#include <quirewise/convert.h>

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
    puts("PASS: PDF -> DOCX/EPUB reflow mode");
    return 0;
}
