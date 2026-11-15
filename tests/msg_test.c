/* Outlook MSG: compound-file properties become layout HTML, PDF and Markdown. */

#include "document_backend.h"

#include <quirewise/convert.h>
#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_MSG_FIXTURE
#error "QUIREWISE_MSG_FIXTURE must be defined"
#endif

static int read_file(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(file); return 0; }
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
    uint8_t *msg = NULL;
    size_t msg_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwConvertReport report;
    QwPdfDocument *document = NULL;
    uint8_t *text = NULL;
    size_t text_size = 0u;

    if (!read_file(QUIREWISE_MSG_FIXTURE, &msg, &msg_size) || !msg ||
        !msg_size) {
        fputs("FAIL: cannot read the MSG fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(msg, msg_size, "msg", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: MSG to HTML (%s)\n", report.note);
        free(msg);
        return 1;
    }
    if (!contains(html, html_size, "Outlook MSG sample") ||
        !contains(html, html_size, "Outlook HTML body") ||
        contains(html, html_size, "Hello from an Outlook message") ||
        !contains(html, html_size, "Sample Sender") ||
        !contains(html, html_size, "note.txt") ||
        !contains(html, html_size, "Attachments")) {
        fputs("FAIL: MSG HTML is missing properties or attachments\n",
              stderr);
        free(html);
        free(msg);
        return 1;
    }
    free(html);
    html = NULL;

    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(msg, msg_size, QW_FORMAT_PDF, NULL, &pdf,
                                    &pdf_size, &report) != QW_STATUS_OK ||
        !pdf || pdf_size < 8u) {
        fprintf(stderr, "FAIL: MSG to PDF (%s)\n", report.note);
        free(msg);
        return 1;
    }
    free(msg);
    if (qw_pdf_open(pdf, pdf_size, &document, NULL, NULL) != QW_STATUS_OK ||
        !document ||
        qw_pdf_extract_text(document, &text, &text_size, NULL) !=
            QW_STATUS_OK ||
        !text || !contains(text, text_size, "Outlook MSG sample") ||
        !contains(text, text_size, "Outlook HTML body")) {
        fputs("FAIL: MSG PDF text is missing the message body\n", stderr);
        qw_pdf_free(text);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    qw_pdf_free(text);
    qw_pdf_close(document);
    free(pdf);
    puts("Quirewise MSG tests passed");
    return 0;
}
