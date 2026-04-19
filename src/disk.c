#define _POSIX_C_SOURCE 200809L

#include "disk.h"
#include "device.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void append_mbr_partition(disk_metadata_t *metadata,
                                 size_t slot,
                                 uint8_t type,
                                 uint32_t start_lba,
                                 uint32_t sector_count) {
    partition_info_t *part;

    if (metadata->partition_count >= MAX_PARTITIONS || sector_count == 0) {
        return;
    }

    part = &metadata->partitions[metadata->partition_count++];
    part->index = slot + 1U;
    part->first_lba = start_lba;
    part->last_lba = (uint64_t)start_lba + sector_count - 1U;
    snprintf(part->description,
             sizeof(part->description),
             "MBR type 0x%02x, sectors=%" PRIu32,
             type,
             sector_count);
}

static void parse_mbr(disk_metadata_t *metadata, const uint8_t *sector0) {
    size_t i;
    bool protective = false;

    metadata->layout = DISK_LAYOUT_MBR;
    metadata->partition_count = 0;

    for (i = 0; i < 4; ++i) {
        const uint8_t *entry = sector0 + 446 + (i * 16);
        uint8_t type = entry[4];
        uint32_t start_lba = le32(entry + 8);
        uint32_t sector_count = le32(entry + 12);

        if (type == 0xEE) {
            protective = true;
        }

        if (type != 0) {
            append_mbr_partition(metadata, i, type, start_lba, sector_count);
        }
    }

    if (protective) {
        metadata->layout = DISK_LAYOUT_GPT_PROTECTIVE;
        snprintf(metadata->summary,
                 sizeof(metadata->summary),
                 "Protective MBR found, GPT header expected at LBA 1");
    } else if (metadata->partition_count == 0) {
        snprintf(metadata->summary, sizeof(metadata->summary), "Valid MBR signature, no active partitions");
    } else {
        snprintf(metadata->summary,
                 sizeof(metadata->summary),
                 "MBR with %zu partition(s)",
                 metadata->partition_count);
    }
}

static void guid_to_string(const uint8_t *guid, char *out, size_t out_size) {
    snprintf(out,
             out_size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             guid[3], guid[2], guid[1], guid[0],
             guid[5], guid[4],
             guid[7], guid[6],
             guid[8], guid[9],
             guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}

static bool guid_is_zero(const uint8_t *guid) {
    size_t i;

    for (i = 0; i < 16; ++i) {
        if (guid[i] != 0) {
            return false;
        }
    }

    return true;
}

static void utf16le_name_to_ascii(const uint8_t *name, size_t bytes, char *out, size_t out_size) {
    size_t in_idx = 0;
    size_t out_idx = 0;

    while (in_idx + 1 < bytes && out_idx + 1 < out_size) {
        uint16_t ch = le16(name + in_idx);

        if (ch == 0) {
            break;
        }

        out[out_idx++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        in_idx += 2;
    }

    out[out_idx] = '\0';
}

static int parse_gpt_entries(const device_handle_t *device,
                             disk_metadata_t *metadata,
                             uint64_t entries_lba,
                             uint32_t entry_count,
                             uint32_t entry_size) {
    uint8_t *table_buf;
    uint64_t total_bytes;
    uint64_t start_byte;
    ssize_t read_bytes;
    uint32_t i;

    if (entry_size < 128 || entry_count == 0) {
        return -1;
    }

    total_bytes = (uint64_t)entry_count * entry_size;
    if (total_bytes > (uint64_t)device->logical_sector_size * 64U) {
        total_bytes = (uint64_t)device->logical_sector_size * 64U;
    }

    table_buf = malloc((size_t)total_bytes);
    if (table_buf == NULL) {
        return -1;
    }

    start_byte = entries_lba * device->logical_sector_size;
    read_bytes = pread(device->fd, table_buf, (size_t)total_bytes, (off_t)start_byte);
    if (read_bytes < 0 || (uint64_t)read_bytes < total_bytes) {
        free(table_buf);
        return -1;
    }

    metadata->partition_count = 0;
    for (i = 0; i < entry_count && metadata->partition_count < MAX_PARTITIONS; ++i) {
        const uint8_t *entry = table_buf + ((size_t)i * entry_size);
        partition_info_t *part;
        char type_guid[40];
        char name[64];

        if (guid_is_zero(entry)) {
            continue;
        }

        part = &metadata->partitions[metadata->partition_count++];
        part->index = i + 1U;
        part->first_lba = le64(entry + 32);
        part->last_lba = le64(entry + 40);
        guid_to_string(entry, type_guid, sizeof(type_guid));
        utf16le_name_to_ascii(entry + 56, entry_size - 56, name, sizeof(name));
        snprintf(part->description,
                 sizeof(part->description),
                 "GPT %.36s %.52s",
                 type_guid,
                 name[0] == '\0' ? "<unnamed>" : name);
    }

    free(table_buf);
    return 0;
}

static int parse_gpt(const device_handle_t *device, disk_metadata_t *metadata) {
    uint8_t *sector;
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    int rc = -1;

    sector = malloc(device->logical_sector_size);
    if (sector == NULL) {
        return -1;
    }

    if (device_read_sector(device, 1, sector) != 0) {
        goto cleanup;
    }

    if (memcmp(sector, "EFI PART", 8) != 0) {
        goto cleanup;
    }

    entries_lba = le64(sector + 72);
    entry_count = le32(sector + 80);
    entry_size = le32(sector + 84);

    metadata->layout = DISK_LAYOUT_GPT;
    snprintf(metadata->summary,
             sizeof(metadata->summary),
             "GPT header: entries=%" PRIu32 ", entry_size=%" PRIu32,
             entry_count,
             entry_size);

    if (parse_gpt_entries(device, metadata, entries_lba, entry_count, entry_size) == 0) {
        rc = 0;
    }

cleanup:
    free(sector);
    return rc;
}

int disk_probe_metadata(const device_handle_t *device, disk_metadata_t *metadata) {
    uint8_t *sector0;

    if (device == NULL || metadata == NULL) {
        return -1;
    }

    memset(metadata, 0, sizeof(*metadata));
    snprintf(metadata->summary, sizeof(metadata->summary), "Partition structures were not detected");

    sector0 = malloc(device->logical_sector_size);
    if (sector0 == NULL) {
        return -1;
    }

    if (device_read_sector(device, 0, sector0) != 0) {
        free(sector0);
        return -1;
    }

    if (sector0[510] == 0x55 && sector0[511] == 0xAA) {
        parse_mbr(metadata, sector0);
        if (metadata->layout == DISK_LAYOUT_GPT_PROTECTIVE) {
            (void)parse_gpt(device, metadata);
        }
    }

    free(sector0);
    return 0;
}
