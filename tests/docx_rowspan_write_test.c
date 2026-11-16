/* DOCX writer maps HTML rowspan to vertical merge cells. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_ROWSPAN_HTML
#error "QUIREWISE_ROWSPAN_HTML must be defined"
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
    uint8_t *html = NULL;
    size_t html_size = 0u;
    uint8_t *docx = NULL;
    size_t docx_size = 0u;
    uint8_t *round = NULL;
    size_t round_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_ROWSPAN_HTML, &html, &html_size) || !html ||
        !html_size) {
        fputs("FAIL: cannot read the rowspan HTML fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_docx_memory(html, html_size, "html", &docx, &docx_size,
                                   &report) != QW_STATUS_OK ||
        !docx || docx_size <= 8u) {
        fprintf(stderr, "FAIL: HTML to DOCX (%s)\n", report.note);
        free(html);
        return 1;
    }
    free(html);
    if (!contains(docx, docx_size, "<w:vMerge w:val=\"restart\"/>") ||
        !contains(docx, docx_size, "<w:vMerge/>") ||
        !contains(docx, docx_size, "Merged")) {
        fputs("FAIL: DOCX vertical merge is missing\n", stderr);
        free(docx);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(docx, docx_size, "docx", &round, &round_size,
                                   &report) != QW_STATUS_OK ||
        !round || !round_size) {
        fprintf(stderr, "FAIL: written DOCX read-back (%s)\n", report.note);
        free(docx);
        return 1;
    }
    free(docx);
    if (!contains(round, round_size, "<td rowspan=\"2\">") ||
        !contains(round, round_size, "Merged") ||
        !contains(round, round_size, "<p>B</p>") ||
        !contains(round, round_size, "<p>C</p>")) {
        fputs("FAIL: rowspan did not survive the DOCX round trip\n", stderr);
        free(round);
        return 1;
    }
    free(round);
    puts("Quirewise DOCX rowspan write tests passed");
    return 0;
}
