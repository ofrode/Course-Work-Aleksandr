#ifndef DEVICE_H
#define DEVICE_H

#include "editor.h"

int device_open(device_handle_t *device, const char *path, unsigned int retries);
void device_close(device_handle_t *device);
int device_read_sector(const device_handle_t *device, uint64_t lba, uint8_t *buffer);
int device_write_sector(const device_handle_t *device, uint64_t lba, const uint8_t *buffer);
int device_reread_sector(const device_handle_t *device, uint64_t lba, uint8_t *buffer);

#endif
