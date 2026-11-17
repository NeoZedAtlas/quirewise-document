/* ODP text box rotation maps to a CSS transform. */

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_ODP_TEXT_ROTATION_FIXTURE
#error "QUIREWISE_ODP_TEXT_ROTATION_FIXTURE must be defined"
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

int main(void)
{
    uint8_t *odp;
    size_t odp_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    odp = read_file(QUIREWISE_ODP_TEXT_ROTATION_FIXTURE, &odp_size);
    if (!odp || !odp_size) {
        fputs("FAIL: cannot read the rotated text ODP fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(odp, odp_size, QW_FORMAT_HTML, NULL,
                                    &html, &html_size,
                                    &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: ODP text rotation to HTML (%s)\n", report.note);
        free(odp);
        return 1;
    }
    free(odp);
    if (!contains(html, html_size, "class=\"textbox\"") ||
        !contains(html, html_size, "transform:rotate(90deg)") ||
        !contains(html, html_size, "Text after image")) {
        free(html);
        fputs("FAIL: ODP text box rotation was not preserved\n", stderr);
        return 1;
    }
    free(html);
    puts("Quirewise ODP text rotation tests passed");
    return 0;
}
