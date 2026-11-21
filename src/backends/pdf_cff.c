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

/* ---------------------------------------------------------------------------
 * CFF2 -> CFF 1 conversion (static instances only)
 * ------------------------------------------------------------------------- */

typedef struct Cff2Index {
    size_t count;
    unsigned off_size;
    size_t offsets;
    size_t data;
    size_t end;
} Cff2Index;

/* CFF2 stores the INDEX count as a 32-bit value. */
static int cff2_index(const uint8_t *cff, size_t size, size_t pos,
                      Cff2Index *index)
{
    uint32_t last;
    if (pos + 5u > size) return 0;
    index->count = cff_u32(cff + pos);
    if (index->count == 0u) {
        index->off_size = 1u;
        index->offsets = pos + 5u;
        index->data = pos + 5u;
        index->end = pos + 5u;
        return 1;
    }
    index->off_size = cff[pos + 4u];
    if (index->off_size < 1u || index->off_size > 4u) return 0;
    index->offsets = pos + 5u;
    if (index->offsets + (index->count + 1u) * index->off_size > size)
        return 0;
    index->data = index->offsets + (index->count + 1u) * index->off_size;
    last = cff_read_offset(cff + index->offsets +
                               index->count * index->off_size,
                           index->off_size);
    if (last == 0u || index->data + last - 1u > size) return 0;
    index->end = index->data + last - 1u;
    return 1;
}

static const uint8_t *cff2_object(const uint8_t *cff, const Cff2Index *index,
                                  size_t object, size_t *length)
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

static int cff2_find_table(const uint8_t *font, size_t size, const char *tag,
                           size_t *offset, size_t *length)
{
    size_t count;
    size_t index;
    if (size < 12u) return 0;
    count = cff_u16(font + 4u);
    for (index = 0u; index < count; ++index) {
        const uint8_t *record = font + 12u + index * 16u;
        if (memcmp(record, tag, 4u) == 0) {
            uint32_t table_offset = cff_u32(record + 8u);
            uint32_t table_length = cff_u32(record + 12u);
            if ((size_t)table_offset + table_length > size) return 0;
            *offset = table_offset;
            *length = table_length;
            return 1;
        }
    }
    return 0;
}

static void cff2_put_fixed(uint8_t *p, uint32_t value)
{
    p[0] = 29u;
    cff_put_u32(p + 1u, value);
}

static int cff2_build_index(uint8_t *out, size_t capacity,
                            const uint8_t *const *objects,
                            const size_t *lengths, size_t count,
                            size_t *out_size)
{
    return cff_build_index(out, capacity, objects, lengths, count, out_size);
}

/* CFF2's vstore operator is 24, above the CFF 1 operator range, so the
   generic operand scanner stops on it. Walk the DICT with the CFF2 rules. */
static int cff2_has_vstore(const uint8_t *dict, size_t size)
{
    size_t pos = 0u;
    while (pos < size) {
        uint8_t b = dict[pos];
        if (b == 24u) return 1;
        if (b == 12u) {
            if (pos + 1u >= size) return 0;
            pos += 2u;
            continue;
        }
        if (b <= 21u) {
            pos += 1u;
            continue;
        }
        if (b == 28u) {
            pos += 3u;
            continue;
        }
        if (b == 29u) {
            pos += 5u;
            continue;
        }
        if (b == 30u) {
            pos += 1u;
            while (pos < size) {
                uint8_t value = dict[pos++];
                if ((value & 0x0fu) == 0x0fu || (value >> 4) == 0x0fu) break;
            }
            continue;
        }
        if (b >= 32u && b <= 246u) {
            pos += 1u;
            continue;
        }
        if (b >= 247u && b <= 254u) {
            pos += 2u;
            continue;
        }
        if (b == 255u) {
            pos += 5u;
            continue;
        }
        return 0;
    }
    return 0;
}
/* The Type2 width operand is only recognised in front of a stem, mask, moveto
   or endchar, so a program that starts with a drawing operator needs an empty
   rmoveto first. */
