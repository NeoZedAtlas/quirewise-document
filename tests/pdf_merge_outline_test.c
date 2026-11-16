/* Merging PDFs keeps outline entries from every input with page offsets. */

#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_MERGE_PDF_A
#error "QUIREWISE_MERGE_PDF_A must be defined"
#endif
#ifndef QUIREWISE_MERGE_PDF_B
#error "QUIREWISE_MERGE_PDF_B must be defined"
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
    uint8_t *first = NULL;
    uint8_t *second = NULL;
    size_t first_size = 0u;
    size_t second_size = 0u;
    const uint8_t *inputs[2];
    size_t sizes[2];
    uint8_t *merged = NULL;
    size_t merged_size = 0u;
    size_t merged_pages = 0u;
    QwPdfDocument *document = NULL;
    char title[128];
    size_t page_index = 99u;
    int depth = 0;

    first = read_file(QUIREWISE_MERGE_PDF_A, &first_size);
    second = read_file(QUIREWISE_MERGE_PDF_B, &second_size);
    if (!first || !second) {
        free(first);
        free(second);
        fputs("FAIL: cannot read the PDF fixtures\n", stderr);
        return 1;
    }
    inputs[0] = first;
    inputs[1] = second;
    sizes[0] = first_size;
    sizes[1] = second_size;
    if (qw_pdf_merge(inputs, sizes, 2u, &merged, &merged_size, &merged_pages,
                     NULL) != QW_STATUS_OK ||
        !merged || !merged_size || merged_pages != 2u) {
        free(first);
        free(second);
        fputs("FAIL: PDF merge failed\n", stderr);
        return 1;
    }
    free(first);
    free(second);
    if (qw_pdf_open(merged, merged_size, &document, NULL, NULL) != QW_STATUS_OK ||
        !document) {
        free(merged);
        fputs("FAIL: merged PDF did not open\n", stderr);
        return 1;
    }
    if (qw_pdf_bookmark_count(document) != 2u) {
        fprintf(stderr, "FAIL: merged bookmark count %zu\n",
                qw_pdf_bookmark_count(document));
        qw_pdf_close(document);
        free(merged);
        return 1;
    }
    if (qw_pdf_bookmark_at(document, 0u, title, sizeof(title), &page_index,
                           &depth) != QW_STATUS_OK ||
        strcmp(title, "Chapter One") != 0 || page_index != 1u) {
        fprintf(stderr, "FAIL: merged bookmark '%s' page=%zu\n", title,
                page_index);
        qw_pdf_close(document);
        free(merged);
        return 1;
    }
    if (qw_pdf_bookmark_at(document, 1u, title, sizeof(title), &page_index,
                           &depth) != QW_STATUS_OK ||
        strcmp(title, "第二章") != 0 || page_index != 1u) {
        fprintf(stderr, "FAIL: second merged bookmark '%s' page=%zu\n", title,
                page_index);
        qw_pdf_close(document);
        free(merged);
        return 1;
    }
    qw_pdf_close(document);
    free(merged);
    puts("Quirewise merged PDF outline tests passed");
    return 0;
}
