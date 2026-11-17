#include "pdf_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_PDF_FONT_MAX_BYTES ((size_t)64u * 1024u * 1024u)

struct QwPdfFont {
    uint8_t *data;
    size_t size;
    size_t glyf;
    size_t glyf_size;
    size_t loca;
    size_t loca_size;
    size_t hmtx;
    size_t hmtx_size;
    size_t cmap;
    size_t cmap_size;
    unsigned units_per_em;
    unsigned num_glyphs;
    unsigned num_hmetrics;
    int index_to_loc;
    int ascent;
    int descent;
    int cap_height;
    int bbox_x_min;
    int bbox_y_min;
    int bbox_x_max;
    int bbox_y_max;
};

static uint16_t read_u16(const uint8_t *data, size_t size, size_t offset)
{
    if (offset + 2u > size) return 0u;
    return (uint16_t)(((uint16_t)data[offset] << 8) | data[offset + 1u]);
}

static uint32_t read_u32(const uint8_t *data, size_t size, size_t offset)
{
    if (offset + 4u > size) return 0u;
    return ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1u] << 16) |
           ((uint32_t)data[offset + 2u] << 8) | data[offset + 3u];
}

static int16_t read_s16(const uint8_t *data, size_t size, size_t offset)
{
    return (int16_t)read_u16(data, size, offset);
}

static int find_table(const uint8_t *data, size_t size, const char *tag,
                      size_t *out_offset, size_t *out_length)
{
    uint16_t count = read_u16(data, size, 4u);
    uint16_t index;
    for (index = 0u; index < count; ++index) {
        size_t entry = 12u + (size_t)index * 16u;
        if (entry + 16u > size) return 0;
        if (memcmp(data + entry, tag, 4u) == 0) {
            uint32_t offset = read_u32(data, size, entry + 8u);
            uint32_t length = read_u32(data, size, entry + 12u);
            if ((size_t)offset > size || (size_t)length > size - offset)
                return 0;
            *out_offset = offset;
            *out_length = length;
            return 1;
        }
    }
    return 0;
}

static int parse_cmap4(const uint8_t *data, size_t size, size_t table,
                       size_t table_size, unsigned codepoint, uint16_t *out_gid)
{
    size_t seg_x2 = read_u16(data, size, table + 6u);
    size_t seg_count;
    size_t end_codes;
    size_t start_codes;
    size_t deltas;
    size_t range_offsets;
    size_t index;
    if (seg_x2 == 0u || seg_x2 % 2u) return 0;
    seg_count = seg_x2 / 2u;
    end_codes = table + 14u;
    start_codes = end_codes + seg_x2 + 2u;
    deltas = start_codes + seg_x2;
    range_offsets = deltas + seg_x2;
    if (range_offsets + seg_x2 > table + table_size) return 0;
    for (index = 0u; index < seg_count; ++index) {
        uint16_t end = read_u16(data, size, end_codes + index * 2u);
        uint16_t start;
        if (codepoint > end) continue;
        start = read_u16(data, size, start_codes + index * 2u);
        if (codepoint < start) return 0;
        {
            uint16_t delta = read_u16(data, size, deltas + index * 2u);
            uint16_t range = read_u16(data, size, range_offsets + index * 2u);
            if (range == 0u) {
                *out_gid = (uint16_t)((codepoint + delta) & 0xFFFFu);
                return 1;
            }
            {
                size_t glyph_address = range_offsets + index * 2u + range +
                                       (codepoint - start) * 2u;
                uint16_t glyph;
                if (glyph_address + 2u > table + table_size) return 0;
                glyph = read_u16(data, size, glyph_address);
                if (glyph == 0u) return 0;
                *out_gid = (uint16_t)((glyph + delta) & 0xFFFFu);
                return 1;
            }
        }
    }
    return 0;
}

