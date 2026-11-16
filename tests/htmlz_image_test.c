/* HTMLZ relative images are inlined as data URIs. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_HTMLZ_IMAGE
#error "QUIREWISE_HTMLZ_IMAGE must be defined"
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
    uint8_t *archive;
    size_t archive_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    archive = read_file(QUIREWISE_HTMLZ_IMAGE, &archive_size);
    if (!archive || !archive_size) {
        fputs("FAIL: cannot read the HTMLZ image fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(archive, archive_size, "htmlz", &html,
                                   &html_size, &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: HTMLZ to HTML (%s)\n", report.note);
        free(archive);
        return 1;
    }
    free(archive);
    if (!contains(html, html_size, "src=\"data:image/png;base64,") ||
        !contains(html, html_size, "alt=\"pic\"") ||
        !contains(html, html_size, "Before image.") ||
        !contains(html, html_size, "After image.") ||
        !contains((const uint8_t *)report.note, strlen(report.note),
                  "1 package image(s) inlined")) {
        fprintf(stderr, "FAIL: HTMLZ image was not inlined; note=%s\n%.*s\n",
                report.note, (int)(html_size > 300u ? 300u : html_size),
                (const char *)html);
        free(html);
        return 1;
    }
    free(html);
    puts("Quirewise HTMLZ image tests passed");
    return 0;
}
