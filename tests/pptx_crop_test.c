/* PPTX DrawingML srcRect crops the embedded image before PNG conversion. */

#include "document_backend.h"

#include <quirewise/convert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_CROP_PPTX
#error "QUIREWISE_CROP_PPTX must be defined"
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

static int png_dimensions(const uint8_t *data, size_t size, unsigned *width,
                          unsigned *height)
{
    static const uint8_t signature[8] = {0x89u, 'P', 'N', 'G', 0x0du, 0x0au,
                                         0x1au, 0x0au};
    if (!data || size < 24u || memcmp(data, signature, 8u) != 0) return 0;
    if (memcmp(data + 12u, "IHDR", 4u) != 0) return 0;
    *width = ((unsigned)data[16] << 24) | ((unsigned)data[17] << 16) |
             ((unsigned)data[18] << 8) | data[19];
    *height = ((unsigned)data[20] << 24) | ((unsigned)data[21] << 16) |
              ((unsigned)data[22] << 8) | data[23];
    return 1;
}

int main(void)
{
    uint8_t *pptx = NULL;
    size_t pptx_size = 0u;
    uint8_t *html = NULL;
    size_t html_size = 0u;
    const char *marker = "base64,";
    QwConvertReport report;

    if (!read_file(QUIREWISE_CROP_PPTX, &pptx, &pptx_size) || !pptx ||
        !pptx_size) {
        fputs("FAIL: cannot read the cropped PPTX fixture\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_doc_convert_html_memory(pptx, pptx_size, "pptx", &html, &html_size,
                                   &report) != QW_STATUS_OK ||
        !html || !html_size) {
        fprintf(stderr, "FAIL: PPTX to HTML (%s)\n", report.note);
        free(pptx);
        return 1;
    }
    free(pptx);
    {
        size_t index;
        const char *start = NULL;
        const char *end;
        uint8_t *png = NULL;
        size_t png_size = 0u;
        unsigned width = 0u;
        unsigned height = 0u;
        for (index = 0u; index + strlen(marker) <= html_size; ++index) {
            if (memcmp(html + index, marker, strlen(marker)) == 0) {
                start = (const char *)html + index + strlen(marker);
                break;
            }
        }
        if (!start) {
            fputs("FAIL: cropped image data URI is missing\n", stderr);
            free(html);
            return 1;
        }
        end = start;
        while ((size_t)(end - (const char *)html) < html_size &&
               *end && *end != '"')
            ++end;
        png = base64_decode(start, (size_t)(end - start), &png_size);
        if (!png ||
            !png_dimensions(png, png_size, &width, &height) ||
            width != 20u || height != 10u) {
            fprintf(stderr, "FAIL: expected 20x10, got %ux%u\n", width,
                    height);
            free(png);
            free(html);
            return 1;
        }
        free(png);
    }
    free(html);
    puts("Quirewise PPTX source-rectangle crop tests passed");
    return 0;
}
