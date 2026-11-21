/* Rebuild a CFF/OpenType font with only the glyphs a document uses. */
#include "pdf_cff.h"

#include <stdlib.h>
#include <string.h>

#define CFF_MAX_GLYPHS 65535u

static uint16_t cff_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t cff_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void cff_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t cff_read_offset(const uint8_t *p, unsigned size)
{
    uint32_t value = 0u;
    unsigned i;
    for (i = 0u; i < size; ++i) value = (value << 8) | p[i];
    return value;
}

static void cff_write_offset(uint8_t *p, uint32_t value, unsigned size)
{
    unsigned i;
    for (i = 0u; i < size; ++i)
        p[size - 1u - i] = (uint8_t)(value >> (8u * i));
}

typedef struct CffIndex {
    size_t start;
    size_t count;
    unsigned off_size;
    size_t offsets;
    size_t data;
    size_t end;
} CffIndex;

static int cff_index(const uint8_t *cff, size_t size, size_t pos,
                     CffIndex *index)
{
    uint32_t last;
    if (pos + 2u > size) return 0;
    index->start = pos;
    index->count = cff_u16(cff + pos);
    if (index->count == 0u) {
        index->off_size = 1u;
        index->offsets = pos + 2u;
        index->data = pos + 2u;
        index->end = pos + 2u;
        return 1;
    }
    if (pos + 3u > size) return 0;
    index->off_size = cff[pos + 2u];
    if (index->off_size < 1u || index->off_size > 4u) return 0;
    index->offsets = pos + 3u;
    if (index->offsets + (index->count + 1u) * index->off_size > size)
        return 0;
    index->data = index->offsets + (index->count + 1u) * index->off_size;
    last = cff_read_offset(cff + index->offsets + index->count * index->off_size,
                           index->off_size);
    if (last == 0u || index->data + last - 1u > size) return 0;
    index->end = index->data + last - 1u;
    return 1;
}

static size_t cff_index_length(const CffIndex *index)
{
    return index->end - index->start;
}

static const uint8_t *cff_index_object(const uint8_t *cff,
                                       const CffIndex *index, size_t object,
                                       size_t *length)
{
    uint32_t start;
    uint32_t stop;
    if (object >= index->count) return NULL;
    start = cff_read_offset(cff + index->offsets + object * index->off_size,
                            index->off_size);
    stop = cff_read_offset(cff + index->offsets +
                               (object + 1u) * index->off_size,
                           index->off_size);
    if (stop < start) return NULL;
    *length = stop - start;
    return cff + index->data + start - 1u;
}

/* Return the operands of DICT operator `op` (12 `op2`). */
static int cff_dict_operands(const uint8_t *dict, size_t size, unsigned op,
                             uint32_t *values, size_t *value_count)
{
    uint32_t operands[48];
    size_t count = 0u;
    size_t pos = 0u;
    while (pos < size) {
        uint8_t b = dict[pos];
        if (b <= 21u) {
            unsigned current = b;
            if (b == 12u) {
                if (pos + 1u >= size) return 0;
                current = 1200u + dict[pos + 1u];
                pos += 2u;
            } else {
                pos += 1u;
            }
            if (current == op) {
                size_t i;
                for (i = 0u; i < count && i < *value_count; ++i)
                    values[i] = operands[i];
                *value_count = count;
                return 1;
            }
            count = 0u;
            continue;
        }
        if (b == 28u) {
            if (pos + 3u > size) return 0;
            operands[count % 48u] = (uint32_t)(int16_t)cff_u16(dict + pos + 1u);
            pos += 3u;
        } else if (b == 29u) {
            if (pos + 5u > size) return 0;
            operands[count % 48u] = cff_u32(dict + pos + 1u);
            pos += 5u;
        } else if (b >= 32u && b <= 246u) {
            operands[count % 48u] = (uint32_t)((int32_t)b - 139);
            pos += 1u;
        } else if (b >= 247u && b <= 250u) {
            if (pos + 2u > size) return 0;
            operands[count % 48u] =
                (uint32_t)((b - 247u) * 256u + dict[pos + 1u] + 108u);
            pos += 2u;
        } else if (b >= 251u && b <= 254u) {
            if (pos + 2u > size) return 0;
            operands[count % 48u] =
                (uint32_t)(-((int32_t)(b - 251u) * 256) - dict[pos + 1u] -
                           108);
            pos += 2u;
        } else if (b == 30u) {
            pos += 1u;
            while (pos < size) {
                uint8_t value = dict[pos++];
                if ((value & 0x0fu) == 0x0fu || (value >> 4) == 0x0fu) break;
            }
            operands[count % 48u] = 0u;
        } else {
            return 0;
        }
        ++count;
    }
    return 0;
}

