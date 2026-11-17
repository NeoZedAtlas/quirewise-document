/* Embedding a caller-supplied TrueType font as the PDF text font. */
#include "document_backend.h"
#include "pdf_font.h"

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

#ifndef QUIREWISE_OTF_TEST_PATH
#error "QUIREWISE_OTF_TEST_PATH must name the test OpenType font"
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

    /* The subset itself must be a structurally valid sfnt. */
    {
        QwPdfFont *original = qw_pdf_font_load(QUIREWISE_FONT_TEST_PATH);
        QwPdfFont *reloaded = NULL;
        QwPdfFontSubset subset;
        uint16_t gid;
        unsigned int codepoint = 'F';
        FILE *file;
        const char *subset_path = "quirewise_subset_font.ttf";
        CHECK(original != NULL);
        gid = qw_pdf_font_gid(original, 'F');
        CHECK(gid != 0u);
        memset(&subset, 0, sizeof(subset));
        CHECK(qw_pdf_font_subset(original, &gid, &codepoint, 1u, &subset) == 1);
        CHECK(subset.data != NULL && subset.size > 0u);
        CHECK(subset.size < 13052u);
        CHECK(subset.map[gid] != 0xFFFFu);
        file = fopen(subset_path, "wb");
        CHECK(file != NULL);
        CHECK(fwrite(subset.data, 1u, subset.size, file) == subset.size);
        fclose(file);
        reloaded = qw_pdf_font_load(subset_path);
        CHECK(reloaded != NULL);
        CHECK(qw_pdf_font_gid(reloaded, 'F') == subset.map[gid]);
        qw_pdf_font_free(reloaded);
        remove(subset_path);
        qw_pdf_font_subset_free(&subset);
        qw_pdf_font_free(original);
    }

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

    /* OpenType/CFF embeds as CIDFontType0 with FontFile3/OpenType. */
    {
        static const char *otf_html =
            "<html><body><h1>Filee 123</h1></body></html>";
        uint8_t *otf_pdf = NULL;
        size_t otf_size = 0u;
        memset(&report, 0, sizeof(report));
        CHECK(qw_doc_convert_html_with_font(
                  otf_html, strlen(otf_html), 595.0f, 842.0f,
                  QUIREWISE_OTF_TEST_PATH, &otf_pdf, &otf_size,
                  &report) == QW_STATUS_OK);
        CHECK(otf_pdf != NULL && otf_size > 500u);
        CHECK(contains(otf_pdf, otf_size, "/CIDFontType0"));
        CHECK(contains(otf_pdf, otf_size, "/FontFile3"));
        CHECK(contains(otf_pdf, otf_size, "/Subtype /OpenType"));
        CHECK(!contains(otf_pdf, otf_size, "/FontFile2"));
        {
            QwPdfDocument *document = NULL;
            size_t pages = 0u;
            uint8_t *text = NULL;
            size_t text_size = 0u;
            CHECK(qw_pdf_open(otf_pdf, otf_size, &document, &pages, NULL) ==
                  QW_STATUS_OK);
            CHECK(qw_pdf_extract_text(document, &text, &text_size, NULL) ==
                  QW_STATUS_OK);
            CHECK(contains(text, text_size, "Filee 123"));
            qw_pdf_free(text);
            qw_pdf_close(document);
        }
        free(otf_pdf);
    }

    puts("PASS: PDF font embedding (TrueType subset + OpenType/CFF)");
    return 0;
}
