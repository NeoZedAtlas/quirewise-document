/* ODT header/footer text extraction.
 *
 * The fixture has one body paragraph plus a master-page header and footer in
 * styles.xml. The HTML intermediate must place them around the body text.
 */

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_ODT_HF_FIXTURE
#error "QUIREWISE_ODT_HF_FIXTURE must be defined"
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

static size_t find_text(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (!needle_size || needle_size > size) return 0u;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return index;
    }
    return 0u;
}

int main(void)
{
    uint8_t *odt = NULL;
    size_t odt_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;
    size_t header_at;
    size_t body_at;
    size_t footer_at;

    if (!read_file(QUIREWISE_ODT_HF_FIXTURE, &odt, &odt_size) || !odt ||
        !odt_size) {
        fputs("FAIL: cannot read the ODT fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(odt, odt_size, QW_FORMAT_HTML, NULL,
                                    &html, &html_size, &report) !=
            QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: ODT to HTML (%s)\n", report.note);
        free(odt);
        return 1;
    }
    free(odt);
    if (!contains(html, html_size, "odt-header") ||
        !contains(html, html_size, "odt-footer") ||
        !contains(html, html_size, "Header text") ||
        !contains(html, html_size, "Footer text") ||
        !contains(html, html_size, "Body text")) {
        fputs("FAIL: ODT header/footer text is missing\n", stderr);
        qw_buffer_free(html);
        return 1;
    }
    header_at = find_text(html, html_size, "Header text");
    body_at = find_text(html, html_size, "Body text");
    footer_at = find_text(html, html_size, "Footer text");
    if (!(header_at < body_at && body_at < footer_at)) {
        fputs("FAIL: ODT header/footer order is wrong\n", stderr);
        qw_buffer_free(html);
        return 1;
    }
    qw_buffer_free(html);
    puts("Quirewise ODT header/footer tests passed");
    return 0;
}