typedef struct CffDictReplacement {
    unsigned op;
    size_t count;
    uint32_t values[2];
} CffDictReplacement;

/* Copy a DICT, replacing the operands of the listed operators. */
static int cff_rewrite_dict(const uint8_t *dict, size_t size,
                            const CffDictReplacement *replacements,
                            size_t replacement_count, uint8_t *out,
                            size_t out_capacity, size_t *out_size)
{
    size_t pos = 0u;
    size_t copy_from = 0u;
    size_t operand_start = 0u;
    size_t out_pos = 0u;
    while (pos < size) {
        uint8_t b = dict[pos];
        if (b <= 21u) {
            size_t op_start = pos;
            unsigned op;
            size_t r;
            if (b == 12u) {
                if (pos + 1u >= size) return 0;
                op = 1200u + dict[pos + 1u];
                pos += 2u;
            } else {
                op = b;
                pos += 1u;
            }
            for (r = 0u; r < replacement_count; ++r) {
                if (replacements[r].op != op) continue;
                {
                    size_t k;
                    if (out_pos + (operand_start - copy_from) > out_capacity)
                        return 0;
                    memcpy(out + out_pos, dict + copy_from,
                           operand_start - copy_from);
                    out_pos += operand_start - copy_from;
                    for (k = 0u; k < replacements[r].count; ++k) {
                        if (out_pos + 5u > out_capacity) return 0;
                        out[out_pos++] = 29u;
                        cff_put_u32(out + out_pos, replacements[r].values[k]);
                        out_pos += 4u;
                    }
                    if (out_pos + (pos - op_start) > out_capacity) return 0;
                    memcpy(out + out_pos, dict + op_start, pos - op_start);
                    out_pos += pos - op_start;
                }
                copy_from = pos;
                operand_start = pos;
                break;
            }
            if (r == replacement_count) operand_start = pos;
            continue;
        }
        if (b == 28u) {
            pos += 3u;
        } else if (b == 29u) {
            pos += 5u;
        } else if (b >= 32u && b <= 246u) {
            pos += 1u;
        } else if (b >= 247u && b <= 254u) {
            pos += 2u;
        } else if (b == 30u) {
            pos += 1u;
            while (pos < size) {
                uint8_t value = dict[pos++];
                if ((value & 0x0fu) == 0x0fu || (value >> 4) == 0x0fu) break;
            }
        } else {
            return 0;
        }
        if (pos > size) return 0;
    }
    if (out_pos + (size - copy_from) > out_capacity) return 0;
    memcpy(out + out_pos, dict + copy_from, size - copy_from);
    out_pos += size - copy_from;
    *out_size = out_pos;
    return 1;
}

/* Build an INDEX with the given objects (offsets are recomputed). */
static int cff_build_index(uint8_t *out, size_t capacity,
                           const uint8_t *const *objects,
                           const size_t *lengths, size_t count,
                           size_t *out_size)
{
    size_t total = 0u;
    size_t index;
    unsigned off_size = 1u;
    size_t header;
    if (count > 65535u) return 0;
    for (index = 0u; index < count; ++index) total += lengths[index];
    if (total + 1u > 0xFFFFFFu) return 0;
    while (total + 1u > ((uint32_t)1u << (8u * off_size))) ++off_size;
    if (off_size > 4u) return 0;
    header = 3u + (count + 1u) * off_size;
    if (header + total > capacity) return 0;
    out[0] = (uint8_t)(count >> 8);
    out[1] = (uint8_t)count;
    out[2] = (uint8_t)off_size;
    {
        uint32_t offset = 1u;
        size_t pos = 3u;
        for (index = 0u; index < count; ++index) {
            cff_write_offset(out + pos, offset, off_size);
            pos += off_size;
            offset += (uint32_t)lengths[index];
        }
        cff_write_offset(out + pos, offset, off_size);
    }
    {
        size_t pos = header;
        for (index = 0u; index < count; ++index) {
            memcpy(out + pos, objects[index], lengths[index]);
            pos += lengths[index];
        }
    }
    *out_size = header + total;
    return 1;
}

