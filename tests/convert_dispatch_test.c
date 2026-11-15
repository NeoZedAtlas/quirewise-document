#include "quirewise/convert.h"
#include "document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static const char *FODT =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" office:version=\"1.2\" "
    "office:mimetype=\"application/vnd.oasis.opendocument.text\">"
    "<office:body><office:text><text:h text:outline-level=\"1\">Dispatch</text:h>"
    "<text:p>Flat ODT through the public conversion API.</text:p>"
    "</office:text></office:body></office:document>";

static const char *HTML =
    "<html><body><h1>Dispatch HTML</h1><p>Rendered by the internal writer.</p></body></html>";

static int is_pdf(const uint8_t *data, size_t size)
{
    return data && size > 8u && memcmp(data, "%PDF-", 5u) == 0;
}

static int contains_bytes(const uint8_t *data, size_t size, const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (!data || !length || length > size) return 0;
    for (index = 0u; index + length <= size; ++index) {
        if (memcmp(data + index, text, length) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    QwConvertOptions options = QW_CONVERT_OPTIONS_INIT;
    QwConvertReport report;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;

    /* SVG is XML but belongs to the vector route: the document package must
     * decline it instead of reporting a broken OpenDocument file. */
    {
        static const char svg[] =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\"/>";
        QwConvertReport svg_report;
        memset(&svg_report, 0, sizeof(svg_report));
        CHECK(qw_convert_to_pdf_memory(svg, sizeof(svg) - 1u, &options, &pdf,
                                       &pdf_size, &svg_report) ==
              QW_STATUS_ERR_UNSUPPORTED);
        CHECK(pdf == NULL && pdf_size == 0u);
        CHECK(strstr(svg_report.note, "SVG") != NULL);
    }

    /* RTF is routed through the text reader and the layout writer. */
    {
        static const char rtf[] =
            "{\\rtf1\\ansi\\deff0 {\\fonttbl {\\f0 Times New Roman;}}"
            "\\f0\\fs24 Hello from the RTF dispatch route.\\par}";
        uint8_t *text = NULL;
        size_t text_size = 0u;
        int rc = qw_convert_to_pdf_memory(rtf, sizeof(rtf) - 1u, &options, &pdf,
                                          &pdf_size, &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(is_pdf(pdf, pdf_size));
        rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_TXT, &options,
                                         &text, &text_size, &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(text_size > 0u);
        CHECK(strstr((const char *)text, "RTF dispatch route") != NULL);
        qw_buffer_free(text);
        free(pdf);
        pdf = NULL;
        pdf_size = 0u;
    }

    /* PDF raster targets beyond PNG, plus the text-backed HTML/Markdown
     * memory routes. */
    {
        static const char html[] =
            "<html><body><h1>PDF raster dispatch</h1><p>body</p></body></html>";
        uint8_t *output = NULL;
        size_t output_size = 0u;
        int rc = qw_convert_to_pdf_memory(html, sizeof(html) - 1u, &options,
                                          &pdf, &pdf_size, &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(is_pdf(pdf, pdf_size));
        rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_JPG,
                                         &options, &output, &output_size,
                                         &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(output && output_size > 2u && output[0] == 0xffu &&
              output[1] == 0xd8u);
        free(output);
        output = NULL;
        output_size = 0u;
        rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_HTML,
                                         &options, &output, &output_size,
                                         &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(output && strstr((const char *)output, "PDF raster dispatch") !=
                            NULL);
        qw_buffer_free(output);
        output = NULL;
        output_size = 0u;
        rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_MARKDOWN,
                                         &options, &output, &output_size,
                                         &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(output && strstr((const char *)output, "PDF raster dispatch") !=
                            NULL);
        qw_buffer_free(output);
        free(pdf);
        pdf = NULL;
        pdf_size = 0u;
    }

    /* EML is routed through the email reader and the layout writer. */
    {
        static const char eml[] =
            "From: sender@example.com\r\n"
            "To: receiver@example.com\r\n"
            "Subject: Dispatch EML\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "\r\n"
            "Hello from the EML dispatch route.\r\n";
        uint8_t *text = NULL;
        size_t text_size = 0u;
        int rc = qw_convert_to_pdf_memory(eml, sizeof(eml) - 1u, &options, &pdf,
                                          &pdf_size, &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(is_pdf(pdf, pdf_size));
        rc = qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_TXT, &options,
                                         &text, &text_size, &report);
        CHECK(rc == QW_STATUS_OK);
        CHECK(text_size > 0u);
        CHECK(strstr((const char *)text, "EML dispatch route") != NULL);
        qw_buffer_free(text);
        free(pdf);
        pdf = NULL;
        pdf_size = 0u;
    }

    memset(&report, 0, sizeof(report));
    CHECK(qw_convert_to_pdf_memory(FODT, strlen(FODT), &options, &pdf, &pdf_size,
                                   &report) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;

    CHECK(qw_convert_to_pdf_memory(HTML, strlen(HTML), &options, &pdf, &pdf_size,
                                   NULL) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;

    /* A PDF is passed through unchanged. */
    {
        static const char small_pdf[] = "%PDF-1.4\n%%EOF\n";
        size_t expected = sizeof(small_pdf) - 1u;
        CHECK(qw_convert_to_pdf_memory(small_pdf, expected, &options, &pdf,
                                       &pdf_size, NULL) == QW_STATUS_OK);
        CHECK(pdf_size == expected && memcmp(pdf, small_pdf, expected) == 0);
        free(pdf);
        pdf = NULL;
    }

    /* Unknown binary input is rejected with a defined status. */
    {
        static const uint8_t junk[] = {0x01u, 0x02u, 0x03u, 0x04u};
        QwConvertReport junk_report;
        memset(&junk_report, 0, sizeof(junk_report));
        CHECK(qw_convert_to_pdf_memory(junk, sizeof(junk), &options, &pdf, &pdf_size,
                                       &junk_report) == QW_STATUS_ERR_UNSUPPORTED);
        CHECK(pdf == NULL && pdf_size == 0u);
        CHECK(junk_report.note[0] != '\0');
    }

    /* Non-OpenDocument XML is rejected, not silently rendered. */
    {
        static const char other_xml[] =
            "<?xml version=\"1.0\"?><root><child/></root>";
        CHECK(qw_convert_to_pdf_memory(other_xml, sizeof(other_xml) - 1u, &options,
                                       &pdf, &pdf_size, NULL) ==
              QW_STATUS_ERR_FORMAT);
        CHECK(pdf == NULL);
    }

    /* A ZIP container that is neither DOCX nor ODT is reported as unsupported,
     * not as a broken OpenDocument package. */
    {
        static const uint8_t zip_like[] = {'P', 'K', 0x03u, 0x04u, 0u, 0u, 0u, 0u};
        QwConvertReport zip_report;
        memset(&zip_report, 0, sizeof(zip_report));
        CHECK(qw_convert_to_pdf_memory(zip_like, sizeof(zip_like), &options, &pdf,
                                       &pdf_size, &zip_report) ==
              QW_STATUS_ERR_UNSUPPORTED);
        CHECK(pdf == NULL && pdf_size == 0u);
        CHECK(zip_report.note[0] != '\0');
    }

    /* The historical convert-memory entry renders ODT/FODT/HTML internally. */
    CHECK(qw_doc_convert_memory((const uint8_t *)FODT, strlen(FODT), ".fodt", 0,
                                  &pdf, &pdf_size, &report) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;
    CHECK(qw_doc_convert_memory((const uint8_t *)FODT, strlen(FODT), NULL, 0,
                                  &pdf, &pdf_size, NULL) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;
    CHECK(qw_doc_convert_memory((const uint8_t *)HTML, strlen(HTML), ".html", 0,
                                  &pdf, &pdf_size, NULL) == QW_STATUS_OK);
    CHECK(is_pdf(pdf, pdf_size));
    free(pdf);
    pdf = NULL;
    {
        static const char small_pdf[] = "%PDF-1.4\n%%EOF\n";
        size_t expected = sizeof(small_pdf) - 1u;
        CHECK(qw_doc_convert_memory((const uint8_t *)small_pdf, expected, ".pdf",
                                      0, &pdf, &pdf_size, NULL) == QW_STATUS_OK);
        CHECK(pdf_size == expected);
        free(pdf);
        pdf = NULL;
    }
    {
        static const uint8_t junk[] = {0x01u, 0x02u, 0x03u, 0x04u};
        CHECK(qw_doc_convert_memory(junk, sizeof(junk), ".bin", 0, &pdf,
                                      &pdf_size, NULL) == QW_STATUS_ERR_UNSUPPORTED);
        pdf = NULL;
        CHECK(qw_doc_convert_memory((const uint8_t *)FODT, strlen(FODT), ".fodt",
                                      1, &pdf, &pdf_size, NULL) == QW_STATUS_OK);
        CHECK(is_pdf(pdf, pdf_size));
        free(pdf);
        pdf = NULL;
    }

    /* The memory entry point must honour page_mode instead of discarding the
     * options (image inputs used to ignore --page and always use the image
     * box). */
    {
        static const uint8_t bmp_1x1[] = {
            'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
            40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0,
            0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        };
        QwConvertOptions a4 = QW_CONVERT_OPTIONS_INIT;
        QwConvertOptions image = QW_CONVERT_OPTIONS_INIT;
        a4.page_mode = QW_RASTER_PAGE_MODE_A4;
        image.page_mode = QW_RASTER_PAGE_MODE_IMAGE;

        CHECK(qw_convert_to_pdf_memory(bmp_1x1, sizeof(bmp_1x1), &a4, &pdf,
                                       &pdf_size, &report) == QW_STATUS_OK);
        CHECK(is_pdf(pdf, pdf_size));
        CHECK(contains_bytes(pdf, pdf_size, "/MediaBox [0 0 595.276 841.89]"));
        free(pdf);
        pdf = NULL;
        pdf_size = 0u;

        CHECK(qw_convert_to_pdf_memory(bmp_1x1, sizeof(bmp_1x1), &image, &pdf,
                                       &pdf_size, &report) == QW_STATUS_OK);
        CHECK(is_pdf(pdf, pdf_size));
        CHECK(contains_bytes(pdf, pdf_size, "/MediaBox [0 0 1 1]"));
        free(pdf);
        pdf = NULL;
        pdf_size = 0u;
    }

    /* Broken spreadsheet/RTF inputs keep their FORMAT classification (the
     * adapters used to flatten everything to FORMAT, so this also guards the
     * status-preservation path). */
    {
        static const uint8_t broken_cfb[512] = {
            0xD0u, 0xCFu, 0x11u, 0xE0u, 0xA1u, 0xB1u, 0x1Au, 0xE1u
        };
        QwConvertReport xls_report;
        memset(&xls_report, 0, sizeof(xls_report));
        CHECK(qw_doc_convert_memory(broken_cfb, sizeof(broken_cfb), "xls", 0,
                                      &pdf, &pdf_size, &xls_report) ==
              QW_STATUS_ERR_FORMAT);
        CHECK(pdf == NULL && pdf_size == 0u);
        CHECK(xls_report.note[0] != '\0');
    }
    {
        static const char broken_rtf[] =
            "{\\rtf1\\ansi {\\fonttbl {\\f0 Arial;}} \\f0 text";
        QwConvertReport rtf_report;
        memset(&rtf_report, 0, sizeof(rtf_report));
        CHECK(qw_doc_convert_memory((const uint8_t *)broken_rtf,
                                      strlen(broken_rtf), "rtf", 0, &pdf,
                                      &pdf_size, &rtf_report) ==
              QW_STATUS_ERR_FORMAT);
        CHECK(pdf == NULL && pdf_size == 0u);
        CHECK(rtf_report.note[0] != '\0');
    }

    CHECK(qw_convert_to_pdf_memory(NULL, 0u, &options, &pdf, &pdf_size, NULL) ==
          QW_STATUS_ERR_INVALID_ARGUMENT);
    puts("PASS: public PDF conversion dispatch (FODT, HTML, PDF passthrough, rejections)");
    return 0;
}
