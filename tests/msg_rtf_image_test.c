/* MSG compressed RTF pictures become embedded PNG data URIs. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_RTF_IMAGE_MSG
#error "QUIREWISE_RTF_IMAGE_MSG must be defined"
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

static int base64_value(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') return (int)(ch - 'A');
    if (ch >= 'a' && ch <= 'z') return (int)(ch - 'a') + 26;
    if (ch >= '0' && ch <= '9') return (int)(ch - '0') + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static uint8_t *base64_decode(const char *text, size_t size, size_t *out_size)
{
    uint8_t *out = (uint8_t *)malloc((size / 4u + 1u) * 3u);
    size_t written = 0u;
    unsigned int value = 0u;
    int bits = 0;
    size_t index;
    if (!out) return NULL;
    for (index = 0u; index < size; ++index) {
        int digit;
        if (text[index] == '=') break;
        digit = base64_value((unsigned char)text[index]);
        if (digit < 0) continue;
        value = (value << 6) | (unsigned int)digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[written++] = (uint8_t)((value >> bits) & 0xffu);
        }
    }
    *out_size = written;
    return out;
}

int main(void)
{
    uint8_t *msg = NULL;
    size_t msg_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    QwConvertReport report;

    if (!read_file(QUIREWISE_RTF_IMAGE_MSG, &msg, &msg_size) || !msg ||
        !msg_size) {
        fputs("FAIL: cannot read the RTF image MSG fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(msg, msg_size, "msg", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: MSG to HTML (%s)\n", report.note);
        free(msg);
        return 1;
    }
    free(msg);
    if (!contains(html, html_size, "Before image.") ||
        !contains(html, html_size, "After image.") ||
        !contains(html, html_size, "data:image/png;base64,")) {
        fputs("FAIL: RTF picture was not embedded\n", stderr);
        free(html);
        return 1;
    }
    {
        size_t index;
        const char *marker = "base64,";
        const char *start = NULL;
        const char *end;
        for (index = 0u; index + strlen(marker) <= html_size; ++index) {
            if (memcmp(html + index, marker, strlen(marker)) == 0) {
                start = (const char *)html + index + strlen(marker);
                break;
            }
        }
        if (!start) {
            free(html);
            fputs("FAIL: image payload is missing\n", stderr);
            return 1;
        }
        end = start;
        while ((size_t)(end - (const char *)html) < html_size && *end &&
               *end != '"')
            ++end;
        {
            size_t png_size = 0u;
            uint8_t *png = base64_decode(start, (size_t)(end - start),
                                         &png_size);
            static const uint8_t signature[8] = {0x89u, 'P', 'N', 'G', 0x0du,
                                                 0x0au, 0x1au, 0x0au};
            unsigned width = 0u;
            unsigned height = 0u;
            int valid = png && png_size >= 24u &&
                        memcmp(png, signature, 8u) == 0 &&
                        memcmp(png + 12u, "IHDR", 4u) == 0;
            if (valid) {
                width = ((unsigned)png[16] << 24) | ((unsigned)png[17] << 16) |
                        ((unsigned)png[18] << 8) | png[19];
                height = ((unsigned)png[20] << 24) | ((unsigned)png[21] << 16) |
                         ((unsigned)png[22] << 8) | png[23];
            }
            if (!valid || width != 40u || height != 20u) {
                fprintf(stderr, "FAIL: expected a 40x20 PNG, got %ux%u\n",
                        width, height);
                free(png);
                free(html);
                return 1;
            }
            free(png);
        }
    }
    free(html);
    puts("Quirewise MSG compressed RTF image tests passed");
    return 0;
}
