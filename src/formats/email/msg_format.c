/* Outlook MSG reader: minimal CFB/MAPI property and attachment extraction.
 *
 * The compound file parser intentionally implements only the read paths MSG
 * files use: FAT, mini FAT, directory streams and named property streams.
 */

#include "msg_format.h"

#include "text_buffer.h"

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
        if (!qw_text_buffer_append(html, " bytes)</li>", strlen(" bytes)</li>"))) {
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

int qw_msg_to_html(const uint8_t *data, size_t size, uint8_t **out_html,
                   size_t *out_size, QwConvertReport *report)
{
    MsgCfb cfb;
    QwTextBuffer html = {0};
    int body_index;
    int used_html_body = 0;
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
    if (!used_html_body && !body &&
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