static unsigned cff2_first_operator(const uint8_t *program, size_t size)
{
    size_t pos = 0u;
    while (pos < size) {
        uint8_t b = program[pos];
        if (b == 12u) return pos + 1u < size ? 1200u + program[pos + 1u] : 0u;
        if (b <= 21u) return b;
        if (b == 28u) pos += 3u;
        else if (b == 29u) pos += 5u;
        else if (b == 30u) {
            pos += 1u;
            while (pos < size) {
                uint8_t value = program[pos++];
                if ((value & 0x0fu) == 0x0fu || (value >> 4) == 0x0fu) break;
            }
        } else if (b == 255u) pos += 5u;
        else if (b >= 32u && b <= 246u) pos += 1u;
        else if (b >= 247u && b <= 254u) pos += 2u;
        else return 0u;
    }
    return 0u;
}

static int cff2_width_allowed(unsigned op)
{
    return op == 1u || op == 3u || op == 18u || op == 19u || op == 20u ||
           op == 23u || op == 21u || op == 22u || op == 4u || op == 14u ||
           op == 0u;
}
static int cff2_flatten_program(const uint8_t *in, size_t in_size,
                                uint8_t **out, size_t *out_size);

static int cff2_advance(const uint8_t *font, size_t font_size, size_t gid)
{
    size_t hhea_offset = 0u;
    size_t hhea_size = 0u;
    size_t hmtx_offset = 0u;
    size_t hmtx_size = 0u;
    unsigned metrics;
    size_t entry;
    if (!cff2_find_table(font, font_size, "hhea", &hhea_offset, &hhea_size) ||
        hhea_size < 36u ||
        !cff2_find_table(font, font_size, "hmtx", &hmtx_offset, &hmtx_size))
        return 0;
    metrics = cff_u16(font + hhea_offset + 34u);
    if (!metrics || hmtx_size < 2u) return 0;
    entry = gid < metrics ? gid : (size_t)(metrics - 1u);
    if (entry * 4u + 2u > hmtx_size) return 0;
    return (int)cff_u16(font + hmtx_offset + entry * 4u);
}

static size_t cff2_width_length(int advance)
{
    if (advance >= -107 && advance <= 107) return 1u;
    if (advance >= -32768 && advance <= 32767) return 3u;
    return 5u;
}

static size_t cff2_width_write(uint8_t *out, int advance)
{
    size_t length = cff2_width_length(advance);
    if (length == 1u) {
        out[0] = (uint8_t)(advance + 139);
    } else if (length == 3u) {
        out[0] = 28u;
        out[1] = (uint8_t)((advance >> 8) & 0xFF);
        out[2] = (uint8_t)(advance & 0xFF);
    } else {
        int32_t fixed_value = (int32_t)((double)advance * 65536.0);
        out[0] = 255u;
        cff_put_u32(out + 1u, (uint32_t)fixed_value);
    }
    return length;
}

