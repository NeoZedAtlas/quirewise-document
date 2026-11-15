/* Layout-preserving PDF -> DOCX/EPUB tests.
 *
 * Build a small PDF from HTML, convert it to DOCX and EPUB through the memory
 * entry point (the PDF is detected by its magic), then verify that the page
 * image is embedded and that the package still parses.
 */

#include <quirewise/convert.h>
#include <quirewise/docx.h>
#include <quirewise/epub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int contains(const unsigned char *data, size_t size,
                    const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static void fail(const char *message, const QwConvertReport *report)
{
    fprintf(stderr, "FAIL: %s (%s)\n", message,
            report && report->note[0] ? report->note : "no note");
    exit(1);
}

int main(void)
{
    static const char html[] =
        "<html><body><h1>Layout page</h1><p>This text is rendered into the "
        "page image.</p></body></html>";
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    uint8_t *docx = NULL;
    size_t docx_size = 0u;
    uint8_t *epub = NULL;
    size_t epub_size = 0u;
    QwConvertReport report;

    memset(&report, 0, sizeof(report));
    if (qw_convert_to_pdf_memory(html, sizeof(html) - 1u, NULL, &pdf, &pdf_size,
                                 &report) != QW_STATUS_OK ||
        !pdf || !pdf_size)
        fail("cannot build the source PDF", &report);
    if (memcmp(pdf, "%PDF-", 5u) != 0) fail("source is not a PDF", &report);

    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_DOCX, NULL, &docx,
                                    &docx_size, &report) != QW_STATUS_OK ||
        !docx || !docx_size)
        fail("PDF to DOCX failed", &report);
    if (!contains(docx, docx_size, "word/media/page1.jpg") ||
        !contains(docx, docx_size, "w:drawing"))
        fail("DOCX has no embedded page image", &report);
    {
        QwDocxDocument *document = NULL;
        if (qw_docx_parse(docx, docx_size, &document) != QW_DOCX_OK ||
            !document)
            fail("image DOCX did not parse", &report);
        qw_docx_document_free(document);
    }
    free(docx);

    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(pdf, pdf_size, QW_FORMAT_EPUB, NULL, &epub,
                                    &epub_size, &report) != QW_STATUS_OK ||
        !epub || !epub_size)
        fail("PDF to EPUB failed", &report);
    if (!contains(epub, epub_size, "OEBPS/page1.jpg") ||
        !contains(epub, epub_size, "chapter2.xhtml"))
        fail("EPUB has no embedded page image or text chapter", &report);
    {
        QwEpubDocument *document = NULL;
        if (qw_epub_parse(epub, epub_size, &document) != QW_EPUB_OK ||
            !document)
            fail("image EPUB did not parse", &report);
        qw_epub_document_free(document);
    }
    free(epub);

    free(pdf);
    puts("Quirewise PDF office image tests passed");
    return 0;
}
