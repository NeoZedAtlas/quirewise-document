/* MSG compressed RTF tables become HTML tables. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_RTF_TABLE_MSG
#error "QUIREWISE_RTF_TABLE_MSG must be defined"
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
    uint8_t *msg = NULL;
    size_t msg_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_RTF_TABLE_MSG, &msg, &msg_size) || !msg ||
        !msg_size) {
        fputs("FAIL: cannot read the RTF table MSG fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(msg, msg_size, "msg", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: MSG to HTML (%s)\n", report.note);
        free(msg);
        return 1;
    }
    free(msg);
    if (!contains(html, html_size, "<table>") ||
        !contains(html, html_size, "<td>A</td>") ||
        !contains(html, html_size, "<td>B</td>") ||
        !contains(html, html_size, "<td>C</td>") ||
        !contains(html, html_size, "<td>D</td>") ||
        !contains(html, html_size, "</table>") ||
        contains(html, html_size, "trowd") ||
        contains(html, html_size, "cellx")) {
        fputs("FAIL: compressed RTF table was not rendered\n", stderr);
        free(html);
        return 1;
    }
    free(html);
    puts("Quirewise MSG compressed RTF table tests passed");
    return 0;
}