int qw_pdf_cff2_to_cff(const uint8_t *font, size_t font_size,
                       uint8_t **out_font, size_t *out_size)
{
    static const char name[] = "QwCFF2";
    static const char registry_text[] = "Adobe";
    static const char ordering_text[] = "Identity";
    size_t cff2_offset = 0u;
    size_t cff2_size = 0u;
    size_t header_size;
    size_t top_dict_size;
    size_t top_dict_offset;
    const uint8_t *top_dict;
    uint32_t values[8];
    size_t value_count;
    size_t charstrings_offset;
    size_t private_offset = 0u;
    size_t private_size = 0u;
    Cff2Index charstrings_index;
    Cff2Index subrs_index;
    size_t glyph_count;
    size_t glyph;
    size_t charstrings_total = 0u;
    size_t charstrings_off_size = 1u;
    size_t name_len;
    size_t string_len;
    size_t top_index_len;
    size_t charstrings_len;
    size_t charset_len;
    size_t fdselect_len;
    size_t fdarray_len;
    size_t private_block = 0u;
    size_t cff_size = 0u;
    size_t offset;
    int variable = 0;
    size_t body;
    uint8_t *cff = NULL;
    const uint8_t *objects[2];
    size_t lengths[2];
    size_t part;
    /* Top DICT: ROS (2-byte op) 5+5+5+2, charset 5+1, CharStrings 5+1,
       FDArray 5+2, FDSelect 5+2. Every number is the fixed 5-byte form, so the
       length does not depend on the offsets it will hold. */
    const size_t top_dict_len = 17u + 6u + 6u + 7u + 7u;
    const size_t font_dict_len = 11u;

    if (!out_font || !out_size) return 0;
    *out_font = NULL;
    *out_size = 0u;
    if (!font || font_size < 12u) return 0;
    if (!cff2_find_table(font, font_size, "CFF2", &cff2_offset, &cff2_size))
        return 0;
    if (cff2_offset + cff2_size > font_size || cff2_size < 5u) return 0;
    {
        const uint8_t *cff2 = font + cff2_offset;
        header_size = cff2[2];
        top_dict_size = cff_u16(cff2 + 3u);
        if (header_size < 5u || header_size + top_dict_size > cff2_size)
            return 0;
        top_dict_offset = header_size;
        top_dict = cff2 + top_dict_offset;
        if (cff2_has_vstore(top_dict, top_dict_size)) variable = 1;
        value_count = 8u;
        if (!cff_dict_operands(top_dict, top_dict_size, 17u, values,
                               &value_count) ||
            value_count != 1u)
            return 0;
        charstrings_offset = values[0];
        value_count = 8u;
        if (cff_dict_operands(top_dict, top_dict_size, 18u, values,
                              &value_count) &&
            value_count == 2u) {
            private_size = values[0];
            private_offset = values[1];
        }
        value_count = 8u;
        if (cff_dict_operands(top_dict, top_dict_size, 1236u, values,
                              &value_count) ||
            cff_dict_operands(top_dict, top_dict_size, 1237u, values,
                              &value_count))
            return 0; /* fonts with their own FDArray need a real instancer */
        if (charstrings_offset >= cff2_size ||
            !cff2_index(cff2, cff2_size, charstrings_offset,
                        &charstrings_index) ||
            !charstrings_index.count)
            return 0;
        if (private_size &&
            (private_offset >= cff2_size ||
             private_offset + private_size > cff2_size))
            return 0;
        if (header_size + top_dict_size < cff2_size) {
            if (!cff2_index(cff2, cff2_size, header_size + top_dict_size,
                            &subrs_index))
                return 0;
            if (subrs_index.count) return 0; /* global subrs need flattening */
        }
        glyph_count = charstrings_index.count;
        for (glyph = 0u; glyph < glyph_count; ++glyph) {
            size_t length = 0u;
            const uint8_t *program =
                cff2_object(cff2, &charstrings_index, glyph, &length);
            if (!program) return 0;
            if (variable) {
                uint8_t *flat = NULL;
                size_t flat_size = 0u;
                unsigned op;
                if (!cff2_flatten_program(program, length, &flat, &flat_size))
                    return 0;
                op = cff2_first_operator(flat, flat_size);
                free(flat);
                length = flat_size;
                if (!cff2_width_allowed(op)) length += 3u; /* 0 0 rmoveto */
            }
            charstrings_total +=
                cff2_width_length(cff2_advance(font, font_size, glyph)) +
                length + 1u; /* width operand + endchar */
        }
        while (charstrings_total + 1u > ((uint32_t)1u << (8u *
                                                           charstrings_off_size)))
            ++charstrings_off_size;
    }

    name_len = 2u + 1u + 2u * 1u + (sizeof(name) - 1u);
    string_len = 2u + 1u + 3u * 1u + (sizeof(registry_text) - 1u) +
                 (sizeof(ordering_text) - 1u);
    top_index_len = 2u + 1u + 2u * 1u + top_dict_len;
    charstrings_len = 2u + 1u + (glyph_count + 1u) * charstrings_off_size +
                      charstrings_total;
    charset_len = 1u + (glyph_count - 1u) * 2u;
    fdselect_len = 1u + glyph_count;
    fdarray_len = 2u + 1u + 2u * 1u + font_dict_len;

    offset = 4u;                 /* header */
    offset += name_len;
    {
        size_t top_index_start = offset;
        size_t strings_start = top_index_start + top_index_len;
        size_t subrs_start = strings_start + string_len;
        size_t charstrings_start = subrs_start + 2u;
        size_t charset_start = charstrings_start + charstrings_len;
        size_t fdselect_start = charset_start + charset_len;
        size_t fdarray_start = fdselect_start + fdselect_len;
        size_t private_start = fdarray_start + fdarray_len;
        cff_size = private_start + private_size;
        size_t pos;
        uint8_t *dict;

        cff = (uint8_t *)calloc(cff_size, 1u);
        if (!cff) return 0;
        /* header */
        cff[0] = 1u;
        cff[1] = 0u;
        cff[2] = 4u;
        cff[3] = 1u;
        /* Name INDEX */
        pos = 4u;
        cff[pos++] = 0u;
        cff[pos++] = 1u;
        cff[pos++] = 1u;
        cff[pos++] = 1u;
        cff[pos++] = (uint8_t)(1u + sizeof(name) - 1u);
        memcpy(cff + pos, name, sizeof(name) - 1u);
        pos += sizeof(name) - 1u;
        /* Top DICT INDEX */
        cff[pos++] = 0u;
        cff[pos++] = 1u;
        cff[pos++] = 1u;
        cff[pos++] = 1u;
        cff[pos++] = (uint8_t)(1u + top_dict_len);
        dict = cff + pos;
        {
            size_t at = 0u;
            cff2_put_fixed(dict + at, 391u); at += 5u;   /* registry */
            cff2_put_fixed(dict + at, 392u); at += 5u;   /* ordering */
            cff2_put_fixed(dict + at, 0u);   at += 5u;   /* supplement */
            dict[at++] = 12u;
            dict[at++] = 30u;
            cff2_put_fixed(dict + at, (uint32_t)charset_start); at += 5u;
            dict[at++] = 15u;
            cff2_put_fixed(dict + at, (uint32_t)charstrings_start); at += 5u;
            dict[at++] = 17u;
            cff2_put_fixed(dict + at, (uint32_t)fdarray_start); at += 5u;
            dict[at++] = 12u;
            dict[at++] = 36u;
            cff2_put_fixed(dict + at, (uint32_t)fdselect_start); at += 5u;
            dict[at++] = 12u;
            dict[at++] = 37u;
            if (at != top_dict_len) { free(cff); cff = NULL; return 0; }
        }
        pos += top_dict_len;
        /* String INDEX: registry + ordering */
        cff[pos++] = 0u;
        cff[pos++] = 2u;
        cff[pos++] = 1u;
        cff[pos++] = 1u;
        cff[pos++] = (uint8_t)(1u + sizeof(registry_text) - 1u);
        cff[pos++] = (uint8_t)(1u + (sizeof(registry_text) - 1u) +
                               (sizeof(ordering_text) - 1u));
        memcpy(cff + pos, registry_text, sizeof(registry_text) - 1u);
        pos += sizeof(registry_text) - 1u;
        memcpy(cff + pos, ordering_text, sizeof(ordering_text) - 1u);
        pos += sizeof(ordering_text) - 1u;
        /* Global Subr INDEX: empty */
        cff[pos++] = 0u;
        cff[pos++] = 0u;
        /* CharStrings INDEX, with endchar appended to each program */
        cff[pos++] = 0u;
        cff[pos++] = (uint8_t)glyph_count;
        cff[pos++] = (uint8_t)charstrings_off_size;
        {
            uint32_t running = 1u;
            size_t gid;
            size_t entry = 0u;
            for (gid = 0u; gid < glyph_count; ++gid) {
                size_t length = 0u;
                size_t width_bytes = 0u;
                cff2_object(font + cff2_offset, &charstrings_index, gid,
                            &length);
                cff_write_offset(cff + pos + entry * charstrings_off_size,
                                 running, (unsigned)charstrings_off_size);
                width_bytes =
                    cff2_width_length(cff2_advance(font, font_size, gid));
                running += (uint32_t)(width_bytes + length + 1u);
                ++entry;
            }
            cff_write_offset(cff + pos + entry * charstrings_off_size, running,
                             (unsigned)charstrings_off_size);
        }
        pos += (glyph_count + 1u) * charstrings_off_size;
        {
            size_t gid;
            for (gid = 0u; gid < glyph_count; ++gid) {
                size_t length = 0u;
                const uint8_t *program =
                    cff2_object(font + cff2_offset, &charstrings_index, gid,
                                &length);
                pos += cff2_width_write(cff + pos,
                                          cff2_advance(font, font_size, gid));
                {
                    const uint8_t *first = program;
                    size_t first_size = length;
                    unsigned op;
                    if (variable) {
                        uint8_t *probe = NULL;
                        size_t probe_size = 0u;
                        if (!cff2_flatten_program(program, length, &probe,
                                                  &probe_size)) {
                            free(cff);
                            cff = NULL;
                            return 0;
                        }
                        first = probe;
                        first_size = probe_size;
                        op = cff2_first_operator(probe, probe_size);
                        free(probe);
                    } else {
                        op = cff2_first_operator(program, length);
                    }
                    (void)first;
                    (void)first_size;
                    if (!cff2_width_allowed(op)) {
                        cff[pos++] = 139u; /* 0 */
                        cff[pos++] = 139u; /* 0 */
                        cff[pos++] = 21u;  /* rmoveto */
                    }
                }
                if (variable) {
                    uint8_t *flat = NULL;
                    size_t flat_size = 0u;
                    if (!cff2_flatten_program(program, length, &flat,
                                              &flat_size)) {
                        free(cff);
                        cff = NULL;
                        return 0;
                    }
                    memcpy(cff + pos, flat, flat_size);
                    pos += flat_size;
                    free(flat);
                } else {
                    memcpy(cff + pos, program, length);
                    pos += length;
                }
                cff[pos++] = 14u; /* endchar */
            }
        }
        /* charset: CID 0 for .notdef, then 1..n-1 */
        cff[pos++] = 0u;
        {
            size_t gid;
            for (gid = 1u; gid < glyph_count; ++gid) {
                cff[pos++] = (uint8_t)(gid >> 8);
                cff[pos++] = (uint8_t)gid;
            }
        }
        /* FDSelect: every glyph uses FD 0 */
        cff[pos++] = 0u;
        memset(cff + pos, 0, glyph_count);
        pos += glyph_count;
        /* FDArray: one Font DICT with the Private block */
        cff[pos++] = 0u;
        cff[pos++] = 1u;
        cff[pos++] = 1u;
        cff[pos++] = 1u;
        cff[pos++] = (uint8_t)(1u + font_dict_len);
        cff2_put_fixed(cff + pos, (uint32_t)private_size);
        pos += 5u;
        cff2_put_fixed(cff + pos, (uint32_t)private_start);
        pos += 5u;
        cff[pos++] = 18u;
        /* Private block */
        if (private_size) {
            memcpy(cff + pos, font + cff2_offset + private_offset,
                   private_size);
            pos += private_size;
        }
        if (pos != cff_size) { free(cff); cff = NULL; return 0; }
        private_block = private_size;
        (void)private_block;
    }

    objects[0] = cff;
    lengths[0] = 0u;
    {
        /* Rebuild the sfnt with the converted table renamed to "CFF ". */
        size_t table_count;
        size_t table;
        size_t dir_size;
        size_t data_size = 0u;
        uint8_t *result;
        size_t result_offset;
        table_count = cff_u16(font + 4u);
        dir_size = 12u + table_count * 16u;
        for (table = 0u; table < table_count; ++table) {
            const uint8_t *record = font + 12u + table * 16u;
            uint32_t length = cff_u32(record + 12u);
            if (memcmp(record, "CFF2", 4u) == 0)
                data_size += align4(cff_size);
            else
                data_size += align4(length);
        }
        result = (uint8_t *)malloc(dir_size + data_size);
        if (!result) { free(cff); return 0; }
        memcpy(result, font, 12u);
        result_offset = dir_size;
        for (table = 0u; table < table_count; ++table) {
            const uint8_t *record = font + 12u + table * 16u;
            uint8_t *target = result + 12u + table * 16u;
            memcpy(target, record, 16u);
            if (memcmp(record, "CFF2", 4u) == 0) {
                memcpy(target, "CFF ", 4u);
                cff_put_u32(target + 8u, (uint32_t)result_offset);
                cff_put_u32(target + 12u, (uint32_t)cff_size);
                cff_put_u32(target + 4u, sfnt_checksum(cff, cff_size));
                memcpy(result + result_offset, cff, cff_size);
                result_offset += align4(cff_size);
            } else {
                uint32_t table_offset = cff_u32(record + 8u);
                uint32_t length = cff_u32(record + 12u);
                cff_put_u32(target + 8u, (uint32_t)result_offset);
                memcpy(result + result_offset, font + table_offset, length);
                result_offset += align4(length);
            }
        }
        free(cff);
        *out_font = result;
        *out_size = dir_size + data_size;
        lengths[0] = cff_size;
        (void)part;
        return 1;
    }
}
/* --- Type2 charstring flattening for variable CFF2 -------------------------
 * A variable CFF2 charstring can contain `blend`; at the default instance the
 * deltas all evaluate to zero, so flattening means keeping the first
 * numBlends operands and dropping the operator. Operands are copied verbatim,
 * so only their spans (and integer values, for numBlends) are tracked. */

