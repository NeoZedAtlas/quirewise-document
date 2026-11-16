/* EPUB figures keep img and figcaption inside a figure element. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_FIGURE_EPUB
#error "QUIREWISE_FIGURE_EPUB must be defined"
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

static int count(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    int total = 0;
    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) ++total;
    }
    return total;
}

int main(void)
{
    uint8_t *epub;
    size_t epub_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    epub = read_file(QUIREWISE_FIGURE_EPUB, &epub_size);
    if (!epub || !epub_size) {
        fputs("FAIL: cannot read the figure EPUB fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(epub, epub_size, "epub", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: figure EPUB to HTML (%s)\n", report.note);
        free(epub);
        return 1;
    }
    free(epub);
    if (!contains(html, html_size, "<figure><img src=\"data:image/png;base64,") ||
        !contains(html, html_size, "<figcaption>Red square</figcaption>") ||
        !contains(html, html_size, "</figure>") ||
        !contains(html, html_size, "Before figure.") ||
        !contains(html, html_size, "After figure.") ||
        count(html, html_size, "<figure>") != 1 ||
        count(html, html_size, "</figure>") != 1) {
        fputs("FAIL: figure markup was not preserved\n", stderr);
        free(html);
        return 1;
    }
    free(html);
    puts("Quirewise EPUB figure tests passed");
    return 0;
}
