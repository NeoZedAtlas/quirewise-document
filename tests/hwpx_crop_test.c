/* HWPX hp:imgclip crops the embedded picture before PNG encoding. */

#include <quirewise/hwpx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_HWPX_CROP
#error "QUIREWISE_HWPX_CROP must be defined"
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

static int contains(const char *text, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(text + index, needle, needle_size) == 0) return 1;
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
    uint8_t *hwpx;
    size_t hwpx_size = 0u;
    QwHwpxDocument *document = NULL;
    const uint8_t *html = NULL;
    size_t html_size = 0u;
    const char *marker = "base64,";
    size_t index;
    const char *start = NULL;

    hwpx = read_file(QUIREWISE_HWPX_CROP, &hwpx_size);
    if (!hwpx || !hwpx_size) {
        fputs("FAIL: cannot read the HWPX crop fixture\n", stderr);
        return 1;
    }
    if (qw_hwpx_parse(hwpx, hwpx_size, &document) != QW_HWPX_OK ||
        !document) {
        free(hwpx);
        fputs("FAIL: HWPX crop fixture did not parse\n", stderr);
        return 1;
    }
    html = qw_hwpx_document_html(document, &html_size);
    if (!html || !html_size ||
        !contains((const char *)html, html_size, "width=\"192\"") ||
        !contains((const char *)html, html_size, "height=\"128\"")) {
        qw_hwpx_document_free(document);
        free(hwpx);
        fputs("FAIL: HWPX cropped image lost its display size\n", stderr);
        return 1;
    }
    for (index = 0u; index + strlen(marker) <= html_size; ++index) {
        if (memcmp(html + index, marker, strlen(marker)) == 0) {
            start = (const char *)html + index + strlen(marker);
            break;
        }
    }
    if (!start) {
        qw_hwpx_document_free(document);
        free(hwpx);
        fputs("FAIL: HWPX cropped image has no data URI\n", stderr);
        return 1;
    }
    {
        const char *end = start;
        size_t png_size = 0u;
        uint8_t *png;
        unsigned width = 0u;
        unsigned height = 0u;
        while ((size_t)(end - (const char *)html) < html_size && *end &&
               *end != '"')
            ++end;
        png = base64_decode(start, (size_t)(end - start), &png_size);
        if (png && png_size >= 24u && png[0] == 0x89u && png[1] == 'P' &&
            png[2] == 'N' && png[3] == 'G' &&
            memcmp(png + 12u, "IHDR", 4u) == 0) {
            width = ((unsigned)png[16] << 24) | ((unsigned)png[17] << 16) |
                    ((unsigned)png[18] << 8) | png[19];
            height = ((unsigned)png[20] << 24) | ((unsigned)png[21] << 16) |
                     ((unsigned)png[22] << 8) | png[23];
        }
        free(png);
        qw_hwpx_document_free(document);
        free(hwpx);
        if (width != 16u || height != 40u) {
            fprintf(stderr, "FAIL: expected 16x40 cropped PNG, got %ux%u\n",
                    width, height);
            return 1;
        }
    }
    puts("Quirewise HWPX crop tests passed");
    return 0;
}