static int parse_cmap12(const uint8_t *data, size_t size, size_t table,
                        size_t table_size, unsigned codepoint, uint16_t *out_gid)
{
    uint32_t groups = read_u32(data, size, table + 12u);
    uint32_t index;
    if (groups > (table_size - 16u) / 12u) return 0;
    for (index = 0u; index < groups; ++index) {
        size_t entry = table + 16u + (size_t)index * 12u;
        uint32_t start = read_u32(data, size, entry);
        uint32_t end = read_u32(data, size, entry + 4u);
        uint32_t start_gid = read_u32(data, size, entry + 8u);
        if (codepoint < start || codepoint > end) continue;
        *out_gid = (uint16_t)(start_gid + (codepoint - start));
        return 1;
    }
    return 0;
}

static int cmap_lookup(const QwPdfFont *font, unsigned codepoint,
                       uint16_t *out_gid)
{
    size_t table = font->cmap;
    size_t table_size = font->cmap_size;
    size_t count;
    size_t index;
    if (table_size < 4u) return 0;
    count = read_u16(font->data, font->size, table + 2u);
    for (index = 0u; index < count; ++index) {
        size_t entry = table + 4u + index * 8u;
        uint16_t platform;
        uint16_t encoding;
        size_t subtable;
        size_t subtable_size;
        uint16_t format;
        if (entry + 8u > table + table_size) return 0;
        platform = read_u16(font->data, font->size, entry);
        encoding = read_u16(font->data, font->size, entry + 2u);
        subtable = table + read_u32(font->data, font->size, entry + 4u);
        if (subtable >= table + table_size) continue;
        subtable_size = (table + table_size) - subtable;
        format = read_u16(font->data, font->size, subtable);
        if (format == 12u && subtable_size >= 16u) {
            if (parse_cmap12(font->data, font->size, subtable, subtable_size,
                             codepoint, out_gid))
                return 1;
        } else if (format == 4u && subtable_size >= 16u) {
            if (parse_cmap4(font->data, font->size, subtable, subtable_size,
                            codepoint, out_gid))
                return 1;
        }
        (void)platform;
        (void)encoding;
    }
    return 0;
}

QwPdfFont *qw_pdf_font_load(const char *path)
{
    FILE *file;
    long length;
    QwPdfFont *font;
    uint32_t version;
    size_t head;
    size_t head_size;
    size_t maxp;
    size_t maxp_size;
    size_t hhea;
    size_t hhea_size;
    if (!path) return NULL;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    length = ftell(file);
    if (length <= 0 || (size_t)length > QW_PDF_FONT_MAX_BYTES) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    font = (QwPdfFont *)calloc(1u, sizeof(*font));
    if (!font) { fclose(file); return NULL; }
    font->data = (uint8_t *)malloc((size_t)length);
    if (!font->data || fread(font->data, 1u, (size_t)length, file) !=
                           (size_t)length) {
        free(font->data);
        free(font);
        fclose(file);
        return NULL;
    }
    fclose(file);
    font->size = (size_t)length;

    version = read_u32(font->data, font->size, 0u);
    if (version != 0x00010000u && version != 0x74727565u) {
        /* CFF/OTTO and unknown sfnt flavours are not embeddable here. */
        qw_pdf_font_free(font);
        return NULL;
    }
    if (!find_table(font->data, font->size, "head", &head, &head_size) ||
        head_size < 54u ||
        !find_table(font->data, font->size, "maxp", &maxp, &maxp_size) ||
        maxp_size < 6u ||
        !find_table(font->data, font->size, "hhea", &hhea, &hhea_size) ||
        hhea_size < 36u ||
        !find_table(font->data, font->size, "hmtx", &font->hmtx,
                    &font->hmtx_size) ||
        !find_table(font->data, font->size, "cmap", &font->cmap,
                    &font->cmap_size) ||
        !find_table(font->data, font->size, "loca", &font->loca,
                    &font->loca_size) ||
        !find_table(font->data, font->size, "glyf", &font->glyf,
                    &font->glyf_size)) {
        qw_pdf_font_free(font);
        return NULL;
    }
    font->units_per_em = read_u16(font->data, font->size, head + 18u);
    font->index_to_loc = read_s16(font->data, font->size, head + 50u);
    font->bbox_x_min = read_s16(font->data, font->size, head + 36u);
    font->bbox_y_min = read_s16(font->data, font->size, head + 38u);
    font->bbox_x_max = read_s16(font->data, font->size, head + 40u);
    font->bbox_y_max = read_s16(font->data, font->size, head + 42u);
    font->num_glyphs = read_u16(font->data, font->size, maxp + 4u);
    font->num_hmetrics = read_u16(font->data, font->size, hhea + 34u);
    if (!font->units_per_em || !font->num_glyphs || !font->num_hmetrics ||
        font->index_to_loc < 0 || font->index_to_loc > 1) {
        qw_pdf_font_free(font);
        return NULL;
    }
    /* Ascender/descender come from hhea; cap height from OS/2 when present. */
    font->ascent = read_s16(font->data, font->size, hhea + 4u);
    font->descent = read_s16(font->data, font->size, hhea + 6u);
    font->cap_height = font->ascent;
    {
        size_t os2;
        size_t os2_size;
        if (find_table(font->data, font->size, "OS/2", &os2, &os2_size) &&
            os2_size >= 90u) {
            uint16_t version = read_u16(font->data, font->size, os2);
            if (version >= 2u) {
                font->cap_height = read_s16(font->data, font->size, os2 + 88u);
            }
        }
    }
    return font;
}

