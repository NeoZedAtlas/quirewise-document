/* XPS -> PDF: FixedPage Glyphs text reaches the PDF text layer.
 *
 * The fixture package has one page with two glyph runs. The unified memory
 * conversion must detect the package, build layout HTML, render a PDF, and the
 * extracted text must contain both runs.
 */

#include <quirewise/convert.h>
#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_XPS_FIXTURE
#error "QUIREWISE_XPS_FIXTURE must be defined"
#endif
#ifndef QUIREWISE_XPS_IMAGE_FIXTURE
#error "QUIREWISE_XPS_IMAGE_FIXTURE must be defined"
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

int main(void)
{
    uint8_t *xps = NULL;
    size_t xps_size = 0u;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_XPS_FIXTURE, &xps, &xps_size) || !xps ||
        !xps_size) {
        fputs("FAIL: cannot read the XPS fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(xps, xps_size, QW_FORMAT_PDF, NULL, &pdf,
                                    &pdf_size, &report) != QW_STATUS_OK ||
        !pdf || pdf_size < 8u) {
        fprintf(stderr, "FAIL: XPS to PDF (%s)\n", report.note);
        free(xps);
        return 1;
    }
    free(xps);
    if (memcmp(pdf, "%PDF-", 5u) != 0) {
        fputs("FAIL: XPS output is not a PDF\n", stderr);
        free(pdf);
        return 1;
    }
    {
        QwPdfDocument *document = NULL;
        uint8_t *text = NULL;
        size_t text_size = 0u;

        if (qw_pdf_open(pdf, pdf_size, &document, NULL, NULL) !=
            QW_STATUS_OK) {
            fputs("FAIL: XPS PDF does not open\n", stderr);
            free(pdf);
            return 1;
        }
        if (qw_pdf_extract_text(document, &text, &text_size, NULL) != QW_STATUS_OK ||
            !text || !text_size) {
            fputs("FAIL: XPS PDF text extraction failed\n", stderr);
            qw_pdf_close(document);
            free(pdf);
            return 1;
        }
        if (!contains(text, text_size, "XPS page text") ||
            !contains(text, text_size, "Second line")) {
            fprintf(stderr, "FAIL: XPS PDF text missing (%.100s)\n",
                    (const char *)text);
            qw_pdf_free(text);
            qw_pdf_close(document);
            free(pdf);
            return 1;
        }
        qw_pdf_free(text);
        qw_pdf_close(document);
    }
    free(pdf);

    /* ImageBrush pages embed the resource as a PNG data URI and paint it. */
    {
        uint8_t *image_xps = NULL;
        size_t image_xps_size = 0u;
        uint8_t *image_pdf = NULL;
        size_t image_pdf_size = 0u;
        QwPdfDocument *document = NULL;
        size_t pages = 0u;
        QwPageImage page;
        int red = 0;

        if (!read_file(QUIREWISE_XPS_IMAGE_FIXTURE, &image_xps,
                       &image_xps_size) ||
            !image_xps || !image_xps_size) {
            fputs("FAIL: cannot read the XPS image fixture\n", stderr);
            return 1;
        }
        memset(&report, 0, sizeof(report));
        if (qw_convert_to_format_memory(image_xps, image_xps_size,
                                        QW_FORMAT_PDF, NULL, &image_pdf,
                                        &image_pdf_size, &report) !=
                QW_STATUS_OK ||
            !image_pdf || image_pdf_size < 8u) {
            fprintf(stderr, "FAIL: XPS image to PDF (%s)\n", report.note);
            free(image_xps);
            return 1;
        }
        free(image_xps);
        if (!contains(image_pdf, image_pdf_size, "/Subtype /Image")) {
            fputs("FAIL: XPS PDF has no image XObject\n", stderr);
            free(image_pdf);
            return 1;
        }
        memset(&page, 0, sizeof(page));
        if (qw_pdf_open(image_pdf, image_pdf_size, &document, &pages, NULL) !=
                QW_STATUS_OK ||
            !document || pages < 1u ||
            qw_pdf_render_page(document, 0u, 144u, &page, NULL) !=
                QW_STATUS_OK ||
            !page.data) {
            fputs("FAIL: XPS image PDF does not render\n", stderr);
            qw_pdf_close(document);
            free(image_pdf);
            return 1;
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
        free(image_pdf);
        if (!red) {
            fputs("FAIL: XPS image was not painted\n", stderr);
            return 1;
        }
    }    puts("Quirewise XPS to PDF tests passed");
    return 0;
}
