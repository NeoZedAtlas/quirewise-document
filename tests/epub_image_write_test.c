/* EPUB writer extracts data URI images into OEBPS/images and references them. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_EPUB_IMAGE_SRC
#error "QUIREWISE_EPUB_IMAGE_SRC must be defined"
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
    uint8_t *written = NULL;
    size_t written_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_EPUB_IMAGE_SRC, &epub, &epub_size) || !epub ||
        !epub_size) {
        fputs("FAIL: cannot read the EPUB fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_epub_memory(epub, epub_size, "epub", &written,
                                   &written_size, &report) != QW_STATUS_OK ||
        !written || written_size <= 8u) {
        fprintf(stderr, "FAIL: EPUB to EPUB (%s)\n", report.note);
        free(epub);
        return 1;
    }
    free(epub);
    if (!contains(written, written_size, "OEBPS/images/image0.png") ||
        !contains(written, written_size, "src=\"images/image0.png\"") ||
        !contains(written, written_size, "href=\"images/image0.png\"")) {
        fputs("FAIL: EPUB image part or references are missing\n", stderr);
        free(written);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(written, written_size, "epub", &html,
                                   &html_size, &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: written EPUB read-back (%s)\n", report.note);
        free(written);
        return 1;
    }
    free(written);
    if (!contains(html, html_size, "data:image/png;base64") ||
        !contains(html, html_size, "EPUB image page")) {
        fputs("FAIL: image did not survive the EPUB round trip\n", stderr);
        free(html);
        return 1;
    }
    free(html);
    puts("Quirewise EPUB image write tests passed");
    return 0;
}
