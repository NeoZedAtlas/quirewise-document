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
    size_t head;
    size_t head_size;
    size_t hhea;
    size_t hhea_size;
    size_t maxp;
    size_t maxp_size;
    unsigned units_per_em;
    unsigned num_glyphs;
    unsigned num_hmetrics;
    int index_to_loc;
    int cff;
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
    if (version == 0x4F54544Fu) {
        font->cff = 1; /* OpenType/CFF: no loca/glyf tables */
    } else if (version != 0x00010000u && version != 0x74727565u) {
        qw_pdf_font_free(font);
        return NULL;
    }
    if (!find_table(font->data, font->size, "head", &font->head,
                    &font->head_size) ||
        font->head_size < 54u ||
        !find_table(font->data, font->size, "maxp", &font->maxp,
                    &font->maxp_size) ||
        font->maxp_size < 6u ||
        !find_table(font->data, font->size, "hhea", &font->hhea,
                    &font->hhea_size) ||
        font->hhea_size < 36u ||
        !find_table(font->data, font->size, "hmtx", &font->hmtx,
                    &font->hmtx_size) ||
        !find_table(font->data, font->size, "cmap", &font->cmap,
                    &font->cmap_size)) {
        qw_pdf_font_free(font);
        return NULL;
    }
    if (!font->cff &&
        (!find_table(font->data, font->size, "loca", &font->loca,
                     &font->loca_size) ||
         !find_table(font->data, font->size, "glyf", &font->glyf,
                     &font->glyf_size))) {
        qw_pdf_font_free(font);
        return NULL;
    }
    font->units_per_em = read_u16(font->data, font->size, font->head + 18u);
    font->index_to_loc = read_s16(font->data, font->size, font->head + 50u);
    font->bbox_x_min = read_s16(font->data, font->size, font->head + 36u);
    font->bbox_y_min = read_s16(font->data, font->size, font->head + 38u);
    font->bbox_x_max = read_s16(font->data, font->size, font->head + 40u);
    font->bbox_y_max = read_s16(font->data, font->size, font->head + 42u);
    font->num_glyphs = read_u16(font->data, font->size, font->maxp + 4u);
    font->num_hmetrics = read_u16(font->data, font->size, font->hhea + 34u);
    if (!font->units_per_em || !font->num_glyphs || !font->num_hmetrics ||
        (!font->cff && (font->index_to_loc < 0 || font->index_to_loc > 1))) {
        qw_pdf_font_free(font);
        return NULL;
    }
    /* Ascender/descender come from hhea; cap height from OS/2 when present. */
    font->ascent = read_s16(font->data, font->size, font->hhea + 4u);
    font->descent = read_s16(font->data, font->size, font->hhea + 6u);
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

/* ---- subsetting ---- */

typedef struct QwPdfSubsetBuild {
    const QwPdfFont *font;
    uint8_t *glyf;
    size_t glyf_size;
    size_t glyf_capacity;
    uint32_t *offsets;
    uint16_t *map;
    uint16_t *order;
    unsigned count;
} QwPdfSubsetBuild;

typedef struct QwPdfCmapEntry {
    unsigned int codepoint;
    uint16_t gid;
} QwPdfCmapEntry;

