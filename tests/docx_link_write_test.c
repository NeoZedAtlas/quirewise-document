/* DOCX writer keeps safe hyperlinks as Word hyperlink relationships. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_LINK_ODT_SRC
#error "QUIREWISE_LINK_ODT_SRC must be defined"
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
    uint8_t *docx = NULL;
    size_t docx_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_LINK_ODT_SRC, &odt, &odt_size) || !odt ||
        !odt_size) {
        fputs("FAIL: cannot read the link ODT fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_docx_memory(odt, odt_size, "odt", &docx, &docx_size,
                                   &report) != QW_STATUS_OK ||
        !docx || docx_size <= 8u) {
        fprintf(stderr, "FAIL: ODT to DOCX (%s)\n", report.note);
        free(odt);
        return 1;
    }
    free(odt);
    if (!contains(docx, docx_size, "w:hyperlink") ||
        !contains(docx, docx_size, "relationships/hyperlink") ||
        !contains(docx, docx_size, "TargetMode=\"External\"") ||
        !contains(docx, docx_size, "https://example.com") ||
        contains(docx, docx_size, "javascript:")) {
        fputs("FAIL: DOCX hyperlink relationships are missing or unsafe\n",
              stderr);
        free(docx);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(docx, docx_size, "docx", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: written DOCX read-back (%s)\n", report.note);
        free(docx);
        return 1;
    }
    free(docx);
    if (!contains(html, html_size,
                  "<a href=\"https://example.com\">Quirewise link</a>")) {
        fputs("FAIL: hyperlink did not survive the DOCX round trip\n",
              stderr);
        free(html);
        return 1;
    }
    free(html);
    puts("Quirewise DOCX hyperlink write tests passed");
    return 0;
}
