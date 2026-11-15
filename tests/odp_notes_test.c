/* ODP speaker notes: presentation:notes text appended to the slide section. */

#include <quirewise/convert.h>
#include <quirewise/pdf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_ODP_NOTES_FIXTURE
#error "QUIREWISE_ODP_NOTES_FIXTURE must be defined"
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
    uint8_t *odp = NULL;
    size_t odp_size = 0u;
    uint8_t *pdf = NULL;
    size_t pdf_size = 0u;
    QwConvertReport report;
    QwPdfDocument *document = NULL;
    uint8_t *text = NULL;
    size_t text_size = 0u;

    if (!read_file(QUIREWISE_ODP_NOTES_FIXTURE, &odp, &odp_size) || !odp ||
        !odp_size) {
        fputs("FAIL: cannot read the ODP fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(odp, odp_size, QW_FORMAT_PDF, NULL, &pdf,
                                    &pdf_size, &report) != QW_STATUS_OK ||
        !pdf || pdf_size < 8u) {
        fprintf(stderr, "FAIL: ODP to PDF (%s)\n", report.note);
        free(odp);
        return 1;
    }
    free(odp);
    if (qw_pdf_open(pdf, pdf_size, &document, NULL, NULL) != QW_STATUS_OK ||
        !document) {
        fputs("FAIL: ODP PDF does not open\n", stderr);
        free(pdf);
        return 1;
    }
    if (qw_pdf_extract_text(document, &text, &text_size, NULL) !=
            QW_STATUS_OK ||
        !text || !text_size ||
        !contains(text, text_size, "Slide body text") ||
        !contains(text, text_size, "Speaker notes")) {
        fputs("FAIL: ODP notes text missing from the PDF\n", stderr);
        qw_pdf_free(text);
        qw_pdf_close(document);
        free(pdf);
        return 1;
    }
    qw_pdf_free(text);
    qw_pdf_close(document);
    free(pdf);
    puts("Quirewise ODP notes tests passed");
    return 0;
}
