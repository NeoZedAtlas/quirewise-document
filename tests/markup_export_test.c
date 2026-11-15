/* Markdown-source exports: IPYNB notebook JSON and OPML outline. */

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    static const char markdown[] =
        "# Top\n"
        "\n"
        "Body line\n"
        "\n"
        "## Child\n"
        "- item\n";
    uint8_t *output = NULL;
    size_t output_size = 0u;
    QwConvertReport report;

    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(markdown, sizeof(markdown) - 1u,
                                    QW_FORMAT_IPYNB, NULL, &output,
                                    &output_size, &report) != QW_STATUS_OK ||
        !output || !output_size) {
        fprintf(stderr, "FAIL: Markdown to IPYNB (%s)\n", report.note);
        return 1;
    }
    if (!contains(output, output_size, "\"nbformat\":4") ||
        !contains(output, output_size, "\"cell_type\":\"markdown\"") ||
        !contains(output, output_size, "\"source\":[") ||
        !contains(output, output_size, "# Top\\n") ||
        !contains(output, output_size, "## Child\\n")) {
        fputs("FAIL: IPYNB structure is missing fields\n", stderr);
        free(output);
        return 1;
    }
    free(output);
    output = NULL;
    output_size = 0u;

    memset(&report, 0, sizeof(report));
    if (qw_convert_to_format_memory(markdown, sizeof(markdown) - 1u,
                                    QW_FORMAT_OPML, NULL, &output,
                                    &output_size, &report) != QW_STATUS_OK ||
        !output || !output_size) {
        fprintf(stderr, "FAIL: Markdown to OPML (%s)\n", report.note);
        return 1;
    }
    if (!contains(output, output_size, "<opml version=\"2.0\">") ||
        !contains(output, output_size, "<outline text=\"Top\">") ||
        !contains(output, output_size, "<outline text=\"Child\">") ||
        !contains(output, output_size, "<outline text=\"- item\"/>") ||
        !contains(output, output_size, "<outline text=\"Body line\"/>") ||
        count_occurrences(output, output_size, "</outline>") != 2u) {
        fputs("FAIL: OPML outline hierarchy is wrong\n", stderr);
        free(output);
        return 1;
    }
    free(output);
    puts("Quirewise Markdown export tests passed");
    return 0;
}
