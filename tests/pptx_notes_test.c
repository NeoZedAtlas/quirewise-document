/* PPTX speaker-notes extraction.
 *
 * The fixture has one slide with a title, body text and notes ("Speaker
 * notes"). The layout HTML must append a notes section with that text.
 */

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_PPTX_NOTES_FIXTURE
#error "QUIREWISE_PPTX_NOTES_FIXTURE must be defined"
#endif

static int read_file(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;

    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(file);
        return 0;
    }
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
    uint8_t *pptx = NULL;
    size_t pptx_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_PPTX_NOTES_FIXTURE, &pptx, &pptx_size) || !pptx ||
        !pptx_size) {
        fputs("FAIL: cannot read the notes PPTX fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(pptx, pptx_size, QW_FORMAT_HTML, NULL,
                                    &html, &html_size, &report) !=
            QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: PPTX to HTML (%s)\n", report.note);
        free(pptx);
        return 1;
    }
    free(pptx);
    if (!contains(html, html_size, "class=\"notes\"") ||
        !contains(html, html_size, "Speaker notes") ||
        !contains(html, html_size, "Notes slide") ||
        !contains(html, html_size, "Body text")) {
        fputs("FAIL: PPTX notes text is missing\n", stderr);
        qw_buffer_free(html);
        return 1;
    }
    qw_buffer_free(html);
    puts("Quirewise PPTX notes tests passed");
    return 0;
}