void qw_pdf_font_free(QwPdfFont *font)
{
    if (!font) return;
    free(font->data);
    free(font);
}

uint16_t qw_pdf_font_gid(const QwPdfFont *font, unsigned int codepoint)
{
    uint16_t gid = 0u;
    if (!font) return 0u;
    if (!cmap_lookup(font, codepoint, &gid)) return 0u;
    if (gid >= font->num_glyphs) return 0u;
    return gid;
}

uint16_t qw_pdf_font_advance(const QwPdfFont *font, uint16_t gid)
{
    size_t offset;
    unsigned advance;
    if (!font) return 0u;
    if (gid >= font->num_hmetrics) gid = (uint16_t)(font->num_hmetrics - 1u);
    offset = font->hmtx + (size_t)gid * 4u;
    if (offset + 2u > font->hmtx + font->hmtx_size) return 0u;
    advance = read_u16(font->data, font->size, offset);
    return (uint16_t)((advance * 1000u + font->units_per_em / 2u) /
                      font->units_per_em);
}

const uint8_t *qw_pdf_font_data(const QwPdfFont *font, size_t *size)
{
    if (!font) {
        if (size) *size = 0u;
        return NULL;
    }
    if (size) *size = font->size;
    return font->data;
}

int qw_pdf_font_metrics(const QwPdfFont *font, int *ascent, int *descent,
                        int *cap_height)
{
    if (!font) return 0;
    if (ascent) {
        *ascent = (int)(((long)font->ascent * 1000 + (long)font->units_per_em / 2) /
                        (long)font->units_per_em);
    }
    if (descent) {
        *descent = (int)(((long)font->descent * 1000 + (long)font->units_per_em / 2) /
                         (long)font->units_per_em);
    }
    if (cap_height) {
        *cap_height = (int)(((long)font->cap_height * 1000 +
                             (long)font->units_per_em / 2) /
                            (long)font->units_per_em);
    }
    return 1;
}

int qw_pdf_font_bbox(const QwPdfFont *font, int *x_min, int *y_min,
                     int *x_max, int *y_max)
{
    long upem;
    if (!font) return 0;
    upem = (long)font->units_per_em;
    if (x_min) *x_min = (int)(((long)font->bbox_x_min * 1000 + upem / 2) / upem);
    if (y_min) *y_min = (int)(((long)font->bbox_y_min * 1000 + upem / 2) / upem);
    if (x_max) *x_max = (int)(((long)font->bbox_x_max * 1000 + upem / 2) / upem);
    if (y_max) *y_max = (int)(((long)font->bbox_y_max * 1000 + upem / 2) / upem);
    return 1;
}