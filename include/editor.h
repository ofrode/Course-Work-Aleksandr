#ifndef EDITOR_H
#define EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDITOR_STATUS_CAPACITY 256
#define MAX_PARTITIONS 128

typedef enum {
    DISK_LAYOUT_UNKNOWN = 0,
    DISK_LAYOUT_MBR,
    DISK_LAYOUT_GPT_PROTECTIVE,
    DISK_LAYOUT_GPT
} disk_layout_t;

typedef struct {
    uint64_t index;
    uint64_t first_lba;
    uint64_t last_lba;
    char description[96];
} partition_info_t;

typedef struct {
    disk_layout_t layout;
    size_t partition_count;
    partition_info_t partitions[MAX_PARTITIONS];
    char summary[160];
} disk_metadata_t;

typedef struct {
    int fd;
    char path[256];
    uint32_t logical_sector_size;
    uint64_t total_bytes;
    uint64_t total_sectors;
    unsigned int retry_count;
} device_handle_t;

typedef struct {
    device_handle_t device;
    disk_metadata_t metadata;
    uint8_t *sector_buf;
    uint8_t *original_buf;
    uint64_t current_lba;
    size_t cursor;
    bool dirty;
    bool edit_high_nibble;
    char status[EDITOR_STATUS_CAPACITY];
    bool quit_requested;
} editor_state_t;

#endif
