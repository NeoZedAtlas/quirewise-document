/* HTML title and author metadata reach the generated PDF /Info. */

#include "document_backend.h"

#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char html[] =
    "<!doctype html><html><head><title>Quarterly Report</title>"
    "<meta name=\"author\" content=\"Ada Lovelace\"/></head>"
    "<body><h1>Summary</h1><p>Body text</p></body></html>";


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
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwPdfDocument *document = NULL;
    size_t page_count = 0u;
    QwConvertReport report;
    char title[128];
    char author[128];

    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html(html, strlen(html), &pdf, &pdf_size, &report) !=
            QW_STATUS_OK ||
        !pdf || !pdf_size) {
        fprintf(stderr, "FAIL: HTML to PDF (%s)\n", report.note);
        return 1;
    }
    if (!contains(pdf, pdf_size, "/Title (Quarterly Report)") ||
        !contains(pdf, pdf_size, "/Author (Ada Lovelace)") ||
        !contains(pdf, pdf_size, "/Info ")) {
        free(pdf);
        fputs("FAIL: PDF Info dictionary is missing\n", stderr);
        return 1;
    }
    if (qw_pdf_open(pdf, pdf_size, &document, &page_count, &report) !=
            QW_STATUS_OK ||
        !document) {
        free(pdf);
        fprintf(stderr, "FAIL: generated PDF did not parse (%s)\n",
                report.note);
        return 1;
    }
    memset(title, 0, sizeof(title));
    memset(author, 0, sizeof(author));
    if (qw_pdf_document_title(document, title, sizeof(title)) != QW_STATUS_OK ||
        strcmp(title, "Quarterly Report") != 0) {
        fprintf(stderr, "FAIL: PDF title is [%s]\n", title);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    if (qw_pdf_document_author(document, author, sizeof(author)) != QW_STATUS_OK ||
        strcmp(author, "Ada Lovelace") != 0) {
        fprintf(stderr, "FAIL: PDF author is [%s]\n", author);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    qw_pdf_close(document);
    free(pdf);
    puts("Quirewise PDF metadata tests passed");
    return 0;
}
