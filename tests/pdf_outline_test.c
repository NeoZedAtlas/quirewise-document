/* PDF outline entries are exposed with titles, page indices and depth. */

#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_OUTLINE_PDF
#error "QUIREWISE_OUTLINE_PDF must be defined"
#endif

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
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
    uint8_t *pdf;
    size_t pdf_size = 0u;
    QwPdfDocument *document = NULL;
    size_t pages = 0u;
    char title[128];
    size_t page_index = 99u;
    int depth = 99;

    pdf = read_file(QUIREWISE_OUTLINE_PDF, &pdf_size);
    if (!pdf || !pdf_size) {
        fputs("FAIL: cannot read the outline PDF fixture\n", stderr);
        return 1;
    }
    if (qw_pdf_open(pdf, pdf_size, &document, &pages, NULL) != QW_STATUS_OK ||
        !document || pages != 1u) {
        free(pdf);
        fputs("FAIL: outline PDF did not open\n", stderr);
        return 1;
    }
    if (qw_pdf_bookmark_count(document) != 2u) {
        fprintf(stderr, "FAIL: bookmark count %zu\n",
                qw_pdf_bookmark_count(document));
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    if (qw_pdf_bookmark_at(document, 0u, title, sizeof(title), &page_index,
                           &depth) != QW_STATUS_OK ||
        strcmp(title, "Chapter One") != 0 || page_index != 0u || depth != 0) {
        fprintf(stderr, "FAIL: first bookmark '%s' page=%zu depth=%d\n", title,
                page_index, depth);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    if (qw_pdf_bookmark_at(document, 1u, title, sizeof(title), &page_index,
                           &depth) != QW_STATUS_OK ||
        strcmp(title, "第二章") != 0 || page_index != 0u) {
        fprintf(stderr, "FAIL: second bookmark '%s' page=%zu\n", title,
                page_index);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    if (qw_pdf_bookmark_at(document, 2u, title, sizeof(title), NULL, NULL) !=
        QW_STATUS_ERR_INVALID_ARGUMENT) {
        fputs("FAIL: out-of-range bookmark index was accepted\n", stderr);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    qw_pdf_close(document);
    free(pdf);
    puts("Quirewise PDF outline tests passed");
    return 0;
}