static int cff_charset(const uint8_t *cff, size_t size, size_t offset,
                       size_t glyph_count, uint16_t *values)
{
    uint8_t format;
    size_t pos = offset;
    size_t gid;
    if (!glyph_count || pos >= size) return 0;
    format = cff[pos++];
    values[0] = 0u;
    if (format == 0u) {
        for (gid = 1u; gid < glyph_count; ++gid) {
            if (pos + 2u > size) return 0;
            values[gid] = cff_u16(cff + pos);
            pos += 2u;
        }
        return 1;
    }
    if (format != 1u && format != 2u) return 0;
    gid = 1u;
    while (gid < glyph_count) {
        uint32_t first;
        uint32_t n_left;
        uint32_t i;
        if (pos + 2u > size) return 0;
        first = cff_u16(cff + pos);
        pos += 2u;
        if (format == 1u) {
            if (pos + 1u > size) return 0;
            n_left = cff[pos++];
        } else {
            if (pos + 2u > size) return 0;
            n_left = cff_u16(cff + pos);
            pos += 2u;
        }
        for (i = 0u; i <= n_left && gid < glyph_count; ++i)
            values[gid++] = (uint16_t)(first + i);
    }
    return gid == glyph_count;
}

static int cff_fdselect(const uint8_t *cff, size_t size, size_t offset,
                        size_t glyph_count, uint8_t *fds)
{
    uint8_t format;
    size_t pos = offset;
    if (pos >= size) return 0;
    format = cff[pos++];
    if (format == 0u) {
        size_t gid;
        if (pos + glyph_count > size) return 0;
        for (gid = 0u; gid < glyph_count; ++gid) fds[gid] = cff[pos + gid];
        return 1;
    }
    if (format == 3u) {
        uint16_t ranges;
        uint16_t i;
        size_t range_pos;
        if (pos + 2u > size) return 0;
        ranges = cff_u16(cff + pos);
        pos += 2u;
        range_pos = pos;
        for (i = 0u; i < ranges; ++i) {
            uint16_t first;
            uint8_t fd;
            uint16_t next;
            size_t gid;
            if (range_pos + 5u > size) return 0;
            first = cff_u16(cff + range_pos);
            fd = cff[range_pos + 2u];
            next = cff_u16(cff + range_pos + 3u);
            for (gid = first; gid < next && gid < glyph_count; ++gid)
                fds[gid] = fd;
            range_pos += 3u;
        }
        return 1;
    }
    return 0;
}

/* Copy the Private DICT and its local Subrs block; returns its byte length. */
static size_t cff_private_block(const uint8_t *cff, size_t size,
                                size_t private_offset, size_t private_size,
                                size_t *out_size)
{
    uint32_t subrs_relative = 0u;
    size_t value_count = 1u;
    size_t subrs_length = 0u;
    size_t total = private_size;
    if (private_offset + private_size > size) return 0u;
    if (cff_dict_operands(cff + private_offset, private_size, 19u,
                          &subrs_relative, &value_count) &&
        value_count == 1u) {
        CffIndex subrs;
        size_t subrs_start = private_offset + subrs_relative;
        if (subrs_start < size &&
            cff_index(cff, size, subrs_start, &subrs)) {
            subrs_length = cff_index_length(&subrs);
            if (subrs_relative + subrs_length > total)
                total = subrs_relative + subrs_length;
        }
    }
    *out_size = total;
    return total;
}