static void write_u16(uint8_t *data, unsigned value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static int subset_loca_range(const QwPdfFont *font, unsigned gid,
                             size_t *start, size_t *end)
{
    size_t a;
    size_t b;
    if (gid >= font->num_glyphs) return 0;
    if (font->index_to_loc == 0) {
        a = font->loca + (size_t)gid * 2u;
        b = a + 2u;
        if (a + 4u > font->loca + font->loca_size) return 0;
        *start = (size_t)read_u16(font->data, font->size, a) * 2u;
        *end = (size_t)read_u16(font->data, font->size, b) * 2u;
    } else {
        a = font->loca + (size_t)gid * 4u;
        b = a + 4u;
        if (a + 8u > font->loca + font->loca_size) return 0;
        *start = read_u32(font->data, font->size, a);
        *end = read_u32(font->data, font->size, b);
    }
    if (*start > *end || *end > font->glyf_size) return 0;
    return 1;
}

static int subset_append_glyf(QwPdfSubsetBuild *build, const uint8_t *data,
                              size_t size)
{
    if (build->glyf_size + size > build->glyf_capacity) {
        size_t grown = build->glyf_capacity ? build->glyf_capacity : 4096u;
        uint8_t *resized;
        while (grown < build->glyf_size + size) {
            if (grown > ((size_t)-1) / 2u) return 0;
            grown *= 2u;
        }
        resized = (uint8_t *)realloc(build->glyf, grown);
        if (!resized) return 0;
        build->glyf = resized;
        build->glyf_capacity = grown;
    }
    if (size) memcpy(build->glyf + build->glyf_size, data, size);
    build->glyf_size += size;
    return 1;
}

static int subset_add(QwPdfSubsetBuild *build, unsigned old_gid)
{
    if (old_gid >= build->font->num_glyphs) return 0;
    if (build->map[old_gid] != 0xFFFFu) return 1;
    if (build->count >= build->font->num_glyphs) return 0;
    build->map[old_gid] = (uint16_t)build->count;
    build->order[build->count] = (uint16_t)old_gid;
    build->count += 1u;
    return 1;
}

static int subset_closure(QwPdfSubsetBuild *build)
{
    size_t scan = 0u;
    while (scan < build->count) {
        unsigned old_gid = build->order[scan++];
        size_t start;
        size_t end;
        size_t pos;
        int16_t contours;
        if (!subset_loca_range(build->font, old_gid, &start, &end)) return 0;
        if (end - start < 10u) continue;
        contours = read_s16(build->font->data, build->font->size,
                            build->font->glyf + start);
        if (contours >= 0) continue;
        pos = start + 10u;
        for (;;) {
            uint16_t flags;
            uint16_t component;
            size_t args;
            if (pos + 4u > end) return 0;
            flags = read_u16(build->font->data, build->font->size,
                             build->font->glyf + pos);
            component = read_u16(build->font->data, build->font->size,
                                 build->font->glyf + pos + 2u);
            pos += 4u;
            args = (flags & 0x0001u) ? 4u : 2u;
            pos += args;
            if (flags & 0x0008u) pos += 2u;
            else if (flags & 0x0040u) pos += 4u;
            else if (flags & 0x0080u) pos += 8u;
            if (pos > end) return 0;
            if (!subset_add(build, component)) return 0;
            if (!(flags & 0x0020u)) break;
        }
    }
    return 1;
}

static int subset_build_glyf(QwPdfSubsetBuild *build)
{
    unsigned index;
    for (index = 0u; index < build->count; ++index) {
        unsigned old_gid = build->order[index];
        size_t start;
        size_t end;
        size_t base;
        if (!subset_loca_range(build->font, old_gid, &start, &end)) return 0;
        while (build->glyf_size % 4u) {
            if (!subset_append_glyf(build, (const uint8_t *)"\0", 1u)) return 0;
        }
        build->offsets[index] = (uint32_t)build->glyf_size;
        base = build->glyf_size;
        if (end > start &&
            !subset_append_glyf(build,
                                build->font->data + build->font->glyf + start,
                                end - start)) {
            return 0;
        }
        if (end - start >= 10u &&
            read_s16(build->font->data, build->font->size,
                     build->font->glyf + start) < 0) {
            size_t pos = base + 10u;
            for (;;) {
                uint16_t flags;
                size_t args;
                uint16_t old_component;
                uint16_t new_component;
                if (pos + 4u > build->glyf_size) return 0;
                flags = read_u16(build->glyf, build->glyf_size, pos);
                old_component = read_u16(build->glyf, build->glyf_size, pos + 2u);
                new_component = build->map[old_component];
                if (new_component == 0xFFFFu) return 0;
                write_u16(build->glyf + pos + 2u, new_component);
                pos += 4u;
                args = (flags & 0x0001u) ? 4u : 2u;
                pos += args;
                if (flags & 0x0008u) pos += 2u;
                else if (flags & 0x0040u) pos += 4u;
                else if (flags & 0x0080u) pos += 8u;
                if (!(flags & 0x0020u)) break;
            }
        }
    }
    build->offsets[build->count] = (uint32_t)build->glyf_size;
    return 1;
}

static uint16_t subset_advance(const QwPdfFont *font, unsigned gid)
{
    size_t offset;
    unsigned metrics = font->num_hmetrics;
    if (!metrics) return 0u;
    if (gid >= metrics) gid = metrics - 1u;
    offset = font->hmtx + (size_t)gid * 4u;
    if (offset + 2u > font->hmtx + font->hmtx_size) return 0u;
    return read_u16(font->data, font->size, offset);
}

static int16_t subset_lsb(const QwPdfFont *font, unsigned gid)
{
    size_t offset;
    unsigned metrics = font->num_hmetrics;
    if (!metrics) return 0;
    if (gid < metrics) {
        offset = font->hmtx + (size_t)gid * 4u + 2u;
    } else {
        offset = font->hmtx + (size_t)metrics * 4u +
                 (size_t)(gid - metrics) * 2u;
    }
    if (offset + 2u > font->hmtx + font->hmtx_size) return 0;
    return read_s16(font->data, font->size, offset);
}

static int cmap_entry_compare(const void *left, const void *right)
{
    unsigned int a = ((const QwPdfCmapEntry *)left)->codepoint;
    unsigned int b = ((const QwPdfCmapEntry *)right)->codepoint;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static uint8_t *subset_build_cmap(const QwPdfCmapEntry *entries, size_t count,
                                  size_t *out_size)
{
    size_t segments = count + 1u;
    size_t length;
    size_t size;
    size_t seg_x2;
    size_t search_range;
    size_t index;
    unsigned entry_selector = 0u;
    uint8_t *cmap;
    uint8_t *sub;
    if (count > 0xFFFFu) return NULL;
    seg_x2 = segments * 2u;
    length = 16u + 8u * segments;
    size = 12u + length;
    cmap = (uint8_t *)calloc(1u, size);
    if (!cmap) return NULL;
    write_u16(cmap, 0u);
    write_u16(cmap + 2u, 1u);
    write_u16(cmap + 4u, 3u);
    write_u16(cmap + 6u, 1u);
    write_u32(cmap + 8u, 12u);
    sub = cmap + 12u;
    write_u16(sub, 4u);
    write_u16(sub + 2u, (unsigned)length);
    write_u16(sub + 4u, 0u);
    write_u16(sub + 6u, (unsigned)seg_x2);
    while (((size_t)1u << (entry_selector + 1u)) <= segments) entry_selector += 1u;
    search_range = (size_t)2u * ((size_t)1u << entry_selector);
    write_u16(sub + 8u, (unsigned)search_range);
    write_u16(sub + 10u, entry_selector);
    write_u16(sub + 12u, (unsigned)(seg_x2 - search_range));
    for (index = 0u; index < count; ++index) {
        write_u16(sub + 14u + index * 2u, entries[index].codepoint);
    }
    write_u16(sub + 14u + count * 2u, 0xFFFFu);
    for (index = 0u; index < count; ++index) {
        write_u16(sub + 16u + seg_x2 + index * 2u, entries[index].codepoint);
    }
    write_u16(sub + 16u + seg_x2 + count * 2u, 0xFFFFu);
    for (index = 0u; index < count; ++index) {
        unsigned delta =
            ((unsigned)entries[index].gid - entries[index].codepoint) & 0xFFFFu;
        write_u16(sub + 16u + 2u * seg_x2 + index * 2u, delta);
    }
    write_u16(sub + 16u + 2u * seg_x2 + count * 2u, 1u);
    *out_size = size;
    return cmap;
}

static uint32_t font_checksum(const uint8_t *data, size_t size)
{
    uint32_t sum = 0u;
    size_t index = 0u;
    while (index + 4u <= size) {
        sum += read_u32(data, size, index);
        index += 4u;
    }
    if (index < size) {
        uint8_t tail[4] = {0u, 0u, 0u, 0u};
        memcpy(tail, data + index, size - index);
        sum += read_u32(tail, 4u, 0u);
    }
    return sum;
}

typedef struct QwPdfTable {
    const char *tag;
    const uint8_t *data;
    size_t size;
} QwPdfTable;

static uint8_t *subset_assemble(QwPdfSubsetBuild *build,
                                const QwPdfCmapEntry *entries,
                                size_t entry_count, size_t *out_size)
{
    const QwPdfFont *font = build->font;
    uint8_t head[54];
    uint8_t hhea[36];
    uint8_t *maxp = NULL;
    uint8_t *hmtx = NULL;
    uint8_t *loca = NULL;
    uint8_t *cmap = NULL;
    size_t cmap_size = 0u;
    QwPdfTable tables[7];
    size_t offsets[7];
    size_t index;
    size_t offset;
    size_t total;
    uint32_t sum;
    uint32_t adjustment;
    uint8_t *out = NULL;

    hmtx = (uint8_t *)calloc(build->count ? build->count : 1u, 4u);
    loca = (uint8_t *)calloc((size_t)build->count + 1u, 4u);
    maxp = (uint8_t *)malloc(font->maxp_size);
    if (!hmtx || !loca || !maxp) goto done;
    memcpy(head, font->data + font->head, 54u);
    memcpy(hhea, font->data + font->hhea, 36u);
    memcpy(maxp, font->data + font->maxp, font->maxp_size);
    head[50] = 0u;
    head[51] = 1u;
    memset(head + 8, 0, 4u);
    write_u16(hhea + 34u, build->count);
    write_u16(maxp + 4u, build->count);
    for (index = 0u; index < build->count; ++index) {
        unsigned old_gid = build->order[index];
        write_u16(hmtx + index * 4u, subset_advance(font, old_gid));
        write_u16(hmtx + index * 4u + 2u, (uint16_t)subset_lsb(font, old_gid));
    }
    for (index = 0u; index <= build->count; ++index) {
        write_u32(loca + index * 4u, build->offsets[index]);
    }
    cmap = subset_build_cmap(entries, entry_count, &cmap_size);
    if (!cmap) goto done;

    tables[0].tag = "cmap"; tables[0].data = cmap; tables[0].size = cmap_size;
    tables[1].tag = "glyf"; tables[1].data = build->glyf; tables[1].size = build->glyf_size;
    tables[2].tag = "head"; tables[2].data = head; tables[2].size = 54u;
    tables[3].tag = "hhea"; tables[3].data = hhea; tables[3].size = 36u;
    tables[4].tag = "hmtx"; tables[4].data = hmtx; tables[4].size = (size_t)build->count * 4u;
    tables[5].tag = "loca"; tables[5].data = loca; tables[5].size = ((size_t)build->count + 1u) * 4u;
    tables[6].tag = "maxp"; tables[6].data = maxp; tables[6].size = font->maxp_size;

    total = 12u + 7u * 16u;
    offset = total;
    for (index = 0u; index < 7u; ++index) {
        while (offset % 4u) offset += 1u;
        offsets[index] = offset;
        offset += tables[index].size;
    }
    while (offset % 4u) offset += 1u;
    total = offset;
    out = (uint8_t *)calloc(1u, total);
    if (!out) goto done;
    write_u32(out, 0x00010000u);
    write_u16(out + 4u, 7u);
    write_u16(out + 6u, 64u);
    write_u16(out + 8u, 2u);
    write_u16(out + 10u, 48u);
    for (index = 0u; index < 7u; ++index) {
        size_t entry = 12u + index * 16u;
        memcpy(out + entry, tables[index].tag, 4u);
        write_u32(out + entry + 4u, font_checksum(tables[index].data,
                                                  tables[index].size));
        write_u32(out + entry + 8u, (uint32_t)offsets[index]);
        write_u32(out + entry + 12u, (uint32_t)tables[index].size);
        memcpy(out + offsets[index], tables[index].data, tables[index].size);
    }
    sum = font_checksum(out, total);
    adjustment = 0xB1B0AFBAu - sum;
    write_u32(out + offsets[2] + 8u, adjustment);
    *out_size = total;

done:
    free(hmtx);
    free(loca);
    free(maxp);
    free(cmap);
    return out;
}

int qw_pdf_font_subset(const QwPdfFont *font, const uint16_t *gids,
                       const unsigned int *codepoints, size_t count,
                       QwPdfFontSubset *out)
{
    QwPdfSubsetBuild build;
    QwPdfCmapEntry *entries = NULL;
    size_t entry_count = 0u;
    size_t index;
    if (!font || !out || (count && (!gids || !codepoints))) return 0;
    memset(out, 0, sizeof(*out));
    if (font->cff) {
        uint16_t *map;
        uint8_t *copy;
        unsigned index;
        map = (uint16_t *)malloc((size_t)font->num_glyphs * sizeof(uint16_t));
        copy = (uint8_t *)malloc(font->size);
        if (!map || !copy) {
            free(map);
            free(copy);
            return 0;
        }
        memcpy(copy, font->data, font->size);
        for (index = 0u; index < font->num_glyphs; ++index)
            map[index] = (uint16_t)index;
        out->data = copy;
        out->size = font->size;
        out->map = map;
        out->num_glyphs = font->num_glyphs;
        return 1;
    }
    memset(&build, 0, sizeof(build));
    build.font = font;
    build.map = (uint16_t *)malloc((size_t)font->num_glyphs * sizeof(uint16_t));
    build.order = (uint16_t *)malloc((size_t)font->num_glyphs * sizeof(uint16_t));
    build.offsets =
        (uint32_t *)malloc(((size_t)font->num_glyphs + 1u) * sizeof(uint32_t));
    if (!build.map || !build.order || !build.offsets) goto fail;
    for (index = 0u; index < font->num_glyphs; ++index) build.map[index] = 0xFFFFu;
    if (!subset_add(&build, 0u)) goto fail;
    for (index = 0u; index < count; ++index) {
        if (!subset_add(&build, gids[index])) goto fail;
    }
    if (!subset_closure(&build)) goto fail;
    if (!subset_build_glyf(&build)) goto fail;
    entries = (QwPdfCmapEntry *)malloc((count ? count : 1u) * sizeof(*entries));
    if (!entries) goto fail;
    for (index = 0u; index < count; ++index) {
        uint16_t new_gid = build.map[gids[index]];
        if (new_gid == 0xFFFFu) continue;
        entries[entry_count].codepoint = codepoints[index];
        entries[entry_count].gid = new_gid;
        entry_count += 1u;
    }
    qsort(entries, entry_count, sizeof(*entries), cmap_entry_compare);
    {
        size_t unique = 0u;
        for (index = 0u; index < entry_count; ++index) {
            if (unique && entries[index].codepoint == entries[unique - 1u].codepoint)
                continue;
            entries[unique++] = entries[index];
        }
        entry_count = unique;
    }
    out->data = subset_assemble(&build, entries, entry_count, &out->size);
    if (!out->data) goto fail;
    out->map = build.map;
    out->num_glyphs = build.count;
    free(build.glyf);
    free(build.order);
    free(build.offsets);
    free(entries);
    return 1;
fail:
    free(build.glyf);
    free(build.map);
    free(build.order);
    free(build.offsets);
    free(entries);
    return 0;
}

void qw_pdf_font_subset_free(QwPdfFontSubset *subset)
{
    if (!subset) return;
    free(subset->data);
    free(subset->map);
    memset(subset, 0, sizeof(*subset));
}

int qw_pdf_font_is_cff(const QwPdfFont *font)
{
    return font ? font->cff : 0;
}