typedef struct Cff2Operand {
    size_t start;
    size_t length;
    int32_t value;
} Cff2Operand;

#define CFF2_STACK_LIMIT 96u

static int cff2_operand_at(const uint8_t *in, size_t in_size, size_t pos,
                           Cff2Operand *operand, size_t *next)
{
    uint8_t b = in[pos];
    if (b == 28u) {
        if (pos + 3u > in_size) return 0;
        operand->value = (int32_t)(int16_t)(((uint16_t)in[pos + 1u] << 8) |
                                            in[pos + 2u]);
        operand->start = pos;
        operand->length = 3u;
        *next = pos + 3u;
        return 1;
    }
    if (b == 29u) {
        if (pos + 5u > in_size) return 0;
        operand->value = (int32_t)(((uint32_t)in[pos + 1u] << 24) |
                                   ((uint32_t)in[pos + 2u] << 16) |
                                   ((uint32_t)in[pos + 3u] << 8) |
                                   in[pos + 4u]);
        operand->start = pos;
        operand->length = 5u;
        *next = pos + 5u;
        return 1;
    }
    if (b == 30u) {
        size_t at = pos + 1u;
        while (at < in_size) {
            uint8_t value = in[at++];
            if ((value & 0x0fu) == 0x0fu || (value >> 4) == 0x0fu) break;
        }
        operand->value = 0;
        operand->start = pos;
        operand->length = at - pos;
        *next = at;
        return 1;
    }
    if (b == 255u) {
        int32_t fixed;
        if (pos + 5u > in_size) return 0;
        fixed = (int32_t)(((uint32_t)in[pos + 1u] << 24) |
                          ((uint32_t)in[pos + 2u] << 16) |
                          ((uint32_t)in[pos + 3u] << 8) | in[pos + 4u]);
        operand->value = fixed / 65536;
        operand->start = pos;
        operand->length = 5u;
        *next = pos + 5u;
        return 1;
    }
    if (b >= 32u && b <= 246u) {
        operand->value = (int32_t)b - 139;
        operand->start = pos;
        operand->length = 1u;
        *next = pos + 1u;
        return 1;
    }
    if (b >= 247u && b <= 250u) {
        if (pos + 2u > in_size) return 0;
        operand->value = (int32_t)(b - 247u) * 256 + in[pos + 1u] + 108;
        operand->start = pos;
        operand->length = 2u;
        *next = pos + 2u;
        return 1;
    }
    if (b >= 251u && b <= 254u) {
        if (pos + 2u > in_size) return 0;
        operand->value = -((int32_t)(b - 251u) * 256) - in[pos + 1u] - 108;
        operand->start = pos;
        operand->length = 2u;
        *next = pos + 2u;
        return 1;
    }
    return 0;
}

