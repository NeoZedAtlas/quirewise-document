/* XPS page text reaches HTML and Markdown output. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_XPS_SAMPLE
#error "QUIREWISE_XPS_SAMPLE must be defined"
#endif

#ifndef QUIREWISE_XPS_IMAGE_SAMPLE
#error "QUIREWISE_XPS_IMAGE_SAMPLE must be defined"
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
    uint8_t *xps;
    size_t xps_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    uint8_t *markdown = NULL;
    size_t markdown_size = 0u;
    QwConvertReport report;

    xps = read_file(QUIREWISE_XPS_SAMPLE, &xps_size);
    if (!xps || !xps_size) {
        fputs("FAIL: cannot read the XPS fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(xps, xps_size, "xps", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: XPS to HTML (%s)\n", report.note);
        free(xps);
        return 1;
    }
    if (!contains(html, html_size, "<section class=\"xps-page\">") ||
        !contains(html, html_size, "<h2>Page 1</h2>") ||
        !contains(html, html_size, "XPS page text") ||
        !contains(html, html_size, "Second line")) {
        fputs("FAIL: XPS HTML page text was not rendered\n", stderr);
        free(html);
        free(xps);
        return 1;
    }
    free(html);
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_markdown_memory(xps, xps_size, "xps",
                                       &markdown, &markdown_size, &report) !=
            QW_STATUS_OK ||
        !markdown || !markdown_size) {
        fprintf(stderr, "FAIL: XPS to Markdown (%s)\n", report.note);
        free(xps);
        return 1;
    }
    free(xps);
    if (!contains(markdown, markdown_size, "## Page 1") ||
        !contains(markdown, markdown_size, "XPS page text Second line")) {
        fputs("FAIL: XPS Markdown page text was not rendered\n", stderr);
        free(markdown);
        return 1;
    }
    free(markdown);
    {
        uint8_t *image_xps;
        size_t image_xps_size = 0u;
        uint8_t *image_md = NULL;
        size_t image_md_size = 0u;
        image_xps = read_file(QUIREWISE_XPS_IMAGE_SAMPLE, &image_xps_size);
        if (!image_xps || !image_xps_size) {
            fputs("FAIL: cannot read the XPS image fixture\n", stderr);
            return 1;
        }
        memset(&report, 0, sizeof(report));
        if (qw_doc_convert_markdown_memory(image_xps, image_xps_size, "xps",
                                           &image_md, &image_md_size,
                                           &report) != QW_STATUS_OK ||
            !image_md || !image_md_size) {
            fprintf(stderr, "FAIL: XPS image to Markdown (%s)\n",
                    report.note);
            free(image_xps);
            return 1;
        }
        free(image_xps);
        if (!contains(image_md, image_md_size,
                      "![](data:image/png;base64,") ||
            !contains(image_md, image_md_size, "XPS image page")) {
            fputs("FAIL: XPS Markdown image was dropped\n", stderr);
            free(image_md);
            return 1;
        }
        free(image_md);
    }
    puts("Quirewise XPS HTML tests passed");
    return 0;
}
