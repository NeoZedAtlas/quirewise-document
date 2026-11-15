/* HWPX BinData image embedding.
 *
 * The fixture section has a text paragraph and one hp:pic referencing an
 * hp:img with binaryItemIDRef="img0"; content.hpf maps img0 to
 * BinData/red.png. HTML must embed the PNG as a data URI and the PDF route
 * must paint it.
 */

#include <quirewise/convert.h>
#include <quirewise/hwpx.h>
#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_HWPX_IMAGE_FIXTURE
#error "QUIREWISE_HWPX_IMAGE_FIXTURE must be defined"
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
    uint8_t *hwpx = NULL;
    size_t hwpx_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_HWPX_IMAGE_FIXTURE, &hwpx, &hwpx_size) || !hwpx ||
        !hwpx_size) {
        fputs("FAIL: cannot read the HWPX fixture\n", stderr);
        return 1;
    }

    {
        QwHwpxDocument *document = NULL;
        const uint8_t *html;
        size_t html_size = 0u;

        if (qw_hwpx_parse(hwpx, hwpx_size, &document) != QW_HWPX_OK ||
            !document) {
            fputs("FAIL: HWPX parse failed\n", stderr);
            free(hwpx);
            return 1;
        }
        html = qw_hwpx_document_html(document, &html_size);
        if (!html || !html_size || !contains(html, html_size, "<img") ||
            !contains(html, html_size, "data:image/png;base64") ||
            !contains(html, html_size, "Image page")) {
            fputs("FAIL: HWPX image content is missing\n", stderr);
            qw_hwpx_document_free(document);
            free(hwpx);
            return 1;
        }
        qw_hwpx_document_free(document);
    }

    {
        uint8_t *pdf = NULL;
        size_t pdf_size = 0u;
        QwPdfDocument *document = NULL;
        size_t pages = 0u;
        QwPageImage page;
        int red = 0;

        memset(&report, 0, sizeof(report));
        if (qw_convert_to_format_memory(hwpx, hwpx_size, QW_FORMAT_PDF, NULL,
                                        &pdf, &pdf_size, &report) !=
                QW_STATUS_OK ||
            !pdf || pdf_size < 8u) {
            fprintf(stderr, "FAIL: HWPX to PDF (%s)\n", report.note);
            free(hwpx);
            return 1;
        }
        free(hwpx);
        if (!contains(pdf, pdf_size, "/Subtype /Image")) {
            fputs("FAIL: HWPX PDF has no image XObject\n", stderr);
            free(pdf);
            return 1;
        }
        memset(&page, 0, sizeof(page));
        if (qw_pdf_open(pdf, pdf_size, &document, &pages, NULL) !=
                QW_STATUS_OK ||
            !document || pages < 1u ||
            qw_pdf_render_page(document, 0u, 144u, &page, NULL) !=
                QW_STATUS_OK ||
            !page.data) {
            fputs("FAIL: HWPX PDF does not render\n", stderr);
            qw_pdf_close(document);
            free(pdf);
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
        free(pdf);
        if (!red) {
            fputs("FAIL: HWPX image was not painted\n", stderr);
            return 1;
        }
    }

    puts("Quirewise HWPX image tests passed");
    return 0;
}
