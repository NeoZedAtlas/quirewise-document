/* EML attachment content: text inline, images as data URIs.
 *
 * The fixture is a multipart message with a plain-text body, a text/plain
 * attachment and a red PNG attachment. HTML shows both payloads; PDF embeds
 * the image XObject and paints it.
 */

#include <quirewise/convert.h>
#include <quirewise/email.h>
#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_EML_ATTACHMENT_FIXTURE
#error "QUIREWISE_EML_ATTACHMENT_FIXTURE must be defined"
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
    uint8_t *eml = NULL;
    size_t eml_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_EML_ATTACHMENT_FIXTURE, &eml, &eml_size) || !eml ||
        !eml_size) {
        fputs("FAIL: cannot read the EML fixture\n", stderr);
        return 1;
    }

    {
        QwEmailDocument *document = NULL;
        const uint8_t *html;
        size_t html_size = 0u;

        if (qw_email_parse(eml, eml_size, &document) != QW_EMAIL_OK ||
            !document) {
            fputs("FAIL: EML parse failed\n", stderr);
            free(eml);
            return 1;
        }
        html = qw_email_document_html(document, &html_size);
        if (!html || !html_size || !contains(html, html_size, "Body text") ||
            !contains(html, html_size, "Attached note text") ||
            !contains(html, html_size, "data:image/png;base64") ||
            !contains(html, html_size, "attachment-text")) {
            fputs("FAIL: EML attachment content is missing\n", stderr);
            qw_email_document_free(document);
            free(eml);
            return 1;
        }
        qw_email_document_free(document);
    }

    {
        uint8_t *pdf = NULL;
        size_t pdf_size = 0u;
        QwPdfDocument *document = NULL;
        size_t pages = 0u;
        QwPageImage page;
        int red = 0;

        memset(&report, 0, sizeof(report));
        if (qw_convert_to_format_memory(eml, eml_size, QW_FORMAT_PDF, NULL,
                                        &pdf, &pdf_size, &report) !=
                QW_STATUS_OK ||
            !pdf || pdf_size < 8u)
            {
            fprintf(stderr, "FAIL: EML to PDF (%s)\n", report.note);
            free(eml);
            return 1;
        }
        if (!contains(pdf, pdf_size, "/Subtype /Image")) {
            fputs("FAIL: EML PDF has no image XObject\n", stderr);
            free(pdf);
            free(eml);
            return 1;
        }
        memset(&page, 0, sizeof(page));
        if (qw_pdf_open(pdf, pdf_size, &document, &pages, NULL) !=
                QW_STATUS_OK ||
            !document || pages < 1u) {
            fputs("FAIL: EML PDF does not open\n", stderr);
            free(pdf);
            free(eml);
            return 1;
        }
        if (qw_pdf_render_page(document, 0u, 144u, &page, NULL) !=
                QW_STATUS_OK ||
            !page.data) {
            qw_pdf_close(document);
            free(pdf);
            free(eml);
            fputs("FAIL: EML PDF page does not render\n", stderr);
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
            free(eml);
            fputs("FAIL: EML attachment image was not painted\n", stderr);
            return 1;
        }
    }

    free(eml);
    puts("Quirewise EML attachment tests passed");
    return 0;
}
