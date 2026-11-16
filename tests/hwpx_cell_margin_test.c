/* HWPX table cell margins map to CSS padding. */

#include <quirewise/hwpx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_HWPX_CELL_MARGIN
#error "QUIREWISE_HWPX_CELL_MARGIN must be defined"
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

static int contains(const char *text, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(text + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    uint8_t *hwpx;
    size_t hwpx_size = 0u;
    QwHwpxDocument *document = NULL;
    const uint8_t *html = NULL;
    size_t html_size = 0u;

    hwpx = read_file(QUIREWISE_HWPX_CELL_MARGIN, &hwpx_size);
    if (!hwpx || !hwpx_size) {
        fputs("FAIL: cannot read the HWPX cell margin fixture\n", stderr);
        return 1;
    }
    if (qw_hwpx_parse(hwpx, hwpx_size, &document) != QW_HWPX_OK ||
        !document) {
        free(hwpx);
        fputs("FAIL: HWPX cell margin fixture did not parse\n", stderr);
        return 1;
    }
    html = qw_hwpx_document_html(document, &html_size);
    if (!html || !html_size ||
        !contains((const char *)html, html_size,
                  "<td style=\"padding:4px 8px 4px 8px;\">") ||
        !contains((const char *)html, html_size, "Padded")) {
        qw_hwpx_document_free(document);
        free(hwpx);
        fputs("FAIL: HWPX cell margin was not preserved\n", stderr);
        return 1;
    }
    qw_hwpx_document_free(document);
    free(hwpx);
    puts("Quirewise HWPX cell margin tests passed");
    return 0;
}
