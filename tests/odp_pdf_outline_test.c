/* ODP headings flow into the PDF outline with nesting. */

#include "document_backend.h"

#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_OUTLINE_ODP
#error "QUIREWISE_OUTLINE_ODP must be defined"
#endif

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1u, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)size;
    return data;
}

int main(void)
{
    uint8_t *odp;
    size_t odp_size = 0u;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwPdfDocument *document = NULL;
    size_t page_count = 0u;
    QwConvertReport report;
    char title[128];
    size_t page_index = 0u;
    int depth = 0;

    odp = read_file(QUIREWISE_OUTLINE_ODP, &odp_size);
    if (!odp || !odp_size) {
        fputs("FAIL: cannot read the ODP outline fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_memory(odp, odp_size, ".odp", 0, &pdf, &pdf_size,
                              &report) != QW_STATUS_OK ||
        !pdf || !pdf_size) {
        fprintf(stderr, "FAIL: ODP headings to PDF (%s)\n", report.note);
        free(odp);
        return 1;
    }
    free(odp);
    if (qw_pdf_open(pdf, pdf_size, &document, &page_count, &report) !=
            QW_STATUS_OK ||
        !document) {
        free(pdf);
        fprintf(stderr, "FAIL: ODP outline PDF did not parse (%s)\n",
                report.note);
        return 1;
    }
    if (qw_pdf_bookmark_count(document) != 2u) {
        qw_pdf_close(document);
        free(pdf);
        fputs("FAIL: ODP PDF outline count is wrong\n", stderr);
        return 1;
    }
    memset(title, 0, sizeof(title));
    if (qw_pdf_bookmark_at(document, 0u, title, sizeof(title), &page_index,
                           &depth) != QW_STATUS_OK ||
        strstr(title, "Slide 1") == NULL || strstr(title, "Intro") == NULL || depth != 0) {
        fprintf(stderr, "FAIL: ODP outline first entry [%s] depth=%d\n",
                title, depth);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    memset(title, 0, sizeof(title));
    if (qw_pdf_bookmark_at(document, 1u, title, sizeof(title), &page_index,
                           &depth) != QW_STATUS_OK ||
        strstr(title, "Slide 2") == NULL || strstr(title, "Details") == NULL || depth != 0) {
        fprintf(stderr, "FAIL: ODP outline second entry [%s] depth=%d\n",
                title, depth);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    qw_pdf_close(document);
    free(pdf);
    puts("Quirewise ODP PDF outline tests passed");
    return 0;
}
