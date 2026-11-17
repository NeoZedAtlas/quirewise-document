/* Embedding a caller-supplied TrueType font as the PDF text font. */
#include "document_backend.h"

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

#ifndef QUIREWISE_FONT_TEST_PATH
#error "QUIREWISE_FONT_TEST_PATH must name the test TrueType font"
#endif

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
    QwConvertReport report;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    int rc;

    memset(&report, 0, sizeof(report));
    rc = qw_doc_convert_html_with_font(html, strlen(html), 595.0f, 842.0f,
                                       QUIREWISE_FONT_TEST_PATH, &pdf, &pdf_size,
                                       &report);
    CHECK(rc == QW_STATUS_OK);
    CHECK(pdf != NULL && pdf_size > 500u);
    CHECK(memcmp(pdf, "%PDF-1.4", 8u) == 0);
    CHECK(contains(pdf, pdf_size, "/FontFile2"));
    CHECK(contains(pdf, pdf_size, "/Subtype /Type0"));
    CHECK(contains(pdf, pdf_size, "/Encoding /Identity-H"));
    CHECK(contains(pdf, pdf_size, "/ToUnicode"));
    /* Page resources must reference only the embedded font. */
    CHECK(contains(pdf, pdf_size, "/Font << /F3"));
    CHECK(!contains(pdf, pdf_size, "/Font << /F1"));

    /* The ToUnicode map must give the reader back the original text. */
    {
        QwPdfDocument *document = NULL;
        size_t pages = 0u;
        uint8_t *text = NULL;
        size_t text_size = 0u;
        CHECK(qw_pdf_open(pdf, pdf_size, &document, &pages, NULL) == QW_STATUS_OK);
        CHECK(pages == 1u);
        CHECK(qw_pdf_extract_text(document, &text, &text_size, NULL) ==
              QW_STATUS_OK);
        CHECK(contains(text, text_size, "Filee test 123"));
        CHECK(contains(text, text_size, "a b e f g"));
        qw_pdf_free(text);
        qw_pdf_close(document);
    }
    free(pdf);

    /* A missing or non-TrueType font is reported, not silently ignored. */
    pdf = NULL;
    pdf_size = 0u;
    memset(&report, 0, sizeof(report));
    rc = qw_doc_convert_html_with_font(html, strlen(html), 595.0f, 842.0f,
                                       "Z:/quirewise/no-such-font.ttf", &pdf,
                                       &pdf_size, &report);
    CHECK(rc != QW_STATUS_OK);
    CHECK(pdf == NULL);
    CHECK(strstr(report.note, "Cannot embed font") != NULL);

    /* The default route keeps the predefined Helvetica/STSong-Light pair. */
    pdf = NULL;
    pdf_size = 0u;
    CHECK(qw_doc_convert_html(html, strlen(html), &pdf, &pdf_size, NULL) ==
          QW_STATUS_OK);
    CHECK(contains(pdf, pdf_size, "/STSong-Light"));
    CHECK(!contains(pdf, pdf_size, "/FontFile2"));
    free(pdf);

    /* A dedicated route (RTF) must embed the font as well. */
    {
        static const char *rtf = "{\\rtf1\\ansi Filee test 123\\par}";
        QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
        uint8_t *rtf_pdf = NULL;
        size_t rtf_size = 0u;
        options.pdf_font_path = QUIREWISE_FONT_TEST_PATH;
        memset(&report, 0, sizeof(report));
        CHECK(qw_doc_convert_memory_with_options(
                  (const uint8_t *)rtf, strlen(rtf), "rtf", 0, &options,
                  &rtf_pdf, &rtf_size, &report) == QW_STATUS_OK);
        CHECK(rtf_pdf != NULL && rtf_size > 500u);
        CHECK(contains(rtf_pdf, rtf_size, "/FontFile2"));
        CHECK(contains(rtf_pdf, rtf_size, "/Identity-H"));
        free(rtf_pdf);
    }

    puts("PASS: PDF font embedding (CIDFontType2 + Identity-H + ToUnicode)");
    return 0;
}
