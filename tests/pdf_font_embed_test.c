/* Embedding a caller-supplied TrueType font as the PDF text font. */
#include "document_backend.h"
#include "pdf_cff.h"
#include "pdf_font.h"

#include <quirewise/compression.h>
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

    /* PDF/A-1b structure: XMP metadata, sRGB output intent, embedded font. */
    {
        QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
        uint8_t *pdfa_pdf = NULL;
        size_t pdfa_size = 0u;
        options.pdf_font_path = QUIREWISE_FONT_TEST_PATH;
        options.pdfa_level = QW_PDFA_1B;
        memset(&report, 0, sizeof(report));
        CHECK(qw_convert_to_pdf_memory(html, strlen(html), &options, &pdfa_pdf,
                                       &pdfa_size, &report) == QW_STATUS_OK);
        CHECK(contains(pdfa_pdf, pdfa_size, "/GTS_PDFA1"));
        CHECK(contains(pdfa_pdf, pdfa_size, "/OutputIntent"));
        CHECK(contains(pdfa_pdf, pdfa_size, "pdfaid:part"));
        CHECK(contains(pdfa_pdf, pdfa_size, "pdfaid:conformance"));
        CHECK(contains(pdfa_pdf, pdfa_size, "/DestOutputProfile"));
        CHECK(contains(pdfa_pdf, pdfa_size, "/FontFile2"));
        CHECK(contains(pdfa_pdf, pdfa_size, "<pdfaid:part>1</pdfaid:part>"));
        /* veraPDF (ISO 19005-1 6.1.3) requires a trailer /ID, and every font
           object must be embedded: the Helvetica/STSong-Light slots stay null
           when the embedded font carries the text. */
        CHECK(contains(pdfa_pdf, pdfa_size, "/ID [<"));
        CHECK(!contains(pdfa_pdf, pdfa_size, "/BaseFont /Helvetica"));
        CHECK(!contains(pdfa_pdf, pdfa_size, "/BaseFont /STSong-Light"));
        free(pdfa_pdf);

        /* PDF/A-2b and 3b keep the same structure with another part id. */
        options.pdfa_level = QW_PDFA_2B;
        pdfa_pdf = NULL;
        pdfa_size = 0u;
        memset(&report, 0, sizeof(report));
        CHECK(qw_convert_to_pdf_memory(html, strlen(html), &options, &pdfa_pdf,
                                       &pdfa_size, &report) == QW_STATUS_OK);
        CHECK(contains(pdfa_pdf, pdfa_size, "<pdfaid:part>2</pdfaid:part>"));
        CHECK(contains(pdfa_pdf, pdfa_size, "/GTS_PDFA1"));
        free(pdfa_pdf);
        options.pdfa_level = QW_PDFA_3B;
        pdfa_pdf = NULL;
        pdfa_size = 0u;
        memset(&report, 0, sizeof(report));
        CHECK(qw_convert_to_pdf_memory(html, strlen(html), &options, &pdfa_pdf,
                                       &pdfa_size, &report) == QW_STATUS_OK);
        CHECK(contains(pdfa_pdf, pdfa_size, "<pdfaid:part>3</pdfaid:part>"));
        free(pdfa_pdf);

        /* Without an embedded font the writer refuses PDF/A. */
        options.pdfa_level = QW_PDFA_1B;
        options.pdf_font_path = NULL;
        pdfa_pdf = NULL;
        pdfa_size = 0u;
        memset(&report, 0, sizeof(report));
        CHECK(qw_convert_to_pdf_memory(html, strlen(html), &options, &pdfa_pdf,
                                       &pdfa_size, &report) != QW_STATUS_OK);
        CHECK(pdfa_pdf == NULL);
        CHECK(strstr(report.note, "PDF/A") != NULL);
    }

    /* Characters outside the font are reported, not silently dropped. */
    {
        static const char *gap_html = "<html><body><p>z</p></body></html>";
        uint8_t *gap_pdf = NULL;
        size_t gap_size = 0u;
        memset(&report, 0, sizeof(report));
        CHECK(qw_doc_convert_html_with_font(
                  gap_html, strlen(gap_html), 595.0f, 842.0f,
                  QUIREWISE_FONT_TEST_PATH, &gap_pdf, &gap_size,
                  &report) == QW_STATUS_OK);
        CHECK(strstr(report.note, "not covered") != NULL);
        free(gap_pdf);
    }

    /* Non-ASCII text without an embedded font carries a warning. */
    {
        static const char zh[] = "<p>\xE4\xB8\xAD\xE6\x96\x87</p>";
        QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
        uint8_t *zh_pdf = NULL;
        size_t zh_size = 0u;
        QwConvertReport note;
        memset(&note, 0, sizeof(note));
        CHECK(qw_convert_to_pdf_memory(zh, strlen(zh), &options, &zh_pdf,
                                       &zh_size, &note) == QW_STATUS_OK);
        CHECK(strstr(note.note, "without embedding") != NULL);
        free(zh_pdf);
        options.pdf_font_path = QUIREWISE_FONT_TEST_PATH;
        zh_pdf = NULL;
        zh_size = 0u;
        memset(&note, 0, sizeof(note));
        CHECK(qw_convert_to_pdf_memory(zh, strlen(zh), &options, &zh_pdf,
                                       &zh_size, &note) == QW_STATUS_OK);
        CHECK(strstr(note.note, "without embedding") == NULL);
        free(zh_pdf);
    }

    /* CFF blanking keeps the file size but shrinks unused glyph data. */
    {
        FILE *file = fopen(QUIREWISE_OTF_TEST_PATH, "rb");
        long length;
        uint8_t *original = NULL;
        uint8_t *blanked = NULL;
        size_t size = 0u;
        size_t bound;
        uint8_t *plain_zip = NULL;
        uint8_t *blank_zip = NULL;
        size_t plain_size = 0u;
        size_t blank_size = 0u;
        uint16_t gids[1] = {1u};

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        original = (uint8_t *)malloc((size_t)length);
        blanked = (uint8_t *)malloc((size_t)length);
        CHECK(original != NULL);
        CHECK(blanked != NULL);
        size = fread(original, 1u, (size_t)length, file);
        fclose(file);
        memcpy(blanked, original, size);
        CHECK(qw_pdf_cff_blank_unused(blanked, size, gids, 1u) == 1);
        CHECK(memcmp(original, blanked, size) != 0);
        bound = qw_compression_zlib_bound(size);
        plain_zip = (uint8_t *)malloc(bound);
        blank_zip = (uint8_t *)malloc(bound);
        CHECK(plain_zip != NULL);
        CHECK(blank_zip != NULL);
        CHECK(qw_compression_deflate_zlib(original, size, plain_zip, bound,
                                          &plain_size));
        CHECK(qw_compression_deflate_zlib(blanked, size, blank_zip, bound,
                                          &blank_size));
        CHECK(blank_size < plain_size);
        free(original);
        free(blanked);
        free(plain_zip);
        free(blank_zip);
    }

    /* True CFF subsetting rebuilds CharStrings, charset and FDSelect. */
    {
        FILE *file = fopen(QUIREWISE_OTF_TEST_PATH, "rb");
        long length;
        uint8_t *data = NULL;
        size_t size = 0u;
        uint16_t gids[2] = {1u, 2u};
        uint16_t map[256];
        uint8_t *subset = NULL;
        size_t subset_size = 0u;
        unsigned glyph_count = 0u;

        CHECK(file != NULL);
        CHECK(fseek(file, 0, SEEK_END) == 0);
        length = ftell(file);
        CHECK(length > 0);
        CHECK(fseek(file, 0, SEEK_SET) == 0);
        data = (uint8_t *)malloc((size_t)length);
        CHECK(data != NULL);
        size = fread(data, 1u, (size_t)length, file);
        fclose(file);
        CHECK(size == (size_t)length);
        CHECK(qw_pdf_cff_subset(data, size, gids, 2u, &subset, &subset_size,
                                map, &glyph_count) == 1);
        CHECK(subset != NULL && subset_size > 0u);
        CHECK(subset_size < size);
        CHECK(glyph_count >= 3u);
        CHECK(map[0] == 0u);
        CHECK(map[1] == 1u);
        CHECK(map[2] == 2u);
        CHECK(map[3] == 0xFFFFu);
        file = fopen(".tmp-cff-subset.otf", "wb");
        CHECK(file != NULL);
        CHECK(fwrite(subset, 1u, subset_size, file) == subset_size);
        fclose(file);
        {
            QwPdfFont *font = qw_pdf_font_load(".tmp-cff-subset.otf");
            CHECK(font != NULL);
            qw_pdf_font_free(font);
        }
        remove(".tmp-cff-subset.otf");
        free(subset);
        free(data);
    }

    /* A static CFF2 font (no variation store) is rewritten as CFF 1 and
       embeds as a CIDFontType0; a CFF2 font that cannot be flattened keeps the
       actionable rejection. */
    {
        QwPdfFont *font = qw_pdf_font_load(QUIREWISE_CFF2_STATIC_PATH);
        CHECK(font != NULL);
        CHECK(qw_pdf_font_is_cff(font) == 1);
        qw_pdf_font_free(font);
        {
            QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
            uint8_t *pdf = NULL;
            size_t pdf_size = 0u;
            QwConvertReport note;
            memset(&note, 0, sizeof(note));
            options.pdf_font_path = QUIREWISE_CFF2_STATIC_PATH;
            CHECK(qw_convert_to_pdf_memory(html, strlen(html), &options, &pdf,
                                           &pdf_size, &note) == QW_STATUS_OK);
            CHECK(pdf != NULL && pdf_size > 0u);
            CHECK(contains(pdf, pdf_size, "/CIDFontType0"));
            CHECK(contains(pdf, pdf_size, "/FontFile3"));
            free(pdf);
        }
        /* PDF/A-2b accepts the converted CFF program ... */
        {
            QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
            uint8_t *pdf = NULL;
            size_t pdf_size = 0u;
            QwConvertReport note;
            memset(&note, 0, sizeof(note));
            options.pdf_font_path = QUIREWISE_CFF2_STATIC_PATH;
            options.pdfa_level = QW_PDFA_2B;
            CHECK(qw_convert_to_pdf_memory(html, strlen(html), &options, &pdf,
                                           &pdf_size, &note) == QW_STATUS_OK);
            CHECK(contains(pdf, pdf_size, "<pdfaid:part>2</pdfaid:part>"));
            free(pdf);
        }
        /* ... while PDF/A-1b only admits Type 1/TrueType/Type 3 programs. */
        {
            QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
            uint8_t *pdf = NULL;
            size_t pdf_size = 0u;
            QwConvertReport note;
            memset(&note, 0, sizeof(note));
            options.pdf_font_path = QUIREWISE_CFF2_STATIC_PATH;
            options.pdfa_level = QW_PDFA_1B;
            CHECK(qw_convert_to_pdf_memory(html, strlen(html), &options, &pdf,
                                           &pdf_size, &note) ==
                  QW_STATUS_ERR_UNSUPPORTED);
            CHECK(pdf == NULL);
            CHECK(strstr(note.note, "PDF/A-1b") != NULL);
            CHECK(strstr(note.note, "TrueType") != NULL);
        }
    }

    /* CFF2 variable-font outlines cannot be embedded in PDF 1.x: the loader
       must say so instead of failing on a missing CFF table. */
    {
        const char *reason = NULL;
        QwPdfFont *font = qw_pdf_font_load_ex(QUIREWISE_CFF2_TEST_PATH, &reason);
        CHECK(font == NULL);
        CHECK(reason != NULL);
        CHECK(strstr(reason, "CFF2") != NULL);
        {
            QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
            uint8_t *pdf = NULL;
            size_t pdf_size = 0u;
            options.pdf_font_path = QUIREWISE_CFF2_TEST_PATH;
            memset(&report, 0, sizeof(report));
            CHECK(qw_convert_to_pdf_memory(html, strlen(html), &options, &pdf,
                                           &pdf_size, &report) ==
                  QW_STATUS_ERR_UNSUPPORTED);
            CHECK(pdf == NULL);
            CHECK(strstr(report.note, "CFF2") != NULL);
        }
    }

    puts("PASS: PDF font embedding (TrueType subset + OpenType/CFF)");
    return 0;
}
