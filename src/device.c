#define _POSIX_C_SOURCE 200809L

#include "device.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int detect_sector_size(int fd, uint32_t *sector_size) {
    int size = 0;

    if (ioctl(fd, BLKSSZGET, &size) == 0 && size > 0) {
        *sector_size = (uint32_t)size;
        return 0;
    }

    *sector_size = 512U;
    return 0;
}

static int detect_total_bytes(int fd, uint64_t *total_bytes) {
    unsigned long long size = 0ULL;
    struct stat st;

    if (ioctl(fd, BLKGETSIZE64, &size) == 0 && size > 0ULL) {
        *total_bytes = (uint64_t)size;
        return 0;
    }

    if (fstat(fd, &st) != 0) {
        return -1;
    }

    if (S_ISREG(st.st_mode)) {
        *total_bytes = (uint64_t)st.st_size;
        return 0;
    }

    errno = ENOTSUP;
    return -1;
}

int device_open(device_handle_t *device, const char *path, unsigned int retries) {
    if (device == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(device, 0, sizeof(*device));
    device->fd = -1;
    device->retry_count = retries == 0 ? 3U : retries;

    device->fd = open(path, O_RDWR | O_CLOEXEC);
    if (device->fd < 0) {
        return -1;
    }

    if (detect_sector_size(device->fd, &device->logical_sector_size) != 0 ||
        detect_total_bytes(device->fd, &device->total_bytes) != 0) {
        int saved_errno = errno;
        close(device->fd);
        memset(device, 0, sizeof(*device));
        device->fd = -1;
        errno = saved_errno;
        return -1;
    }

    device->total_sectors = device->total_bytes / device->logical_sector_size;
    snprintf(device->path, sizeof(device->path), "%s", path);
    return 0;
}

void device_close(device_handle_t *device) {
    if (device == NULL) {
        return;
    }

    if (device->fd >= 0) {
        close(device->fd);
    }

    memset(device, 0, sizeof(*device));
    device->fd = -1;
}

static int checked_io(bool is_write,
                      const device_handle_t *device,
                      uint64_t lba,
                      uint8_t *buffer,
                      const uint8_t *write_buffer) {
    off_t offset;
    ssize_t result;
    unsigned int attempt;

    if (device == NULL || device->fd < 0 || buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (lba >= device->total_sectors) {
        errno = ERANGE;
        return -1;
    }

    offset = (off_t)(lba * device->logical_sector_size);

    for (attempt = 0; attempt < device->retry_count; ++attempt) {
        if (is_write) {
            result = pwrite(device->fd, write_buffer, device->logical_sector_size, offset);
        } else {
            result = pread(device->fd, buffer, device->logical_sector_size, offset);
        }

        if (result == (ssize_t)device->logical_sector_size) {
            return 0;
        }

        if (result < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
    }

    errno = EIO;
    return -1;
}

int device_read_sector(const device_handle_t *device, uint64_t lba, uint8_t *buffer) {
    return checked_io(false, device, lba, buffer, NULL);
}

int device_reread_sector(const device_handle_t *device, uint64_t lba, uint8_t *buffer) {
    return checked_io(false, device, lba, buffer, NULL);
}

int device_write_sector(const device_handle_t *device, uint64_t lba, const uint8_t *buffer) {
    uint8_t *verify_buffer;
    int rc;

    if (device == NULL || buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    verify_buffer = malloc(device->logical_sector_size);
    if (verify_buffer == NULL) {
        errno = ENOMEM;
        return -1;
    }

    rc = checked_io(true, device, lba, verify_buffer, buffer);
    if (rc == 0) {
        if (fdatasync(device->fd) != 0) {
            rc = -1;
        } else if (device_reread_sector(device, lba, verify_buffer) != 0 ||
                   memcmp(verify_buffer, buffer, device->logical_sector_size) != 0) {
            errno = EIO;
            rc = -1;
        }
    }

    free(verify_buffer);
    return rc;
}
