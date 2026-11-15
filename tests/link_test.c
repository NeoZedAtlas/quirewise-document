/* Safe external hyperlinks in ODT and ODP become <a href>, unsafe ones only
 * keep their labels. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_LINK_ODT
#error "QUIREWISE_LINK_ODT must be defined"
#endif
#ifndef QUIREWISE_LINK_ODP
#error "QUIREWISE_LINK_ODP must be defined"
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

static int check_link(const char *path, const char *extension,
                      const char *safe_link, const char *label,
                      const char *blocked_label)
{
    uint8_t *data = NULL;
    size_t data_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;
    int rc;

    if (!read_file(path, &data, &data_size) || !data || !data_size) {
        fprintf(stderr, "FAIL: cannot read %s\n", path);
        return 0;
    }
    memset(&report, 0, sizeof(report));
    rc = qw_doc_convert_html_memory(data, data_size, extension, &html,
                                    &html_size, &report);
    free(data);
    if (rc != QW_STATUS_OK || !html || !html_size) {
        fprintf(stderr, "FAIL: %s to HTML (%s)\n", path, report.note);
        return 0;
    }
    if (!contains(html, html_size, safe_link) ||
        !contains(html, html_size, label) ||
        contains(html, html_size, "javascript:") ||
        !contains(html, html_size, blocked_label)) {
        fprintf(stderr, "FAIL: %s link handling is wrong\n", extension);
        free(html);
        return 0;
    }
    free(html);
    return 1;
}

int main(void)
{
    if (!check_link(QUIREWISE_LINK_ODT, "odt",
                    "<a href=\"https://example.com\">Quirewise link</a>",
                    "Quirewise link", "blocked link")) {
        return 1;
    }
    if (!check_link(QUIREWISE_LINK_ODP, "odp",
                    "<a href=\"https://example.com\">ODP link</a>",
                    "ODP link", "ODP link")) {
        return 1;
    }
    puts("Quirewise link tests passed");
    return 0;
}
