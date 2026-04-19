#ifndef DISK_H
#define DISK_H

#include "editor.h"

int disk_probe_metadata(const device_handle_t *device, disk_metadata_t *metadata);

#endif
