#define _POSIX_C_SOURCE 200809L

#include "device.h"
#include "disk.h"
#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s <device-or-image> [retries]\n"
            "Example: %s /dev/sdb 5\n",
            argv0,
            argv0);
}

int main(int argc, char **argv) {
    editor_state_t state;
    unsigned int retries = 3U;
    int rc;

    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 3) {
        retries = (unsigned int)strtoul(argv[2], NULL, 10);
        if (retries == 0U) {
            fprintf(stderr, "Retry count must be greater than zero\n");
            return EXIT_FAILURE;
        }
    }

    memset(&state, 0, sizeof(state));
    state.device.fd = -1;

    if (device_open(&state.device, argv[1], retries) != 0) {
        fprintf(stderr, "Failed to open %s: %s\n", argv[1], strerror(errno));
        return EXIT_FAILURE;
    }

    state.sector_buf = malloc(state.device.logical_sector_size);
    state.original_buf = malloc(state.device.logical_sector_size);
    if (state.sector_buf == NULL || state.original_buf == NULL) {
        fprintf(stderr, "Not enough memory for sector buffers\n");
        device_close(&state.device);
        free(state.sector_buf);
        free(state.original_buf);
        return EXIT_FAILURE;
    }

    if (disk_probe_metadata(&state.device, &state.metadata) != 0) {
        snprintf(state.metadata.summary,
                 sizeof(state.metadata.summary),
                 "Metadata probe failed: %s",
                 strerror(errno));
    }

    snprintf(state.status,
             sizeof(state.status),
             "Opened %.220s, sector size %u",
             state.device.path,
             state.device.logical_sector_size);

    rc = ui_run(&state);

    free(state.sector_buf);
    free(state.original_buf);
    device_close(&state.device);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
