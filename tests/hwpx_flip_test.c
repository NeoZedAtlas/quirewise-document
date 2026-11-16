/* HWPX picture flips mirror the decoded pixels. */

#include <quirewise/hwpx.h>
#include <quirewise/image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_HWPX_FLIPPED
#error "QUIREWISE_HWPX_FLIPPED must be defined"
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
    int ok = 0;

    hwpx = read_file(QUIREWISE_HWPX_FLIPPED, &hwpx_size);
    if (!hwpx || !hwpx_size) {
        fputs("FAIL: cannot read the HWPX flip fixture\n", stderr);
        return 1;
    }
    if (qw_hwpx_parse(hwpx, hwpx_size, &document) != QW_HWPX_OK ||
        !document) {
        free(hwpx);
        fputs("FAIL: HWPX flip fixture did not parse\n", stderr);
        return 1;
    }
    html = qw_hwpx_document_html(document, &html_size);
    if (!html || !html_size) {
        qw_hwpx_document_free(document);
        free(hwpx);
        fputs("FAIL: HWPX flip fixture produced no HTML\n", stderr);
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
        fputs("FAIL: flipped HWPX image has no data URI\n", stderr);
        return 1;
    }
    {
        const char *end = start;
        size_t png_size = 0u;
        uint8_t *png;
        QwImage decoded;
        while ((size_t)(end - (const char *)html) < html_size && *end &&
               *end != '"')
            ++end;
        png = base64_decode(start, (size_t)(end - start), &png_size);
        memset(&decoded, 0, sizeof(decoded));
        if (png && qw_image_decode(png, png_size, &decoded) == QW_IMAGE_OK &&
            decoded.width == 4u && decoded.height == 2u &&
            decoded.format == QW_IMAGE_PIXEL_RGB24 && decoded.pixels) {
            const uint8_t *first = decoded.pixels;
            const uint8_t *last = decoded.pixels + 3u * 3u;
            ok = first[0] < 64u && first[2] > 128u &&
                 last[0] > 128u && last[2] < 64u;
        }
        qw_image_free(&decoded);
        free(png);
    }
    qw_hwpx_document_free(document);
    free(hwpx);
    if (!ok) {
        fputs("FAIL: horizontal flip did not mirror the pixels\n", stderr);
        return 1;
    }
    puts("Quirewise HWPX flip tests passed");
    return 0;
}
