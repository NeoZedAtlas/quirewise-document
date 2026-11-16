/* DOCX writer embeds data URI images from the HTML intermediate. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_DOCX_IMAGE_EPUB
#error "QUIREWISE_DOCX_IMAGE_EPUB must be defined"
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
    uint8_t *epub = NULL;
    size_t epub_size = 0u;
    uint8_t *docx = NULL;
    size_t docx_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_DOCX_IMAGE_EPUB, &epub, &epub_size) || !epub ||
        !epub_size) {
        fputs("FAIL: cannot read the EPUB fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_docx_memory(epub, epub_size, "epub", &docx, &docx_size,
                                   &report) != QW_STATUS_OK ||
        !docx || docx_size <= 8u) {
        fprintf(stderr, "FAIL: EPUB to DOCX (%s)\n", report.note);
        free(epub);
        return 1;
    }
    free(epub);
    if (!contains(docx, docx_size, "word/media/image0.png") ||
        !contains(docx, docx_size, "w:drawing") ||
        !contains(docx, docx_size, "rIdImg0")) {
        fputs("FAIL: DOCX image part or drawing is missing\n", stderr);
        free(docx);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(docx, docx_size, "docx", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: DOCX read-back (%s)\n", report.note);
        free(docx);
        return 1;
    }
    free(docx);
    if (!contains(html, html_size, "data:image/png;base64") ||
        !contains(html, html_size, "EPUB image page")) {
        fputs("FAIL: embedded DOCX image did not survive the read-back\n",
              stderr);
        free(html);
        return 1;
    }
    free(html);
    puts("Quirewise DOCX image write tests passed");
    return 0;
}
