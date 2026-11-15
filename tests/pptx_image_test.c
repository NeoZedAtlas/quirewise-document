/* PPTX image extraction: p:pic -> slide rels -> media part -> data URI.
 *
 * The fixture is one slide with a red 2x2 PNG stretched to 2 inches and a
 * title. HTML must embed the image as a data URI (not "[image omitted]") and
 * the PDF route must embed and paint an image XObject.
 */

#include <quirewise/convert.h>
#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_PPTX_IMAGE_FIXTURE
#error "QUIREWISE_PPTX_IMAGE_FIXTURE must be defined"
#endif

static int read_file(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;

    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(file);
        return 0;
    }
    if (fread(data, 1u, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = data;
    *out_size = (size_t)size;
    return 1;
}

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

static void fail(const char *message, const QwConvertReport *report)
{
    fprintf(stderr, "FAIL: %s (%s)\n", message,
            report && report->note[0] ? report->note : "no note");
    exit(1);
}

int main(void)
{
    uint8_t *pptx = NULL;
    size_t pptx_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_PPTX_IMAGE_FIXTURE, &pptx, &pptx_size) ||
        !pptx || !pptx_size)
        fail("cannot read the PPTX fixture", NULL);

    /* HTML keeps the picture as a data URI instead of an omitted marker. */
    {
        uint8_t *html = NULL;
        size_t html_size = 0u;

        memset(&report, 0, sizeof(report));
        if (qw_convert_to_format_memory(pptx, pptx_size, QW_FORMAT_HTML, NULL,
                                        &html, &html_size, &report) !=
                QW_STATUS_OK ||
            !html || !html_size)
            fail("PPTX to HTML failed", &report);
        if (!contains(html, html_size, "<img") ||
            !contains(html, html_size, "data:image/png;base64,"))
            fail("PPTX HTML has no embedded picture", &report);
        qw_buffer_free(html);
    }

    /* PDF carries an image XObject and paints red pixels at 144 dpi. */
    {
        uint8_t *pdf = NULL;
        size_t pdf_size = 0u;
        QwPdfDocument *document = NULL;
        size_t pages = 0u;
        QwPageImage page;
        int red = 0;

        memset(&report, 0, sizeof(report));
        if (qw_convert_to_format_memory(pptx, pptx_size, QW_FORMAT_PDF, NULL,
                                        &pdf, &pdf_size, &report) !=
                QW_STATUS_OK ||
            !pdf || pdf_size < 8u)
            fail("PPTX to PDF failed", &report);
        if (!contains(pdf, pdf_size, "/Subtype /Image"))
            fail("PPTX PDF has no image XObject", &report);
        memset(&page, 0, sizeof(page));
        if (qw_pdf_open(pdf, pdf_size, &document, &pages, NULL) !=
                QW_STATUS_OK ||
            !document || pages < 1u)
            fail("PPTX PDF does not open", &report);
        if (qw_pdf_render_page(document, 0u, 144u, &page, NULL) !=
                QW_STATUS_OK ||
            !page.data) {
            qw_pdf_close(document);
            free(pdf);
            fail("PPTX PDF page does not render", &report);
        }
        {
            uint32_t row;
            for (row = 0u; row < page.height && !red; ++row) {
                uint32_t column;
                for (column = 0u; column < page.width; ++column) {
                    const uint8_t *pixel =
                        page.data + (size_t)row * (size_t)page.width * 3u +
                        (size_t)column * 3u;
                    if (pixel[0] > 150u && pixel[1] < 100u &&
                        pixel[2] < 100u) {
                        red = 1;
                        break;
                    }
                }
            }
        }
        qw_pdf_page_free(&page);
        qw_pdf_close(document);
        free(pdf);
        if (!red) fail("PPTX picture was not painted into the PDF", &report);
    }

    free(pptx);
    puts("Quirewise PPTX image tests passed");
    return 0;
}
