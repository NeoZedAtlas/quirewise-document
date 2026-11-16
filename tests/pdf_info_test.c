/* PDF information dictionary strings decode to UTF-8. */

#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_INFO_PDF
#error "QUIREWISE_INFO_PDF must be defined"
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
    char title[64];
    char author[64];

    pdf = read_file(QUIREWISE_INFO_PDF, &pdf_size);
    if (!pdf || !pdf_size) {
        fputs("FAIL: cannot read the PDF fixture\n", stderr);
        return 1;
    }
    if (qw_pdf_open(pdf, pdf_size, &document, &pages, NULL) != QW_STATUS_OK ||
        !document || pages != 1u) {
        free(pdf);
        fputs("FAIL: PDF did not open\n", stderr);
        return 1;
    }
    if (qw_pdf_document_title(document, title, sizeof(title)) != QW_STATUS_OK ||
        strcmp(title, "中文") != 0) {
        fprintf(stderr, "FAIL: title is '%s'\n", title);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    if (qw_pdf_document_author(document, author, sizeof(author)) !=
            QW_STATUS_OK ||
        strcmp(author, "Alice Author") != 0) {
        fprintf(stderr, "FAIL: author is '%s'\n", author);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    qw_pdf_close(document);
    free(pdf);
    puts("Quirewise PDF information tests passed");
    return 0;
}