static uint32_t sfnt_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0u;
    size_t i;
    for (i = 0u; i + 4u <= length; i += 4u) sum += cff_u32(data + i);
    if (i < length) {
        uint32_t last = 0u;
        size_t k;
        for (k = 0u; k < length - i; ++k)
            last |= (uint32_t)data[i + k] << (24 - 8 * k);
        sum += last;
    }
    return sum;
}

static size_t align4(size_t value)
{
    return (value + 3u) & ~(size_t)3u;
}

/* Rebuild the sfnt with the new CFF table, keeping every other table. */
static int cff_wrap_sfnt(const uint8_t *font, size_t font_size,
                         const uint8_t *new_cff, size_t new_cff_size,
                         uint8_t **out, size_t *out_size)
{
    size_t table_count;
    size_t table;
    size_t dir_size;
    size_t data_size = 0u;
    size_t offset;
    uint8_t *result;
    if (font_size < 12u) return 0;
    table_count = cff_u16(font + 4u);
    dir_size = 12u + table_count * 16u;
    for (table = 0u; table < table_count; ++table) {
        const uint8_t *record = font + 12u + table * 16u;
        uint32_t length = cff_u32(record + 12u);
        uint32_t table_offset = cff_u32(record + 8u);
        if (memcmp(record, "CFF ", 4u) == 0) {
            data_size += align4(new_cff_size);
        } else {
            if ((size_t)table_offset + length > font_size) return 0;
            data_size += align4(length);
        }
    }
    result = (uint8_t *)malloc(dir_size + data_size);
    if (!result) return 0;
    memcpy(result, font, 12u);
    offset = dir_size;
    for (table = 0u; table < table_count; ++table) {
        const uint8_t *record = font + 12u + table * 16u;
        uint8_t *target = result + 12u + table * 16u;
        memcpy(target, record, 16u);
        if (memcmp(record, "CFF ", 4u) == 0) {
            cff_put_u32(target + 8u, (uint32_t)offset);
            cff_put_u32(target + 12u, (uint32_t)new_cff_size);
            cff_put_u32(target + 4u, sfnt_checksum(new_cff, new_cff_size));
            memcpy(result + offset, new_cff, new_cff_size);
            offset += align4(new_cff_size);
        } else {
            uint32_t table_offset = cff_u32(record + 8u);
            uint32_t length = cff_u32(record + 12u);
            cff_put_u32(target + 8u, (uint32_t)offset);
            memcpy(result + offset, font + table_offset, length);
            offset += align4(length);
        }
    }
    *out = result;
    *out_size = dir_size + data_size;
    return 1;
}

