/* DOCX vertical merge: vMerge restart/continue become <td rowspan="N">. */

#include "document_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_DOCX_MERGE_FIXTURE
#error "QUIREWISE_DOCX_MERGE_FIXTURE must be defined"
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
    uint8_t *docx = NULL;
    size_t docx_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;
    int rc;

    if (!read_file(QUIREWISE_DOCX_MERGE_FIXTURE, &docx, &docx_size) || !docx ||
        !docx_size) {
        fputs("FAIL: cannot read the DOCX fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    rc = qw_doc_convert_html_memory(docx, docx_size, "docx", &html,
                                    &html_size, &report);
    free(docx);
    if (rc != QW_STATUS_OK || !html || !html_size) {
        fprintf(stderr, "FAIL: DOCX to HTML (%s)\n", report.note);
        return 1;
    }
    if (!contains(html, html_size, "<table>") ||
        !contains(html, html_size, "rowspan=\"3\"") ||
        !contains(html, html_size, "Merged cell")) {
        fputs("FAIL: vertical merge rowspan missing\n", stderr);
        free(html);
        return 1;
    }
    free(html);
    puts("Quirewise DOCX vertical merge tests passed");
    return 0;
}
