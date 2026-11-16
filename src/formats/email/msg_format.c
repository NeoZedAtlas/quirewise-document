/* Outlook MSG reader: minimal CFB/MAPI property and attachment extraction.
 *
 * The compound file parser intentionally implements only the read paths MSG
 * files use: FAT, mini FAT, directory streams and named property streams.
 */

#include "msg_format.h"

#include "text_buffer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MSG_MAX_INPUT_BYTES = 64 * 1024 * 1024,
    MSG_MAX_DIRECTORY_ENTRIES = 4096,
    MSG_MAX_ATTACHMENTS = 32,
    MSG_MAX_STREAM_BYTES = 32 * 1024 * 1024,
    MSG_MAX_EMBED_BYTES = 32 * 1024 * 1024,
    MSG_MAX_ATTACHMENT_ENTRIES = 256
};

#define MSG_CFB_SIGNATURE "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1"
#define MSG_SECTOR_FREE 0xFFFFFFFFu
#define MSG_SECTOR_END 0xFFFFFFFEu
#define MSG_SECTOR_FAT 0xFFFFFFFDu
#define MSG_SECTOR_DIFAT 0xFFFFFFFCu

typedef struct MsgDirEntry {
    char name[96];
    uint16_t type;
    uint32_t left;
    uint32_t right;
    uint32_t child;
    uint32_t start;
    uint64_t size;
} MsgDirEntry;

typedef struct MsgCfb {
    const uint8_t *data;
    size_t size;
    unsigned sector_size;
    unsigned mini_sector_size;
    uint32_t *fat;
    size_t fat_count;
    uint32_t *mini_fat;
    size_t mini_fat_count;
    MsgDirEntry *directory;
    size_t directory_count;
    uint32_t mini_stream_start;
    uint64_t mini_stream_size;
    uint8_t *mini_stream;
    size_t mini_stream_bytes;
    uint32_t mini_cutoff;
} MsgCfb;

static uint16_t msg_rd16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t msg_rd32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t msg_rd64(const uint8_t *data)
{
    return (uint64_t)msg_rd32(data) |
           ((uint64_t)msg_rd32(data + 4) << 32);
}

static size_t msg_sector_offset(const MsgCfb *cfb, uint32_t sector)
{
    size_t base = cfb->sector_size == 512u ? 512u : (size_t)cfb->sector_size;
    return base + (size_t)sector * (size_t)cfb->sector_size;
}

static int msg_sector_available(const MsgCfb *cfb, uint32_t sector)
{
    size_t offset;
    if (sector >= 0xFFFFFFFAu) return 0;
    offset = msg_sector_offset(cfb, sector);
    return offset + cfb->sector_size <= cfb->size;
}

static void msg_cfb_dispose(MsgCfb *cfb)
{
    if (!cfb) return;
    free(cfb->fat);
    free(cfb->mini_fat);
    free(cfb->directory);
    free(cfb->mini_stream);
    memset(cfb, 0, sizeof(*cfb));
}