int qw_pdf_cff_subset(const uint8_t *font, size_t font_size,
                      const uint16_t *gids, size_t count,
                      uint8_t **out_font, size_t *out_size, uint16_t *map,
                      unsigned *out_glyph_count)
{
    size_t table_count;
    size_t table;
    size_t cff_offset = 0u;
    size_t cff_length = 0u;
    const uint8_t *cff;
    size_t header_size;
    CffIndex name_index;
    CffIndex top_index;
    CffIndex string_index;
    CffIndex global_subrs;
    size_t top_length;
    const uint8_t *top_dict;
    uint32_t charstrings_value = 0u;
    uint32_t charset_value = 0u;
    uint32_t fdarray_value = 0u;
    uint32_t fdselect_value = 0u;
    uint32_t private_values[2] = {0u, 0u};
    size_t private_count = 0u;
    size_t value_count;
    int is_cid;
    CffIndex charstrings;
    size_t glyph_count;
    uint16_t *charset = NULL;
    uint8_t *fdselect = NULL;
    uint16_t *kept = NULL;
    size_t kept_count = 0u;
    uint8_t *charstrings_buf = NULL;
    size_t charstrings_size = 0u;
    uint8_t *charset_buf = NULL;
    size_t charset_size = 0u;
    uint8_t *fdselect_buf = NULL;
    size_t fdselect_size = 0u;
    CffIndex fdarray;
    size_t fd_count = 0u;
    uint8_t **fd_dicts = NULL;
    size_t *fd_lengths = NULL;
    size_t *private_lengths = NULL;
    size_t *private_offsets = NULL;
    uint8_t *private_buf = NULL;
    size_t private_total = 0u;
    uint8_t *fdarray_buf = NULL;
    size_t fdarray_size = 0u;
    uint8_t *new_cff = NULL;
    size_t new_cff_size = 0u;
    uint8_t *new_otf = NULL;
    size_t new_otf_size = 0u;
    int ok = 0;

    if (!font || !out_font || !out_size || !map || !out_glyph_count)
        return 0;
    *out_font = NULL;
    *out_size = 0u;
    *out_glyph_count = 0u;
    if (font_size < 12u) return 0;
    table_count = cff_u16(font + 4u);
    if (!table_count || 12u + table_count * 16u > font_size) return 0;
    for (table = 0u; table < table_count; ++table) {
        const uint8_t *record = font + 12u + table * 16u;
        if (memcmp(record, "CFF ", 4u) == 0) {
            cff_offset = cff_u32(record + 8u);
            cff_length = cff_u32(record + 12u);
            break;
        }
    }
    if (!cff_length || cff_offset + cff_length > font_size) return 0;
    cff = font + cff_offset;
    if (cff_length < 4u) return 0;
    header_size = cff[2];
    if (header_size < 4u || header_size >= cff_length) return 0;
    if (!cff_index(cff, cff_length, header_size, &name_index)) return 0;
    if (!cff_index(cff, cff_length, name_index.end, &top_index)) return 0;
    if (!top_index.count) return 0;
    top_dict = cff_index_object(cff, &top_index, 0u, &top_length);
    if (!top_dict) return 0;
    if (!cff_index(cff, cff_length, top_index.end, &string_index)) return 0;
    if (!cff_index(cff, cff_length, string_index.end, &global_subrs))
        return 0;
    value_count = 1u;
    if (!cff_dict_operands(top_dict, top_length, 17u, &charstrings_value,
                           &value_count) ||
        value_count != 1u)
        return 0;
    value_count = 1u;
    if (!cff_dict_operands(top_dict, top_length, 15u, &charset_value,
                           &value_count) ||
        value_count != 1u)
        return 0;
    value_count = 2u;
    if (cff_dict_operands(top_dict, top_length, 18u, private_values,
                          &value_count))
        private_count = value_count;
    value_count = 1u;
    if (cff_dict_operands(top_dict, top_length, 1236u, &fdarray_value,
                          &value_count) &&
        value_count == 1u &&
        cff_dict_operands(top_dict, top_length, 1237u, &fdselect_value,
                          &value_count) &&
        value_count == 1u)
        is_cid = 1;
    else
        is_cid = 0;
    if (!charstrings_value || !charset_value) return 0;
    if (!cff_index(cff, cff_length, charstrings_value, &charstrings))
        return 0;
    glyph_count = charstrings.count;
    if (glyph_count < 1u || glyph_count > CFF_MAX_GLYPHS) return 0;
    charset = (uint16_t *)malloc(glyph_count * sizeof(uint16_t));
    kept = (uint16_t *)malloc((count + 1u) * sizeof(uint16_t));
    if (!charset || !kept) goto done;
    if (!cff_charset(cff, cff_length, charset_value, glyph_count, charset))
        goto done;
    if (is_cid) {
        fdselect = (uint8_t *)malloc(glyph_count);
        if (!fdselect) goto done;
        if (!cff_fdselect(cff, cff_length, fdselect_value, glyph_count,
                          fdselect))
            goto done;
    }
    {
        size_t index;
        for (index = 0u; index < glyph_count; ++index) map[index] = 0xFFFFu;
    }
    kept[kept_count++] = 0u;
    {
        size_t index;
        for (index = 0u; index < count; ++index) {
            if (gids[index] >= glyph_count || gids[index] == 0u) continue;
            kept[kept_count++] = gids[index];
        }
    }
    {
        size_t i;
        for (i = 1u; i < kept_count; ++i) {
            uint16_t value = kept[i];
            size_t position = i;
            while (position > 0u && kept[position - 1u] > value) {
                kept[position] = kept[position - 1u];
                --position;
            }
            kept[position] = value;
        }
    }
    {
        size_t index;
        size_t unique = 0u;
        for (index = 0u; index < kept_count; ++index) {
            if (index == 0u || kept[index] != kept[unique - 1u])
                kept[unique++] = kept[index];
        }
        kept_count = unique;
    }
    for (table = 0u; table < kept_count; ++table)
        map[kept[table]] = (uint16_t)table;
    *out_glyph_count = (unsigned)kept_count;

    /* CharStrings INDEX */
    {
        const uint8_t **objects =
            (const uint8_t **)malloc(kept_count * sizeof(*objects));
        size_t *lengths = (size_t *)malloc(kept_count * sizeof(*lengths));
        size_t capacity = 0u;
        size_t index;
        if (!objects || !lengths) {
            free(objects);
            free(lengths);
            goto done;
        }
        for (index = 0u; index < kept_count; ++index) {
            objects[index] = cff_index_object(cff, &charstrings, kept[index],
                                              &lengths[index]);
            if (!objects[index]) {
                free(objects);
                free(lengths);
                goto done;
            }
            capacity += lengths[index];
        }
        charstrings_buf = (uint8_t *)malloc(capacity + kept_count * 5u + 16u);
        if (!charstrings_buf ||
            !cff_build_index(charstrings_buf, capacity + kept_count * 5u + 16u,
                             objects, lengths, kept_count, &charstrings_size)) {
            free(objects);
            free(lengths);
            goto done;
        }
        free(objects);
        free(lengths);
    }

    /* charset */
    charset_size = 1u + (kept_count > 1u ? 2u * (kept_count - 1u) : 0u);
    charset_buf = (uint8_t *)malloc(charset_size);
    if (!charset_buf) goto done;
    charset_buf[0] = 0u;
    {
        size_t index;
        for (index = 1u; index < kept_count; ++index) {
            uint16_t value = is_cid ? (uint16_t)index : charset[kept[index]];
            charset_buf[1u + (index - 1u) * 2u] = (uint8_t)(value >> 8);
            charset_buf[2u + (index - 1u) * 2u] = (uint8_t)value;
        }
    }

    /* FDSelect */
    if (is_cid) {
        size_t index;
        fdselect_size = 1u + kept_count;
        fdselect_buf = (uint8_t *)malloc(fdselect_size);
        if (!fdselect_buf) goto done;
        fdselect_buf[0] = 0u;
        for (index = 0u; index < kept_count; ++index)
            fdselect_buf[1u + index] = fdselect[kept[index]];
    }

    /* FDArray dictionaries and Private blocks */
    if (is_cid) {
        size_t fd;
        if (!cff_index(cff, cff_length, fdarray_value, &fdarray)) goto done;
        fd_count = fdarray.count;
    }
    if (is_cid || private_count == 2u) {
        size_t slots = is_cid ? fd_count : 1u;
        size_t slot;
        fd_dicts = (uint8_t **)calloc(slots, sizeof(uint8_t *));
        fd_lengths = (size_t *)calloc(slots, sizeof(size_t));
        private_lengths = (size_t *)calloc(slots, sizeof(size_t));
        private_offsets = (size_t *)calloc(slots, sizeof(size_t));
        if (!fd_dicts || !fd_lengths || !private_lengths || !private_offsets)
            goto done;
        if (is_cid) {
            for (slot = 0u; slot < fd_count; ++slot) {
                size_t dict_length;
                const uint8_t *dict =
                    cff_index_object(cff, &fdarray, slot, &dict_length);
                uint32_t values[2];
                size_t count2 = 2u;
                if (!dict ||
                    !cff_dict_operands(dict, dict_length, 18u, values,
                                       &count2) ||
                    count2 != 2u)
                    goto done;
                if (!cff_private_block(cff, cff_length, values[1], values[0],
                                       &private_lengths[slot]))
                    goto done;
                fd_dicts[slot] = (uint8_t *)malloc(dict_length + 64u);
                if (!fd_dicts[slot]) goto done;
                fd_lengths[slot] = dict_length;
            }
        } else {
            if (!cff_private_block(cff, cff_length, private_values[1],
                                   private_values[0], &private_lengths[0]))
                goto done;
        }
        {
            size_t position = 0u;
            for (slot = 0u; slot < slots; ++slot) {
                private_offsets[slot] = position;
                position += private_lengths[slot];
            }
            private_total = position;
        }
        private_buf = (uint8_t *)malloc(private_total ? private_total : 1u);
        if (!private_buf) goto done;
        if (is_cid) {
            for (slot = 0u; slot < fd_count; ++slot) {
                size_t dict_length;
                const uint8_t *dict =
                    cff_index_object(cff, &fdarray, slot, &dict_length);
                uint32_t values[2];
                size_t count2 = 2u;
                if (!cff_dict_operands(dict, dict_length, 18u, values,
                                       &count2))
                    goto done;
                memcpy(private_buf + private_offsets[slot], cff + values[1],
                       private_lengths[slot]);
            }
        } else {
            memcpy(private_buf, cff + private_values[1], private_lengths[0]);
        }
    }

    /* Layout */
    {
        size_t top_index_size;
        size_t charstrings_offset;
        size_t charset_offset;
        size_t fdselect_offset = 0u;
        size_t private_offset;
        size_t fdarray_offset = 0u;
        CffDictReplacement replacements[6];
        size_t replacement_count = 0u;
        uint8_t *top_scratch = NULL;
        size_t top_size = 0u;
        size_t off_size;
        size_t capacity = 0u;
        size_t slot;
        replacements[0].op = 15u;
        replacements[0].count = 1u;
        replacements[0].values[0] = 0u;
        replacements[1].op = 17u;
        replacements[1].count = 1u;
        replacements[1].values[0] = 0u;
        replacement_count = 2u;
        if (private_count == 2u) {
            replacements[replacement_count].op = 18u;
            replacements[replacement_count].count = 2u;
            replacements[replacement_count].values[0] = private_values[0];
            replacements[replacement_count].values[1] = 0u;
            ++replacement_count;
        }
        if (is_cid) {
            replacements[replacement_count].op = 1236u;
            replacements[replacement_count].count = 1u;
            replacements[replacement_count].values[0] = 0u;
            ++replacement_count;
            replacements[replacement_count].op = 1237u;
            replacements[replacement_count].count = 1u;
            replacements[replacement_count].values[0] = 0u;
            ++replacement_count;
        }
        top_scratch = (uint8_t *)malloc(top_length + replacement_count * 10u +
                                        64u);
        if (!top_scratch) goto done;
        if (!cff_rewrite_dict(top_dict, top_length, replacements,
                              replacement_count, top_scratch,
                              top_length + replacement_count * 10u + 64u,
                              &top_size))
            goto done;
        off_size = 1u;
        while (top_size + 1u > ((uint32_t)1u << (8u * off_size))) ++off_size;
        top_index_size = 3u + 2u * off_size + top_size;
        charstrings_offset = header_size + cff_index_length(&name_index) +
                             top_index_size + cff_index_length(&string_index) +
                             cff_index_length(&global_subrs);
        charset_offset = charstrings_offset + charstrings_size;
        if (is_cid) {
            fdselect_offset = charset_offset + charset_size;
            private_offset = fdselect_offset + fdselect_size;
        } else {
            private_offset = charset_offset + charset_size;
        }
        /* FD dictionaries with the absolute Private offsets. */
        if (is_cid) {
            for (slot = 0u; slot < fd_count; ++slot) {
                size_t dict_length;
                const uint8_t *dict =
                    cff_index_object(cff, &fdarray, slot, &dict_length);
                CffDictReplacement replacement;
                replacement.op = 18u;
                replacement.count = 2u;
                replacement.values[0] = (uint32_t)private_lengths[slot];
                replacement.values[1] =
                    (uint32_t)(private_offset + private_offsets[slot]);
                if (!cff_rewrite_dict(dict, dict_length, &replacement, 1u,
                                      fd_dicts[slot], dict_length + 64u,
                                      &fd_lengths[slot]))
                    goto done;
                capacity += fd_lengths[slot];
            }
            fdarray_buf = (uint8_t *)malloc(capacity + fd_count * 5u + 16u);
            if (!fdarray_buf ||
                !cff_build_index(fdarray_buf, capacity + fd_count * 5u + 16u,
                                 (const uint8_t **)fd_dicts, fd_lengths,
                                 fd_count, &fdarray_size))
                goto done;
            fdarray_offset = private_offset + private_total;
        }
        replacements[0].values[0] = (uint32_t)charset_offset;
        replacements[1].values[0] = (uint32_t)charstrings_offset;
        if (private_count == 2u) {
            size_t r;
            for (r = 0u; r < replacement_count; ++r) {
                if (replacements[r].op == 18u)
                    replacements[r].values[1] = (uint32_t)private_offset;
            }
        }
        if (is_cid) {
            size_t r;
            for (r = 0u; r < replacement_count; ++r) {
                if (replacements[r].op == 1236u)
                    replacements[r].values[0] = (uint32_t)fdarray_offset;
                if (replacements[r].op == 1237u)
                    replacements[r].values[0] = (uint32_t)fdselect_offset;
            }
        }
        if (!cff_rewrite_dict(top_dict, top_length, replacements,
                              replacement_count, top_scratch,
                              top_length + replacement_count * 10u + 64u,
                              &top_size))
            goto done;
        {
            uint8_t *top_index_buf =
                (uint8_t *)malloc(3u + 2u * 4u + top_size);
            size_t top_index_length = 0u;
            if (!top_index_buf) goto done;
            {
                const uint8_t *object = top_scratch;
                size_t length = top_size;
                if (!cff_build_index(top_index_buf, 3u + 2u * 4u + top_size,
                                     &object, &length, 1u,
                                     &top_index_length)) {
                    free(top_index_buf);
                    goto done;
                }
            }
            new_cff_size = header_size + cff_index_length(&name_index) +
                           top_index_length + cff_index_length(&string_index) +
                           cff_index_length(&global_subrs) + charstrings_size +
                           charset_size + fdselect_size + private_total +
                           fdarray_size;
            new_cff = (uint8_t *)malloc(new_cff_size ? new_cff_size : 1u);
            if (!new_cff) {
                free(top_index_buf);
                goto done;
            }
            {
                size_t position = 0u;
                memcpy(new_cff + position, cff, header_size);
                position += header_size;
                memcpy(new_cff + position, cff + name_index.start,
                       cff_index_length(&name_index));
                position += cff_index_length(&name_index);
                memcpy(new_cff + position, top_index_buf, top_index_length);
                position += top_index_length;
                memcpy(new_cff + position, cff + string_index.start,
                       cff_index_length(&string_index));
                position += cff_index_length(&string_index);
                memcpy(new_cff + position, cff + global_subrs.start,
                       cff_index_length(&global_subrs));
                position += cff_index_length(&global_subrs);
                memcpy(new_cff + position, charstrings_buf, charstrings_size);
                position += charstrings_size;
                memcpy(new_cff + position, charset_buf, charset_size);
                position += charset_size;
                if (is_cid) {
                    memcpy(new_cff + position, fdselect_buf, fdselect_size);
                    position += fdselect_size;
                }
                if (private_total) {
                    memcpy(new_cff + position, private_buf, private_total);
                    position += private_total;
                }
                if (is_cid) {
                    memcpy(new_cff + position, fdarray_buf, fdarray_size);
                    position += fdarray_size;
                }
                new_cff_size = position;
            }
            free(top_index_buf);
        }
        free(top_scratch);
    }

    if (!cff_wrap_sfnt(font, font_size, new_cff, new_cff_size, &new_otf,
                       &new_otf_size))
        goto done;
    *out_font = new_otf;
    *out_size = new_otf_size;
    new_otf = NULL;
    ok = 1;

done:
    if (fd_dicts) {
        size_t slot;
        for (slot = 0u; slot < fd_count; ++slot) free(fd_dicts[slot]);
        free(fd_dicts);
    }
    free(fd_lengths);
    free(private_lengths);
    free(private_offsets);
    free(charset);
    free(fdselect);
    free(kept);
    free(charstrings_buf);
    free(charset_buf);
    free(fdselect_buf);
    free(private_buf);
    free(fdarray_buf);
    free(new_cff);
    free(new_otf);
    return ok;
}
