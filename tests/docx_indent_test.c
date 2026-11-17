/* DOCX paragraph indentation maps to CSS margins. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_DOCX_INDENT
#error "QUIREWISE_DOCX_INDENT must be defined"
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
    uint8_t *docx;
    size_t docx_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    docx = read_file(QUIREWISE_DOCX_INDENT, &docx_size);
    if (!docx || !docx_size) {
        fputs("FAIL: cannot read the DOCX indent fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(docx, docx_size, "docx", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: DOCX indent to HTML (%s)\n", report.note);
        free(docx);
        return 1;
    }
    free(docx);
    if (!contains(html, html_size,
                  "style=\"margin-left:48px;text-indent:24px;\"") ||
        !contains(html, html_size, "<p>Plain paragraph</p>")) {
        free(html);
        fputs("FAIL: DOCX paragraph indentation was not preserved\n", stderr);
        return 1;
    }
    free(html);
    puts("Quirewise DOCX indent tests passed");
    return 0;
}