static int msg_cfb_open(MsgCfb *cfb, const uint8_t *data, size_t size)
{
    uint16_t shift;
    uint16_t mini_shift;
    uint32_t fat_sector_count;
    uint32_t first_directory;
    uint32_t first_mini_fat;
    uint32_t mini_fat_sectors;
    uint32_t first_difat;
    uint32_t difat_sectors;
    uint32_t sector_count;
    uint32_t fat_ids[65536];
    size_t fat_id_count = 0u;
    uint32_t index;
    uint32_t sector;
    size_t entries_per_sector;
    size_t i;

    memset(cfb, 0, sizeof(*cfb));
    if (!data || size < 512u || memcmp(data, MSG_CFB_SIGNATURE, 8u) != 0)
        return 0;
    if (size > MSG_MAX_INPUT_BYTES) return 0;
    shift = msg_rd16(data + 30u);
    mini_shift = msg_rd16(data + 32u);
    if (shift < 7u || shift > 12u || mini_shift < 6u || mini_shift > 9u)
        return 0;
    cfb->data = data;
    cfb->size = size;
    cfb->sector_size = 1u << shift;
    cfb->mini_sector_size = 1u << mini_shift;
    fat_sector_count = msg_rd32(data + 44u);
    first_directory = msg_rd32(data + 48u);
    cfb->mini_cutoff = msg_rd32(data + 56u);
    first_mini_fat = msg_rd32(data + 60u);
    mini_fat_sectors = msg_rd32(data + 64u);
    first_difat = msg_rd32(data + 68u);
    difat_sectors = msg_rd32(data + 72u);
    if (fat_sector_count == 0u ||
        fat_sector_count > MSG_MAX_DIRECTORY_ENTRIES * 8u)
        return 0;
    sector_count = (uint32_t)((size - 512u) / cfb->sector_size);

    for (i = 0u; i < 109u && fat_id_count < 65536u; ++i) {
        uint32_t id = msg_rd32(data + 76u + i * 4u);
        if (id == MSG_SECTOR_END || id == MSG_SECTOR_FREE) continue;
        if (id >= sector_count) return 0;
        fat_ids[fat_id_count++] = id;
    }
    sector = first_difat;
    for (i = 0u; i < difat_sectors && fat_id_count < 65536u; ++i) {
        size_t offset = msg_sector_offset(cfb, sector);
        uint32_t j;
        if (sector >= sector_count || offset + cfb->sector_size > size)
            return 0;
        for (j = 0u; j + 1u < cfb->sector_size / 4u; ++j) {
            uint32_t id = msg_rd32(data + offset + j * 4u);
            if (id == MSG_SECTOR_END || id == MSG_SECTOR_FREE) continue;
            if (id >= sector_count || fat_id_count >= 65536u) return 0;
            fat_ids[fat_id_count++] = id;
        }
        sector = msg_rd32(data + offset + cfb->sector_size - 4u);
        if (sector == MSG_SECTOR_END) break;
    }
    if (fat_id_count != fat_sector_count) return 0;
    entries_per_sector = cfb->sector_size / 4u;
    cfb->fat_count = (size_t)fat_sector_count * entries_per_sector;
    cfb->fat = (uint32_t *)malloc(cfb->fat_count * sizeof(uint32_t));
    if (!cfb->fat) return 0;
    for (i = 0u; i < fat_sector_count; ++i) {
        size_t offset = msg_sector_offset(cfb, fat_ids[i]);
        size_t j;
        if (offset + cfb->sector_size > size) return 0;
        for (j = 0u; j < entries_per_sector; ++j)
            cfb->fat[i * entries_per_sector + j] =
                msg_rd32(data + offset + j * 4u);
    }

    /* Directory chain. */
    {
        uint32_t *entries = NULL;
        size_t count = 0u;
        size_t capacity = 0u;
        sector = first_directory;
        while (sector != MSG_SECTOR_END && sector != MSG_SECTOR_FREE) {
            size_t offset;
            uint32_t next;
            if (!msg_sector_available(cfb, sector)) goto directory_fail;
            offset = msg_sector_offset(cfb, sector);
            if (count + entries_per_sector * 4u > capacity) {
                size_t grown_capacity =
                    capacity ? capacity * 2u : entries_per_sector * 4u;
                uint32_t *grown;
                if (grown_capacity > MSG_MAX_DIRECTORY_ENTRIES)
                    grown_capacity = MSG_MAX_DIRECTORY_ENTRIES;
                grown = (uint32_t *)realloc(entries,
                                            grown_capacity * sizeof(uint32_t));
                if (!grown) goto directory_fail;
                entries = grown;
                capacity = grown_capacity;
            }
            if (sector >= cfb->fat_count) goto directory_fail;
            for (i = 0u; i < cfb->sector_size / 128u; ++i) {
                size_t base = offset + i * 128u;
                uint16_t name_length;
                char name[96];
                size_t j;
                uint16_t type;
                if (count >= MSG_MAX_DIRECTORY_ENTRIES) break;
                name_length = msg_rd16(data + base + 64u);
                if (name_length >= 2u) {
                    size_t chars = (size_t)(name_length - 2u) / 2u;
                    if (chars > sizeof(name) - 1u)
                        chars = sizeof(name) - 1u;
                    for (j = 0u; j < chars; ++j) {
                        uint16_t ch = msg_rd16(data + base + j * 2u);
                        name[j] = (ch > 0u && ch < 128u) ? (char)ch : '?';
                    }
                    name[j] = '\0';
                } else {
                    name[0] = '\0';
                }
                type = data[base + 66u];
                if (type == 0u && name[0] == '\0') continue;
                entries[count] = (uint32_t)count; /* placeholder index */
                if (!cfb->directory) {
                    cfb->directory = (MsgDirEntry *)calloc(
                        MSG_MAX_DIRECTORY_ENTRIES, sizeof(MsgDirEntry));
                    if (!cfb->directory) goto directory_fail;
                }
                {
                    MsgDirEntry *entry = &cfb->directory[count];
                    memcpy(entry->name, name, strlen(name) + 1u);
                    entry->type = type;
                    entry->left = msg_rd32(data + base + 68u);
                    entry->right = msg_rd32(data + base + 72u);
                    entry->child = msg_rd32(data + base + 76u);
                    entry->start = msg_rd32(data + base + 116u);
                    entry->size = msg_rd64(data + base + 120u);
                }
                ++count;
            }
            next = (sector < cfb->fat_count) ? cfb->fat[sector]
                                             : MSG_SECTOR_END;
            sector = next;
        }
        free(entries);
        cfb->directory_count = count;
        if (count == 0u) goto directory_fail;
    }

    /* Mini FAT and the root mini stream. */
    if (mini_fat_sectors > 0u && first_mini_fat != MSG_SECTOR_END) {
        entries_per_sector = cfb->sector_size / 4u;
        cfb->mini_fat_count =
            (size_t)mini_fat_sectors * entries_per_sector;
        cfb->mini_fat = (uint32_t *)malloc(cfb->mini_fat_count *
                                           sizeof(uint32_t));
        if (!cfb->mini_fat) return 0;
        sector = first_mini_fat;
        i = 0u;
        while (sector != MSG_SECTOR_END && sector != MSG_SECTOR_FREE &&
               i < cfb->mini_fat_count) {
            size_t offset;
            size_t j;
            if (!msg_sector_available(cfb, sector)) return 0;
            offset = msg_sector_offset(cfb, sector);
            for (j = 0u; j < entries_per_sector && i < cfb->mini_fat_count;
                 ++j, ++i)
                cfb->mini_fat[i] = msg_rd32(data + offset + j * 4u);
            if (sector >= cfb->fat_count) return 0;
            sector = cfb->fat[sector];
        }
    }
    for (i = 0u; i < cfb->directory_count; ++i) {
        if (cfb->directory[i].type == 5u) {
            cfb->mini_stream_start = cfb->directory[i].start;
            cfb->mini_stream_size = cfb->directory[i].size;
            break;
        }
    }
    if (cfb->mini_stream_size > 0u) {
        size_t remaining = (size_t)cfb->mini_stream_size;
        size_t written = 0u;
        if (remaining > MSG_MAX_STREAM_BYTES ||
            remaining > cfb->size - 512u)
            return 0;
        cfb->mini_stream = (uint8_t *)malloc(remaining);
        if (!cfb->mini_stream) return 0;
        sector = cfb->mini_stream_start;
        while (sector != MSG_SECTOR_END && sector != MSG_SECTOR_FREE &&
               written < remaining) {
            size_t offset;
            size_t chunk = remaining - written;
            if (chunk > cfb->sector_size) chunk = cfb->sector_size;
            if (!msg_sector_available(cfb, sector)) return 0;
            offset = msg_sector_offset(cfb, sector);
            memcpy(cfb->mini_stream + written, data + offset, chunk);
            written += chunk;
            if (sector >= cfb->fat_count) return 0;
            sector = cfb->fat[sector];
        }
        cfb->mini_stream_bytes = written;
    }
    return 1;

directory_fail:
    return 0;
}

static uint8_t *msg_read_stream(const MsgCfb *cfb, size_t index,
                                size_t *out_size)
{
    const MsgDirEntry *entry;
    uint8_t *output;
    size_t remaining;
    size_t written = 0u;
    uint32_t sector;

    if (out_size) *out_size = 0u;
    if (!cfb || index >= cfb->directory_count) return NULL;
    entry = &cfb->directory[index];
    if (entry->type != 2u || entry->size == 0u ||
        entry->size > MSG_MAX_STREAM_BYTES)
        return NULL;
    remaining = (size_t)entry->size;
    output = (uint8_t *)malloc(remaining);
    if (!output) return NULL;
    if (entry->size < cfb->mini_cutoff && cfb->mini_stream) {
        sector = entry->start;
        while (sector != MSG_SECTOR_END && sector != MSG_SECTOR_FREE &&
               written < remaining) {
            size_t offset = (size_t)sector * cfb->mini_sector_size;
            size_t chunk = remaining - written;
            if (chunk > cfb->mini_sector_size) chunk = cfb->mini_sector_size;
            if (sector >= cfb->mini_fat_count ||
                offset + chunk > cfb->mini_stream_bytes) {
                free(output);
                return NULL;
            }
            memcpy(output + written, cfb->mini_stream + offset, chunk);
            written += chunk;
            sector = cfb->mini_fat[sector];
        }
    } else {
        sector = entry->start;
        while (sector != MSG_SECTOR_END && sector != MSG_SECTOR_FREE &&
               written < remaining) {
            size_t offset;
            size_t chunk = remaining - written;
            if (chunk > cfb->sector_size) chunk = cfb->sector_size;
            if (!msg_sector_available(cfb, sector) ||
                sector >= cfb->fat_count) {
                free(output);
                return NULL;
            }
            offset = msg_sector_offset(cfb, sector);
            memcpy(output + written, cfb->data + offset, chunk);
            written += chunk;
            sector = cfb->fat[sector];
        }
    }
    if (written != remaining) {
        free(output);
        return NULL;
    }
    if (out_size) *out_size = written;
    return output;
}

