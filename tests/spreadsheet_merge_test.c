/* XLSX merged ranges become colspan/rowspan in the layout HTML. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_MERGED_XLSX
#error "QUIREWISE_MERGED_XLSX must be defined"
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

static size_t count_occurrences(const uint8_t *data, size_t size,
                                const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    size_t count = 0u;
    if (!needle_size || needle_size > size) return 0u;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) ++count;
    }
    return count;
}

int main(void)
{
    uint8_t *xlsx = NULL;
    size_t xlsx_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_MERGED_XLSX, &xlsx, &xlsx_size) || !xlsx ||
        !xlsx_size) {
        fputs("FAIL: cannot read the merged XLSX fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(xlsx, xlsx_size, "xlsx", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: XLSX to HTML (%s)\n", report.note);
        free(xlsx);
        return 1;
    }
    free(xlsx);
    if (!contains(html, html_size, " colspan=\"2\" rowspan=\"2\"") ||
        !contains(html, html_size, "Merged title") ||
        !contains(html, html_size, "C1 value") ||
        !contains(html, html_size, "C2 value") ||
        !contains(html, html_size, "After merge") ||
        count_occurrences(html, html_size, "Merged title") != 1u ||
        count_occurrences(html, html_size, "<tr>") != 3u) {
        fputs("FAIL: merged XLSX cells are not rendered as spans\n", stderr);
        free(html);
        return 1;
    }
    free(html);
    puts("Quirewise XLSX merge tests passed");
    return 0;
}
