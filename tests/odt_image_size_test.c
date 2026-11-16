/* ODT image frames carry svg:width/svg:height, which become <img> width and
 * height attributes in the layout HTML. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_SIZED_ODT
#error "QUIREWISE_SIZED_ODT must be defined"
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
    uint8_t *odt = NULL;
    size_t odt_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_SIZED_ODT, &odt, &odt_size) || !odt ||
        !odt_size) {
        fputs("FAIL: cannot read the sized ODT fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(odt, odt_size, "odt", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: ODT to HTML (%s)\n", report.note);
        free(odt);
        return 1;
    }
    free(odt);
    if (!contains(html, html_size, "data:image/png;base64") ||
        !contains(html, html_size, "width=\"113\"") ||
        !contains(html, html_size, "height=\"76\"") ||
        !contains(html, html_size, "Sized image page")) {
        fputs("FAIL: ODT image size attributes are missing\n", stderr);
        free(html);
        return 1;
    }
    free(html);
    puts("Quirewise ODT image size tests passed");
    return 0;
}