static int msg_find_stream(const MsgCfb *cfb, const char *name)
{
    size_t i;
    for (i = 0u; i < cfb->directory_count; ++i) {
        if (cfb->directory[i].type == 2u &&
            strcmp(cfb->directory[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static void msg_collect_tree(const MsgCfb *cfb, uint32_t node,
                             uint32_t *out, size_t *count, size_t capacity,
                             unsigned depth)
{
    const MsgDirEntry *entry;
    if (depth > 32u || *count >= capacity ||
        node >= cfb->directory_count)
        return;
    entry = &cfb->directory[node];
    msg_collect_tree(cfb, entry->left, out, count, capacity, depth + 1u);
    if (*count < capacity) out[(*count)++] = node;
    msg_collect_tree(cfb, entry->right, out, count, capacity, depth + 1u);
}

static int msg_utf16_to_utf8(const uint8_t *data, size_t size,
                             char **out_text, size_t *out_size)
{
    QwTextBuffer buffer = {0};
    size_t cursor;
    if (!data || !out_text || !out_size) return 0;
    buffer.limit = MSG_MAX_STREAM_BYTES;
    for (cursor = 0u; cursor + 1u < size; cursor += 2u) {
        uint32_t value = msg_rd16(data + cursor);
        char encoded[4];
        size_t length = 0u;
        if (value == 0u) break;
        if (value >= 0xD800u && value <= 0xDBFFu && cursor + 3u < size) {
            uint32_t low = msg_rd16(data + cursor + 2u);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                value = 0x10000u + ((value - 0xD800u) << 10) +
                        (low - 0xDC00u);
                cursor += 2u;
            }
        }
        if (value < 0x80u) {
            encoded[0] = (char)value;
            length = 1u;
        } else if (value < 0x800u) {
            encoded[0] = (char)(0xC0u | (value >> 6));
            encoded[1] = (char)(0x80u | (value & 0x3Fu));
            length = 2u;
        } else if (value < 0x10000u) {
            encoded[0] = (char)(0xE0u | (value >> 12));
            encoded[1] = (char)(0x80u | ((value >> 6) & 0x3Fu));
            encoded[2] = (char)(0x80u | (value & 0x3Fu));
            length = 3u;
        } else {
            encoded[0] = (char)(0xF0u | (value >> 18));
            encoded[1] = (char)(0x80u | ((value >> 12) & 0x3Fu));
            encoded[2] = (char)(0x80u | ((value >> 6) & 0x3Fu));
            encoded[3] = (char)(0x80u | (value & 0x3Fu));
            length = 4u;
        }
        if (!qw_text_buffer_append(&buffer, encoded, length)) {
            free(buffer.data);
            return 0;
        }
    }
    if (!buffer.data) {
        buffer.data = (char *)malloc(1u);
        if (!buffer.data) return 0;
        buffer.data[0] = '\0';
    }
    *out_text = buffer.data;
    *out_size = buffer.size;
    return 1;
}

static int msg_html_escape(QwTextBuffer *html, const char *text, size_t size)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        const char *entity = NULL;
        switch (text[i]) {
        case '&': entity = "&amp;"; break;
        case '<': entity = "&lt;"; break;
        case '>': entity = "&gt;"; break;
        case '"': entity = "&quot;"; break;
        default: break;
        }
        if (entity) {
            if (!qw_text_buffer_append(html, entity, strlen(entity)))
                return 0;
        } else if (!qw_text_buffer_append(html, text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

static int msg_base64_append(QwTextBuffer *buffer, const uint8_t *data,
                             size_t size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0u; i < size; i += 3u) {
        uint32_t value = (uint32_t)data[i] << 16;
        size_t remain = size - i;
        char encoded[4];
        if (remain > 1u) value |= (uint32_t)data[i + 1u] << 8;
        if (remain > 2u) value |= data[i + 2u];
        encoded[0] = alphabet[(value >> 18) & 63u];
        encoded[1] = alphabet[(value >> 12) & 63u];
        encoded[2] = remain > 1u ? alphabet[(value >> 6) & 63u] : '=';
        encoded[3] = remain > 2u ? alphabet[value & 63u] : '=';
        if (!qw_text_buffer_append(buffer, encoded, 4u)) return 0;
    }
    return 1;
}

static const char *msg_image_mime(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot) return NULL;
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0) return "image/gif";
    if (strcmp(dot, ".webp") == 0) return "image/webp";
    if (strcmp(dot, ".bmp") == 0) return "image/bmp";
    return NULL;
}


static int msg_html_body_payload(const uint8_t *data, size_t size,
                                 const uint8_t **out, size_t *out_size);

static int msg_ascii_ci(const char *text, size_t size, const char *word)
{
    size_t i;
    size_t word_size = strlen(word);
    if (size != word_size) return 0;
    for (i = 0u; i < size; ++i) {
        unsigned char a = (unsigned char)text[i];
        unsigned char b = (unsigned char)word[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* 0 binary, 1 plain text, 2 HTML. */
static int msg_text_attachment_kind(const char *name)
{
    const char *dot;
    size_t length;
    if (!name) return 0;
    dot = strrchr(name, '.');
    if (!dot) return 0;
    length = strlen(dot);
    if (msg_ascii_ci(dot, length, ".txt") ||
        msg_ascii_ci(dot, length, ".log") ||
        msg_ascii_ci(dot, length, ".csv") ||
        msg_ascii_ci(dot, length, ".md"))
        return 1;
    if (msg_ascii_ci(dot, length, ".htm") ||
        msg_ascii_ci(dot, length, ".html"))
        return 2;
    return 0;
}

static int msg_append_attachments(const MsgCfb *cfb, QwTextBuffer *html,
                                  size_t *out_count)
{
    size_t i;
    size_t count = 0u;
    size_t embedded_bytes = 0u;
    if (!qw_text_buffer_append(html,
                               "<section class=\"attachments\"><h2>Attachments</h2><ul>",
                               strlen("<section class=\"attachments\"><h2>Attachments</h2><ul>")))
        return 0;
    for (i = 0u; i < cfb->directory_count &&
                count < MSG_MAX_ATTACHMENTS; ++i) {
        const MsgDirEntry *storage = &cfb->directory[i];
        uint32_t subtree[MSG_MAX_ATTACHMENT_ENTRIES];
        size_t subtree_count = 0u;
        size_t j;
        int name_index = -1;
        int data_index = -1;
        char *name = NULL;
        size_t name_size = 0u;
        uint8_t *payload = NULL;
        size_t payload_size = 0u;
        const char *mime;

        if (storage->type != 1u ||
            strncmp(storage->name, "__attach_version1.0", 18u) != 0)
            continue;
        msg_collect_tree(cfb, storage->child, subtree, &subtree_count,
                         MSG_MAX_ATTACHMENT_ENTRIES, 0u);
        for (j = 0u; j < subtree_count; ++j) {
            const MsgDirEntry *entry = &cfb->directory[subtree[j]];
            if (entry->type != 2u) continue;
            if (strcmp(entry->name, "__substg1.0_3707001F") == 0)
                name_index = (int)subtree[j];
            else if (strcmp(entry->name, "__substg1.0_37010102") == 0)
                data_index = (int)subtree[j];
        }
        if (name_index >= 0) {
            uint8_t *raw = msg_read_stream(cfb, (size_t)name_index, &name_size);
            if (raw) {
                (void)msg_utf16_to_utf8(raw, name_size, &name, &name_size);
                free(raw);
            }
        }
        if (data_index >= 0)
            payload = msg_read_stream(cfb, (size_t)data_index, &payload_size);
        if (!name) {
            free(payload);
            continue;
        }
        ++count;
        if (!qw_text_buffer_append(html, "<li>", 4u) ||
            !msg_html_escape(html, name, strlen(name)) ||
            !qw_text_buffer_append(html, " (", 2u)) {
            free(name);
            free(payload);
            return 0;
        }
        {
            char size_text[32];
            int length = snprintf(size_text, sizeof(size_text), "%lu",
                                  (unsigned long)payload_size);
            if (length < 0 ||
                !qw_text_buffer_append(html, size_text, (size_t)length)) {
                free(name);
                free(payload);
                return 0;
            }
        }
        if (!qw_text_buffer_append(html, " bytes)", strlen(" bytes)"))) {
            free(name);
            free(payload);
            return 0;
        }
        {
            int kind = payload ? msg_text_attachment_kind(name) : 0;
            if (kind == 1 && payload_size > 0u &&
                payload_size <= 256u * 1024u) {
                if (!qw_text_buffer_append(html,
                        "<pre class=\"attachment-body\">",
                        strlen("<pre class=\"attachment-body\">")) ||
                    !msg_html_escape(html, (const char *)payload,
                                     payload_size) ||
                    !qw_text_buffer_append(html, "</pre>", 6u)) {
                    free(name);
                    free(payload);
                    return 0;
                }
            } else if (kind == 2 && payload_size > 0u &&
                       payload_size <= 512u * 1024u) {
                const uint8_t *inner = NULL;
                size_t inner_size = 0u;
                if (msg_html_body_payload(payload, payload_size, &inner,
                                          &inner_size)) {
                    if (!qw_text_buffer_append(html,
                            "<div class=\"attachment-body\">", 29u) ||
                        !qw_text_buffer_append(html,
                            (const char *)inner, inner_size) ||
                        !qw_text_buffer_append(html, "</div>", 6u)) {
                        free(name);
                        free(payload);
                        return 0;
                    }
                }
            }
        }
        if (!qw_text_buffer_append(html, "</li>", 5u)) {
            free(name);
            free(payload);
            return 0;
        }
        mime = msg_image_mime(name);
        if (payload && payload_size > 0u && mime &&
            payload_size <= 8u * 1024u * 1024u &&
            embedded_bytes + payload_size <= MSG_MAX_EMBED_BYTES) {
            embedded_bytes += payload_size;
            if (!qw_text_buffer_append(html, "<li><img alt=\"", strlen("<li><img alt=\"")) ||
                !msg_html_escape(html, name, strlen(name)) ||
                !qw_text_buffer_append(html, "\" src=\"data:", 12u) ||
                !qw_text_buffer_append(html, mime, strlen(mime)) ||
                !qw_text_buffer_append(html, ";base64,", 8u) ||
                !msg_base64_append(html, payload, payload_size) ||
                !qw_text_buffer_append(html, "\"/></li>", 8u)) {
                free(name);
                free(payload);
                return 0;
            }
        }
        free(name);
        free(payload);
    }
    if (!qw_text_buffer_append(html, "</ul></section>", 15u)) return 0;
    if (out_count) *out_count = count;
    return 1;
}


/* MSG HTML bodies (10130102) are stored as raw 8-bit HTML; take the inner
 * body when a full document is provided, and ignore UTF-16 looking payloads. */
static int msg_html_body_payload(const uint8_t *data, size_t size,
                                 const uint8_t **out, size_t *out_size)
{
    size_t end = size;
    size_t i;
    size_t start = 0u;
    if (!data || !out || !out_size) return 0;
    while (end > 0u && data[end - 1u] == 0u) --end;
    if (end == 0u) return 0;
    for (i = 0u; i + 1u < end && i < 512u; i += 2u) {
        if (data[i] != 0u && data[i + 1u] == 0u) return 0;
    }
    for (i = 0u; i < end && i < 64u; ++i) {
        if (data[i] == '<') break;
    }
    if (i >= end || i >= 64u) return 0;
    start = i;
    if (end - start >= 6u &&
        (memcmp(data + start, "<html>", 6u) == 0 ||
         memcmp(data + start, "<HTML>", 6u) == 0)) {
        size_t body = start;
        int found = 0;
        for (; body + 5u < end; ++body) {
            if ((data[body] == '<') &&
                (data[body + 1u] == 'b' || data[body + 1u] == 'B') &&
                (data[body + 2u] == 'o' || data[body + 2u] == 'O') &&
                (data[body + 3u] == 'd' || data[body + 3u] == 'D') &&
                (data[body + 4u] == 'y' || data[body + 4u] == 'Y')) {
                found = 1;
                break;
            }
        }
        if (found) {
            while (body < end && data[body] != '>') ++body;
            if (body < end) ++body;
            start = body;
            {
                size_t close = start;
                for (; close + 7u <= end; ++close) {
                    if (data[close] == '<' && data[close + 1u] == '/' &&
                        (data[close + 2u] == 'b' || data[close + 2u] == 'B') &&
                        (data[close + 3u] == 'o' || data[close + 3u] == 'O') &&
                        (data[close + 4u] == 'd' || data[close + 4u] == 'D') &&
                        (data[close + 5u] == 'y' || data[close + 5u] == 'Y')) {
                        end = close;
                        break;
                    }
                }
            }
        }
    }
    if (end <= start) return 0;
    *out = data + start;
    *out_size = end - start;
    return 1;
}

static int msg_append_text_property(const MsgCfb *cfb, QwTextBuffer *html,
                                    const char *stream_name,
                                    const char *prefix, const char *suffix)
{
    int index = msg_find_stream(cfb, stream_name);
    uint8_t *raw;
    size_t raw_size = 0u;
    char *text = NULL;
    size_t text_size = 0u;
    if (index < 0) return 1;
    raw = msg_read_stream(cfb, (size_t)index, &raw_size);
    if (!raw) return 1;
    (void)msg_utf16_to_utf8(raw, raw_size, &text, &text_size);
    free(raw);
    if (!text) return 1;
    if (text_size > 0u) {
        if (!qw_text_buffer_append(html, prefix, strlen(prefix)) ||
            !msg_html_escape(html, text, text_size) ||
            !qw_text_buffer_append(html, suffix, strlen(suffix))) {
            free(text);
            return 0;
        }
    }
    free(text);
    return 1;
}

int qw_msg_probe(const uint8_t *data, size_t size)
{
    MsgCfb cfb;
    size_t i;
    int found = 0;
    if (!msg_cfb_open(&cfb, data, size)) return 0;
    for (i = 0u; i < cfb.directory_count; ++i) {
        if (strncmp(cfb.directory[i].name, "__substg1.0_", 12u) == 0 ||
            strcmp(cfb.directory[i].name, "__properties_version1.0") == 0) {
            found = 1;
            break;
        }
    }
    msg_cfb_dispose(&cfb);
    return found;
}


static int msg_hex_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    ch = (unsigned char)tolower(ch);
    if (ch >= 'a' && ch <= 'f') return (int)(ch - 'a') + 10;
    return -1;
}

static uint32_t msg_u32le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* MS-OXRTFCP: LZFu compressed RTF used by PidTagRtfCompressed. */
static int msg_lzfu_decompress(const uint8_t *data, size_t size,
                               uint8_t **out_data, size_t *out_size)
{
    uint8_t dictionary[4096];
    uint8_t *output = NULL;
    size_t declared;
    size_t written = 0u;
    size_t position;
    if (!data || !out_data || !out_size || size == 0u) return 0;
    *out_data = NULL;
    *out_size = 0u;
    if (size >= 5u && memcmp(data, "{\\rtf", 5u) == 0) {
        uint8_t *copy = (uint8_t *)malloc(size);
        if (!copy) return 0;
        memcpy(copy, data, size);
        *out_data = copy;
        *out_size = size;
        return 1;
    }
    if (size < 12u || data[4] != 'L' || data[5] != 'Z' ||
        data[6] != 'F' || data[7] != 'u')
        return 0;
    declared = (size_t)msg_u32le(data);
    if (declared == 0u || declared > 64u * 1024u * 1024u) return 0;
    output = (uint8_t *)malloc(declared);
    if (!output) return 0;
    memset(dictionary, ' ', sizeof(dictionary));
    position = 12u;
    while (position < size && written < declared) {
        uint8_t control = data[position++];
        int bit;
        for (bit = 0; bit < 8 && written < declared; ++bit) {
            if (control & (1u << bit)) {
                size_t token;
                size_t offset;
                size_t length;
                size_t index;
                if (position + 1u >= size) {
                    free(output);
                    return 0;
                }
                token = (size_t)data[position] |
                        ((size_t)data[position + 1u] << 8);
                position += 2u;
                offset = (token >> 4) & 0xfffu;
                length = (token & 0x0fu) + 2u;
                for (index = 0u; index < length && written < declared;
                     ++index) {
                    uint8_t value = dictionary[(offset + index) & 0xfffu];
                    dictionary[written & 0xfffu] = value;
                    output[written++] = value;
                }
            } else {
                if (position >= size) {
                    free(output);
                    return 0;
                }
                dictionary[written & 0xfffu] = data[position];
                output[written++] = data[position++];
            }
        }
    }
    *out_data = output;
    *out_size = written;
    return written > 0u;
}

typedef struct MsgRtfSkip {
    int depth;
    int skipping;
    int pending;
} MsgRtfSkip;

static int msg_rtf_append_utf8(QwTextBuffer *out, unsigned int code)
{
    char encoded[4];
    size_t size;
    if (code == 0u || code > 0x10ffffu ||
        (code >= 0xd800u && code <= 0xdfffu))
        code = 0xfffdu;
    if (code < 0x80u) {
        encoded[0] = (char)code;
        size = 1u;
    } else if (code < 0x800u) {
        encoded[0] = (char)(0xc0u | (code >> 6));
        encoded[1] = (char)(0x80u | (code & 0x3fu));
        size = 2u;
    } else if (code < 0x10000u) {
        encoded[0] = (char)(0xe0u | (code >> 12));
        encoded[1] = (char)(0x80u | ((code >> 6) & 0x3fu));
        encoded[2] = (char)(0x80u | (code & 0x3fu));
        size = 3u;
    } else {
        encoded[0] = (char)(0xf0u | (code >> 18));
        encoded[1] = (char)(0x80u | ((code >> 12) & 0x3fu));
        encoded[2] = (char)(0x80u | ((code >> 6) & 0x3fu));
        encoded[3] = (char)(0x80u | (code & 0x3fu));
        size = 4u;
    }
    return qw_text_buffer_append(out, encoded, size);
}

static int msg_rtf_is_skip_destination(const char *word, size_t size)
{
    static const char *const names[] = {
        "fonttbl", "colortbl", "stylesheet", "info", "pict", "header",
        "footer", "headerl", "headerr", "footerl", "footerr", "object",
        "themedata", "colorschememapping", "datastore", "latentstyles"
    };
    size_t index;
    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (strlen(names[index]) == size &&
            memcmp(names[index], word, size) == 0)
            return 1;
    }
    return 0;
}

typedef struct MsgRtfImage {
    uint8_t *data;
    size_t size;
    char mime[16];
} MsgRtfImage;

typedef struct MsgRtfImages {
    MsgRtfImage *items;
    size_t count;
    size_t capacity;
} MsgRtfImages;

static void msg_rtf_images_dispose(MsgRtfImages *images)
{
    size_t index;
    if (!images) return;
    for (index = 0u; index < images->count; ++index)
        free(images->items[index].data);
    free(images->items);
    memset(images, 0, sizeof(*images));
}

static int msg_rtf_add_image(MsgRtfImages *images, const uint8_t *data,
                             size_t size, const char *mime)
{
    MsgRtfImage *grown;
    uint8_t *copy;
    if (!images || !data || !size || images->count >= 64u) return 0;
    if (images->count >= images->capacity) {
        size_t next = images->capacity ? images->capacity * 2u : 4u;
        grown = (MsgRtfImage *)realloc(images->items,
                                       next * sizeof(*images->items));
        if (!grown) return 0;
        images->items = grown;
        images->capacity = next;
    }
    copy = (uint8_t *)malloc(size);
    if (!copy) return 0;
    memcpy(copy, data, size);
    images->items[images->count].data = copy;
    images->items[images->count].size = size;
    snprintf(images->items[images->count].mime,
             sizeof(images->items[images->count].mime), "%s", mime);
    ++images->count;
    return 1;
}

/* Parse a \pict group starting after the control word; returns the index
 * after the closing brace. */
static int msg_rtf_hex_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static const uint8_t *msg_rtf_find_hex_signature(const uint8_t *data,
                                                 size_t start, size_t end,
                                                 const char *signature)
{
    size_t size = strlen(signature);
    size_t index;
    if (size == 0u || end < start || end - start < size) return NULL;
    for (index = start; index + size <= end; ++index) {
        size_t offset;
        int matches = 1;
        for (offset = 0u; offset < size; ++offset) {
            int expected = msg_rtf_hex_digit((unsigned char)signature[offset]);
            int actual = msg_rtf_hex_digit(data[index + offset]);
            if (expected < 0 || actual != expected) {
                matches = 0;
                break;
            }
        }
        if (matches) return data + index;
    }
    return NULL;
}

/* Parse a \pict group starting after the control word; returns the index
 * after the closing brace. */
static size_t msg_rtf_parse_picture(const uint8_t *data, size_t size,
                                    size_t index, MsgRtfImages *images)
{
    size_t cursor = index;
    size_t close = size;
    int depth = 1;
    const char *mime = NULL;
    size_t hex_start = 0u;
    size_t hex_end = 0u;
    while (cursor < size) {
        if (data[cursor] == '{') ++depth;
        else if (data[cursor] == '}') {
            if (--depth == 0) { close = cursor; break; }
        }
        ++cursor;
    }
    if (close >= size) return cursor;
    {
        static const char png_tag[] = "pngblip";
        static const char jpeg_tag[] = "jpegblip";
        size_t region = index;
        size_t tag = SIZE_MAX;
        while (region + 7u <= close) {
            if (memcmp(data + region, png_tag, 7u) == 0) {
                mime = "image/png";
                tag = region + 7u;
                break;
            }
            if (region + 8u <= close &&
                memcmp(data + region, jpeg_tag, 8u) == 0) {
                mime = "image/jpeg";
                tag = region + 8u;
                break;
            }
            ++region;
        }
        if (mime) {
            const uint8_t *signature =
                msg_rtf_find_hex_signature(data, tag, close,
                                           strcmp(mime, "image/png") == 0
                                               ? "89504e47" : "ffd8");
            hex_start = signature ? (size_t)(signature - data) : tag;
            hex_end = close;
        }
    }
    if (mime && hex_end > hex_start) {
        uint8_t *decoded = (uint8_t *)malloc((hex_end - hex_start) / 2u + 2u);
        size_t written = 0u;
        int high = -1;
        size_t i;
        if (decoded) {
            for (i = hex_start; i < hex_end; ++i) {
                int digit = msg_rtf_hex_digit(data[i]);
                if (digit < 0) continue;
                if (high < 0) high = digit;
                else {
                    decoded[written++] = (uint8_t)((high << 4) | digit);
                    high = -1;
                }
            }
            if (written && !msg_rtf_add_image(images, decoded, written, mime)) {
                free(decoded);
                return close + 1u;
            }
        }
        free(decoded);
    }
    return close + 1u;
}

/* Conservative RTF-to-text extraction for message bodies: paragraphs, tabs,
 * escaped bytes and \uN escapes; destination groups are skipped. */
static int msg_rtf_to_text(const uint8_t *data, size_t size,
                           MsgRtfImages *images, char **out_text,
                           size_t *out_size)
{
    QwTextBuffer out;
    size_t index = 0u;
    int skip_depth = -1;
    int depth = 0;
    if (!data || !out_text || !out_size) return 0;
    *out_text = NULL;
    *out_size = 0u;
    memset(&out, 0, sizeof(out));
    out.limit = 32u * 1024u * 1024u;
    while (index < size) {
        unsigned char chr = data[index];
        if (chr == '{') {
            ++depth;
            ++index;
            if (skip_depth < 0 && index + 1u < size &&
                data[index] == '\\' &&
                (data[index + 1u] == '*' || data[index + 1u] == '~')) {
                skip_depth = depth;
            }
            continue;
        }
        if (chr == '}') {
            if (skip_depth == depth) skip_depth = -1;
            if (depth > 0) --depth;
            ++index;
            continue;
        }
        if (chr == '\\') {
            size_t start;
            ++index;
            if (index >= size) break;
            chr = data[index];
            if (chr == '\\' || chr == '{' || chr == '}') {
                if (skip_depth < 0 && !qw_text_buffer_append(&out, &chr, 1u))
                    goto out_of_memory;
                ++index;
                continue;
            }
            if (chr == '\'') {
                int high;
                int low;
                if (index + 2u >= size) break;
                high = msg_hex_digit(data[index + 1u]);
                low = msg_hex_digit(data[index + 2u]);
                if (high >= 0 && low >= 0 && skip_depth < 0) {
                    unsigned char byte = (unsigned char)((high << 4) | low);
                    if (!qw_text_buffer_append(&out, (const char *)&byte, 1u))
                        goto out_of_memory;
                }
                index += 3u;
                continue;
            }
            if (chr == '*') {
                if (skip_depth < 0) skip_depth = depth;
                ++index;
                continue;
            }
            if (chr == '~') {
                if (skip_depth < 0 && !qw_text_buffer_append(&out, " ", 1u))
                    goto out_of_memory;
                ++index;
                continue;
            }
            if (chr < 'a' || chr > 'z') {
                ++index;
                continue;
            }
            start = index;
            while (index < size &&
                   ((data[index] >= 'a' && data[index] <= 'z') ||
                    (data[index] >= 'A' && data[index] <= 'Z')))
                ++index;
            {
                const char *word = (const char *)data + start;
                size_t word_size = index - start;
                int negative = 0;
                long parameter = 0;
                int has_parameter = 0;
                if (index < size && data[index] == '-') {
                    negative = 1;
                    ++index;
                }
                while (index < size && data[index] >= '0' &&
                       data[index] <= '9') {
                    parameter = parameter * 10 + (data[index] - '0');
                    has_parameter = 1;
                    ++index;
                }
                if (index < size && data[index] == ' ') ++index;
                if (word_size == 4u && memcmp(word, "pict", 4u) == 0) {
                    if (skip_depth < 0 && images) {
                        size_t after = msg_rtf_parse_picture(data, size, index,
                                                             images);
                        if (!qw_text_buffer_append(&out, "\x02", 1u))
                            goto out_of_memory;
                        index = after;
                        if (depth > 0) --depth;
                        continue;
                    }
                    if (skip_depth < 0) skip_depth = depth;
                    continue;
                }
                if (msg_rtf_is_skip_destination(word, word_size)) {
                    if (skip_depth < 0) skip_depth = depth;
                    continue;
                }
                if (skip_depth >= 0) continue;
                if (word_size == 5u && memcmp(word, "trowd", 5u) == 0) {
                    if (!qw_text_buffer_append(&out, "\x1d", 1u))
                        goto out_of_memory;
                } else if (word_size == 4u &&
                           memcmp(word, "cell", 4u) == 0) {
                    if (!qw_text_buffer_append(&out, "\x1f", 1u))
                        goto out_of_memory;
                } else if (word_size == 3u &&
                           memcmp(word, "row", 3u) == 0) {
                    if (!qw_text_buffer_append(&out, "\x1e", 1u))
                        goto out_of_memory;
                } else if (word_size == 3u && memcmp(word, "par", 3u) == 0) {
                    if (!qw_text_buffer_append(&out, "\n", 1u))
                        goto out_of_memory;
                } else if (word_size == 4u && memcmp(word, "line", 4u) == 0) {
                    if (!qw_text_buffer_append(&out, "\n", 1u))
                        goto out_of_memory;
                } else if (word_size == 3u && memcmp(word, "tab", 3u) == 0) {
                    if (!qw_text_buffer_append(&out, "\t", 1u))
                        goto out_of_memory;
                } else if (word_size == 1u && word[0] == 'u' &&
                           has_parameter) {
                    int code = negative ? -(int)parameter : (int)parameter;
                    if (code < 0) code += 65536;
                    if (!msg_rtf_append_utf8(&out, (unsigned int)code))
                        goto out_of_memory;
                    if (index < size && data[index] == '?') ++index;
                    if (index < size && data[index] == ' ') ++index;
                }
            }
            continue;
        }
        if (chr == '\r' || chr == '\n') {
            ++index;
            continue;
        }
        if (skip_depth < 0 && !qw_text_buffer_append(&out, (const char *)&chr, 1u))
            goto out_of_memory;
        ++index;
    }
    *out_text = out.data ? (char *)out.data : (char *)calloc(1u, 1u);
    if (!*out_text) return 0;
    *out_size = out.data ? out.size : 0u;
    return 1;
out_of_memory:
    free(out.data);
    return 0;
}



static int msg_html_escape_range(QwTextBuffer *out, const char *text,
                                 size_t size)
{
    return msg_html_escape(out, (const uint8_t *)text, size);
}

/* Render RTF text carrying 0x1d/0x1f/0x1e row/cell/row-end markers. */
static int msg_rtf_render_html(QwTextBuffer *out, const char *text, size_t size,
                               MsgRtfImages *images)
{
    size_t index = 0u;
    int table_open = 0;
    int row_open = 0;
    int cell_open = 0;
    int paragraph_open = 0;
    size_t image_index = 0u;
    while (index < size) {
        char chr = text[index];
        if (chr == '\x02') {
            if (images && image_index < images->count) {
                if (row_open && !cell_open) {
                    if (!qw_text_buffer_append(out, "<td>", 4u)) return 0;
                    cell_open = 1;
                } else if (!row_open && !paragraph_open) {
                    if (table_open) {
                        if (!qw_text_buffer_append(out, "</table><p>", 11u))
                            return 0;
                        table_open = 0;
                    } else if (!qw_text_buffer_append(out, "<p>", 3u)) {
                        return 0;
                    }
                    paragraph_open = 1;
                }
                if (!qw_text_buffer_append(out, "<img alt=\"image\" src=\"data:",
                                           sizeof("<img alt=\"image\" src=\"data:") - 1u) ||
                    !qw_text_buffer_append(out, images->items[image_index].mime,
                                           strlen(images->items[image_index].mime)) ||
                    !qw_text_buffer_append(out, ";base64,", 8u) ||
                    !msg_base64_append(out, images->items[image_index].data,
                                       images->items[image_index].size) ||
                    !qw_text_buffer_append(out, "\"/>", 2u))
                    return 0;
                ++image_index;
            }
            ++index;
            continue;
        }
        if (chr == '\x1d') {
            if (cell_open && !qw_text_buffer_append(out, "</td>", 5u)) return 0;
            cell_open = 0;
            if (row_open && !qw_text_buffer_append(out, "</tr>", 5u)) return 0;
            row_open = 0;
            if (paragraph_open) {
                if (!qw_text_buffer_append(out, "</p>", 4u)) return 0;
                paragraph_open = 0;
            }
            if (!table_open) {
                if (!qw_text_buffer_append(out, "<table>", 7u)) return 0;
                table_open = 1;
            }
            if (!qw_text_buffer_append(out, "<tr>", 4u)) return 0;
            row_open = 1;
            ++index;
            continue;
        }
        if (chr == '\x1f') {
            if (cell_open) {
                if (!qw_text_buffer_append(out, "</td>", 5u)) return 0;
                cell_open = 0;
            } else if (row_open) {
                if (!qw_text_buffer_append(out, "<td></td>", 9u)) return 0;
            }
            ++index;
            continue;
        }
        if (chr == '\x1e') {
            if (cell_open) {
                if (!qw_text_buffer_append(out, "</td>", 5u)) return 0;
                cell_open = 0;
            }
            if (row_open) {
                if (!qw_text_buffer_append(out, "</tr>", 5u)) return 0;
                row_open = 0;
            }
            ++index;
            continue;
        }
        if (row_open) {
            if (!cell_open) {
                if (!qw_text_buffer_append(out, "<td>", 4u)) return 0;
                cell_open = 1;
            }
            if (chr == '\r') {
                ++index;
                continue;
            }
            if (chr == '\n') {
                if (!qw_text_buffer_append(out, "<br/>", 5u)) return 0;
                ++index;
                continue;
            }
            {
                size_t start = index;
                while (index < size && text[index] != '\r' &&
                       text[index] != '\n' && text[index] != '\x1d' &&
                       text[index] != '\x1f' && text[index] != '\x1e')
                    ++index;
                if (!msg_html_escape_range(out, text + start, index - start))
                    return 0;
            }
            continue;
        }
        if (!table_open && !paragraph_open) {
            if (!qw_text_buffer_append(out, "<p>", 3u)) return 0;
            paragraph_open = 1;
        }
        if (chr == '\r') {
            ++index;
            continue;
        }
        if (chr == '\n') {
            if (!qw_text_buffer_append(out, "<br/>", 5u)) return 0;
            ++index;
            continue;
        }
        {
            size_t start = index;
            while (index < size && text[index] != '\r' && text[index] != '\n' &&
                   text[index] != '\x1d' && text[index] != '\x1f' &&
                   text[index] != '\x1e')
                ++index;
            if (paragraph_open) {
                if (!msg_html_escape_range(out, text + start, index - start))
                    return 0;
            } else if (table_open) {
                /* Text after a table: close it and start a paragraph. */
                if (!qw_text_buffer_append(out, "</table><p>", 11u)) return 0;
                table_open = 0;
                paragraph_open = 1;
                if (!msg_html_escape_range(out, text + start, index - start))
                    return 0;
            }
        }
    }
    if (cell_open && !qw_text_buffer_append(out, "</td>", 5u)) return 0;
    if (row_open && !qw_text_buffer_append(out, "</tr>", 5u)) return 0;
    if (table_open && !qw_text_buffer_append(out, "</table>", 8u)) return 0;
    if (paragraph_open && !qw_text_buffer_append(out, "</p>", 4u)) return 0;
    return 1;
}

int qw_msg_to_html(const uint8_t *data, size_t size, uint8_t **out_html,
                   size_t *out_size, QwConvertReport *report)
{
    MsgCfb cfb;
    QwTextBuffer html = {0};
    int body_index;
    int used_html_body = 0;
    int used_rtf_body = 0;
    uint8_t *body_raw = NULL;
    size_t body_size = 0u;
    char *body = NULL;
    size_t body_text_size = 0u;
    size_t attachments = 0u;
    int rc = QW_STATUS_OK;

    if (!out_html || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_html = NULL;
    *out_size = 0u;
    if (!data || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (!msg_cfb_open(&cfb, data, size)) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "MSG compound file could not be parsed");
        return QW_STATUS_ERR_FORMAT;
    }
    if (cfb.directory_count == 0u || msg_find_stream(&cfb,
        "__substg1.0_0037001F") < 0 &&
        msg_find_stream(&cfb, "__substg1.0_1000001F") < 0) {
        msg_cfb_dispose(&cfb);
        if (report) snprintf(report->note, sizeof(report->note),
                             "compound file has no MSG property streams");
        return QW_STATUS_ERR_FORMAT;
    }
    html.limit = 128u * 1024u * 1024u;
    if (!qw_text_buffer_append(&html,
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>Outlook message</title><style>body{font-family:sans-serif;"
            "max-width:46rem;margin:2rem auto;line-height:1.4}"
            ".attachments{color:#333;font-size:0.95em}"
            ".attachments img{max-width:100%}</style></head><body>",
            strlen("<!doctype html><html><head><meta charset=\"utf-8\">"
                   "<title>Outlook message</title><style>body{font-family:sans-serif;"
                   "max-width:46rem;margin:2rem auto;line-height:1.4}"
                   ".attachments{color:#333;font-size:0.95em}"
                   ".attachments img{max-width:100%}</style></head><body>"))) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto finish;
    }
    if (!msg_append_text_property(&cfb, &html, "__substg1.0_0037001F",
                                  "<header><h1>", "</h1>") ||
        !msg_append_text_property(&cfb, &html, "__substg1.0_0C1A001F",
                                  "<p>From: ", "</p>") ||
        !msg_append_text_property(&cfb, &html, "__substg1.0_0C1F001F",
                                  "<p>Email: ", "</p>") ||
        !qw_text_buffer_append(&html, "</header><div class=\"body\">", strlen("</header><div class=\"body\">"))) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto finish;
    }
    {
        int html_body_index = msg_find_stream(&cfb,
                                              "__substg1.0_10130102");
        uint8_t *html_body_raw = NULL;
        size_t html_body_size = 0u;
        const uint8_t *html_payload = NULL;
        size_t html_payload_size = 0u;
        int used_html = 0;
        if (html_body_index >= 0)
            html_body_raw = msg_read_stream(
                &cfb, (size_t)html_body_index, &html_body_size);
        if (html_body_raw &&
            msg_html_body_payload(html_body_raw, html_body_size,
                                  &html_payload, &html_payload_size)) {
            if (!qw_text_buffer_append(&html, (const char *)html_payload,
                                       html_payload_size)) {
                free(html_body_raw);
                rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                goto finish;
            }
            used_html = 1;
        }
        free(html_body_raw);
        if (used_html) {
            used_html_body = 1;
            free(body);
            body = NULL;
        }
    }
    body_index = msg_find_stream(&cfb, "__substg1.0_1000001F");
    if (!used_html_body && body_index >= 0)
        body_raw = msg_read_stream(&cfb, (size_t)body_index, &body_size);
    if (!used_html_body && body_raw)
        (void)msg_utf16_to_utf8(body_raw, body_size, &body, &body_text_size);
    free(body_raw);
    body_raw = NULL;
    if (!used_html_body && body && body_text_size > 0u) {
        size_t cursor = 0u;
        while (cursor < body_text_size) {
            size_t start = cursor;
            size_t end;
            while (cursor < body_text_size && body[cursor] != '\n')
                ++cursor;
            end = cursor;
            if (cursor < body_text_size) ++cursor;
            if (end > start && body[end - 1u] == '\r') --end;
            if (end == start) {
                if (!qw_text_buffer_append(&html, "<p></p>", 8u)) {
                    rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                    goto finish;
                }
                continue;
            }
            if (!qw_text_buffer_append(&html, "<p>", 3u) ||
                !msg_html_escape(&html, body + start, end - start) ||
                !qw_text_buffer_append(&html, "</p>", 4u)) {
                rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                goto finish;
            }
        }
    }
    if (!used_html_body && !body) {
        int rtf_index = msg_find_stream(&cfb, "__substg1.0_10090102");
        if (rtf_index >= 0) {
            size_t rtf_raw_size = 0u;
            uint8_t *rtf_raw = msg_read_stream(&cfb, (size_t)rtf_index,
                                               &rtf_raw_size);
            uint8_t *rtf_plain = NULL;
            size_t rtf_plain_size = 0u;
            if (rtf_raw &&
                msg_lzfu_decompress(rtf_raw, rtf_raw_size, &rtf_plain,
                                    &rtf_plain_size)) {
                char *rtf_text = NULL;
                size_t rtf_text_size = 0u;
                MsgRtfImages rtf_images = {0};
                if (msg_rtf_to_text(rtf_plain, rtf_plain_size, &rtf_images,
                                    &rtf_text, &rtf_text_size) &&
                    rtf_text) {
                    if (!qw_text_buffer_append(&html,
                            "<div class=\"rtf-body\">", 22u) ||
                        !msg_rtf_render_html(&html, rtf_text,
                                             rtf_text_size, &rtf_images) ||
                        !qw_text_buffer_append(&html, "</div>", 6u)) {
                        free(rtf_text);
                        msg_rtf_images_dispose(&rtf_images);
                        free(rtf_plain);
                        free(rtf_raw);
                        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                        goto finish;
                    }
                    used_rtf_body = 1;
                    free(rtf_text);
                    msg_rtf_images_dispose(&rtf_images);
                }
                free(rtf_plain);
            }
            free(rtf_raw);
        }
    }
    if (!used_html_body && !used_rtf_body && !body &&
        !qw_text_buffer_append(&html,
                   "<p class=\"empty\">[no text body]</p>",
                   strlen("<p class=\"empty\">[no text body]</p>"))) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto finish;
    }
    free(body);
    body = NULL;
    if (!qw_text_buffer_append(&html, "</div>", 6u) ||
        !msg_append_attachments(&cfb, &html, &attachments) ||
        !qw_text_buffer_append(&html, "</body></html>", 14u)) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto finish;
    }
    *out_html = (uint8_t *)html.data;
    *out_size = html.size;
    html.data = NULL;
    if (report) {
        report->format = QW_FORMAT_EML;
        snprintf(report->note, sizeof(report->note),
                 "MSG layout: text body rendered, %zu attachment(s) listed",
                 attachments);
    }

finish:
    free(body);
    free(html.data);
    msg_cfb_dispose(&cfb);
    return rc;
}