static int cff2_flatten_program(const uint8_t *in, size_t in_size,
                                uint8_t **out, size_t *out_size)
{
    Cff2Operand stack[CFF2_STACK_LIMIT];
    size_t stack_count = 0u;
    size_t stems = 0u;
    size_t pos = 0u;
    size_t used = 0u;
    uint8_t *buffer = (uint8_t *)malloc(in_size + 1u);
    if (!buffer) return 0;
    while (pos < in_size) {
        uint8_t b = in[pos];
        if (b == 12u) {
            size_t index;
            if (pos + 1u >= in_size) goto fail;
            for (index = 0u; index < stack_count; ++index)
                memcpy(buffer + used, in + stack[index].start,
                       stack[index].length), used += stack[index].length;
            buffer[used++] = 12u;
            buffer[used++] = in[pos + 1u];
            stack_count = 0u;
            pos += 2u;
            continue;
        }
        if (b <= 21u) {
            switch (b) {
            case 16u: { /* blend: keep the default operands only */
                size_t index;
                int32_t count;
                size_t rest;
                if (stack_count == 0u) goto fail;
                count = stack[stack_count - 1u].value;
                if (count <= 0 || (size_t)count > stack_count - 1u) goto fail;
                rest = stack_count - 1u - (size_t)count;
                if (rest % (size_t)count != 0u) goto fail;
                for (index = 0u; index < (size_t)count; ++index) {
                    memcpy(buffer + used, in + stack[index].start,
                           stack[index].length);
                    used += stack[index].length;
                }
                stack_count = (size_t)count;
                pos += 1u;
                break;
            }
            case 19u:
            case 20u: { /* hintmask/cntrmask: operand stack holds vstems */
                size_t index;
                size_t mask_bytes;
                if (stack_count) stems += stack_count / 2u;
                for (index = 0u; index < stack_count; ++index) {
                    memcpy(buffer + used, in + stack[index].start,
                           stack[index].length);
                    used += stack[index].length;
                }
                buffer[used++] = b;
                mask_bytes = (stems + 7u) / 8u;
                if (pos + 1u + mask_bytes > in_size) goto fail;
                memcpy(buffer + used, in + pos + 1u, mask_bytes);
                used += mask_bytes;
                stack_count = 0u;
                pos += 1u + mask_bytes;
                break;
            }
            case 10u:
            case 29u: /* callsubr/callgsubr: subr programs are not flattened */
                goto fail;
            case 1u:
            case 3u:
            case 18u:
            case 23u: {
                size_t index;
                for (index = 0u; index < stack_count; ++index) {
                    memcpy(buffer + used, in + stack[index].start,
                           stack[index].length);
                    used += stack[index].length;
                }
                buffer[used++] = b;
                stems += stack_count / 2u;
                stack_count = 0u;
                pos += 1u;
                break;
            }
            default: {
                size_t index;
                for (index = 0u; index < stack_count; ++index) {
                    memcpy(buffer + used, in + stack[index].start,
                           stack[index].length);
                    used += stack[index].length;
                }
                buffer[used++] = b;
                stack_count = 0u;
                pos += 1u;
                break;
            }
            }
            continue;
        }
        if (cff2_operand_at(in, in_size, pos, &stack[stack_count], &pos)) {
            if (stack_count + 1u >= CFF2_STACK_LIMIT) goto fail;
            ++stack_count;
            continue;
        }
        goto fail;
    }
    if (stack_count) goto fail;
    *out = buffer;
    *out_size = used;
    return 1;
fail:
    free(buffer);
    return 0;
}
