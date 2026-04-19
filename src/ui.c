#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "curses_compat.h"
#include "device.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_status(editor_state_t *state, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(state->status, sizeof(state->status), fmt, ap);
    va_end(ap);
}

static int reload_sector(editor_state_t *state, uint64_t lba) {
    if (device_read_sector(&state->device, lba, state->sector_buf) != 0) {
        set_status(state, "Read failed for LBA %" PRIu64 ": %s", lba, strerror(errno));
        return -1;
    }

    memcpy(state->original_buf, state->sector_buf, state->device.logical_sector_size);
    state->current_lba = lba;
    state->cursor = 0;
    state->dirty = false;
    state->edit_high_nibble = true;
    set_status(state, "Loaded sector %" PRIu64, lba);
    return 0;
}

static char printable(uint8_t value) {
    return isprint(value) ? (char)value : '.';
}

static void draw_metadata(WINDOW *win, const editor_state_t *state, int height, int width) {
    int row = 1;
    size_t i;

    box(win, 0, 0);
    mvwprintw(win, 0, 2, " Device ");
    mvwprintw(win, row++, 2, "Path: %s", state->device.path);
    mvwprintw(win, row++, 2, "Sector size: %u", state->device.logical_sector_size);
    mvwprintw(win, row++, 2, "Total sectors: %" PRIu64, state->device.total_sectors);
    mvwprintw(win, row++, 2, "Layout: %s",
              state->metadata.layout == DISK_LAYOUT_GPT ? "GPT" :
              state->metadata.layout == DISK_LAYOUT_GPT_PROTECTIVE ? "Protective MBR" :
              state->metadata.layout == DISK_LAYOUT_MBR ? "MBR" : "Unknown");
    mvwprintw(win, row++, 2, "%s", state->metadata.summary);

    if (row < height - 1) {
        mvwprintw(win, row++, 2, "Partitions:");
    }

    for (i = 0; i < state->metadata.partition_count && row < height - 1; ++i) {
        const partition_info_t *part = &state->metadata.partitions[i];
        mvwprintw(win,
                  row++,
                  2,
                  "#%" PRIu64 " %" PRIu64 "-%" PRIu64 " %.40s",
                  part->index,
                  part->first_lba,
                  part->last_lba,
                  part->description);
    }

    while (row < height - 1) {
        mvwprintw(win, row++, 2, "%-*s", width - 4, "");
    }
}

static void draw_sector(WINDOW *win, const editor_state_t *state, int height) {
    static const int bytes_per_row = 16;
    uint32_t sector_size = state->device.logical_sector_size;
    int visible_rows = height - 2;
    int row;

    box(win, 0, 0);
    mvwprintw(win, 0, 2, " Sector %" PRIu64 " ", state->current_lba);

    for (row = 0; row < visible_rows; ++row) {
        size_t base = (size_t)row * bytes_per_row;
        int col;

        if (base >= sector_size) {
            break;
        }

        mvwprintw(win, row + 1, 2, "%04zx:", base);

        for (col = 0; col < bytes_per_row; ++col) {
            size_t idx = base + (size_t)col;

            if (idx >= sector_size) {
                break;
            }

            if (idx == state->cursor) {
                wattron(win, A_REVERSE);
            }

            mvwprintw(win, row + 1, 9 + (col * 3), "%02X", state->sector_buf[idx]);

            if (idx == state->cursor) {
                wattroff(win, A_REVERSE);
            }
        }

        for (col = 0; col < bytes_per_row; ++col) {
            size_t idx = base + (size_t)col;

            if (idx >= sector_size) {
                break;
            }

            if (idx == state->cursor) {
                wattron(win, A_REVERSE);
            }

            mvwaddch(win, row + 1, 60 + col, printable(state->sector_buf[idx]));

            if (idx == state->cursor) {
                wattroff(win, A_REVERSE);
            }
        }
    }
}

static void draw_status(WINDOW *win, const editor_state_t *state, int width) {
    werase(win);
    mvwprintw(win,
              0,
              0,
              "Arrows move | PgUp/PgDn sector | g goto | r reread | w write | q quit | %s",
              state->dirty ? "modified" : "clean");
    mvwprintw(win, 1, 0, "%-*s", width - 1, state->status);
}

static int prompt_number(const char *label, uint64_t *value_out) {
    WINDOW *prompt;
    char buffer[32];
    int ch;
    size_t len = 0;

    prompt = newwin(5, 42, 3, 6);
    if (prompt == NULL) {
        return -1;
    }

    keypad(prompt, TRUE);
    memset(buffer, 0, sizeof(buffer));

    for (;;) {
        box(prompt, 0, 0);
        mvwprintw(prompt, 1, 2, "%s", label);
        mvwprintw(prompt, 2, 2, "> %s", buffer);
        wmove(prompt, 2, 4 + (int)len);
        wrefresh(prompt);

        ch = wgetch(prompt);
        if (ch == '\n' || ch == KEY_ENTER) {
            if (len == 0) {
                delwin(prompt);
                return -1;
            }
            *value_out = strtoull(buffer, NULL, 10);
            delwin(prompt);
            return 0;
        }
        if (ch == 27) {
            delwin(prompt);
            return -1;
        }
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && len > 0) {
            buffer[--len] = '\0';
            continue;
        }
        if (isdigit(ch) && len + 1 < sizeof(buffer)) {
            buffer[len++] = (char)ch;
            buffer[len] = '\0';
        }
    }
}

static void move_cursor(editor_state_t *state, int delta) {
    int64_t next = (int64_t)state->cursor + delta;

    if (next < 0) {
        next = 0;
    }
    if ((uint64_t)next >= state->device.logical_sector_size) {
        next = (int64_t)state->device.logical_sector_size - 1;
    }

    state->cursor = (size_t)next;
}

static bool hex_value(int ch, uint8_t *value) {
    if (ch >= '0' && ch <= '9') {
        *value = (uint8_t)(ch - '0');
        return true;
    }
    if (ch >= 'a' && ch <= 'f') {
        *value = (uint8_t)(10 + ch - 'a');
        return true;
    }
    if (ch >= 'A' && ch <= 'F') {
        *value = (uint8_t)(10 + ch - 'A');
        return true;
    }
    return false;
}

static void edit_current_byte(editor_state_t *state, int ch) {
    uint8_t value;
    uint8_t *target;

    if (!hex_value(ch, &value)) {
        return;
    }

    target = &state->sector_buf[state->cursor];
    if (state->edit_high_nibble) {
        *target = (uint8_t)((*target & 0x0F) | (value << 4));
        state->edit_high_nibble = false;
    } else {
        *target = (uint8_t)((*target & 0xF0) | value);
        state->edit_high_nibble = true;
        if (state->cursor + 1 < state->device.logical_sector_size) {
            state->cursor++;
        }
    }

    state->dirty = memcmp(state->sector_buf,
                          state->original_buf,
                          state->device.logical_sector_size) != 0;
}

static void handle_write(editor_state_t *state) {
    if (!state->dirty) {
        set_status(state, "Current sector has no changes");
        return;
    }

    if (device_write_sector(&state->device, state->current_lba, state->sector_buf) != 0) {
        set_status(state, "Write failed for LBA %" PRIu64 ": %s", state->current_lba, strerror(errno));
        return;
    }

    memcpy(state->original_buf, state->sector_buf, state->device.logical_sector_size);
    state->dirty = false;
    set_status(state, "Sector %" PRIu64 " written and verified", state->current_lba);
}

static void handle_reread(editor_state_t *state) {
    if (device_reread_sector(&state->device, state->current_lba, state->sector_buf) != 0) {
        set_status(state, "Re-read failed: %s", strerror(errno));
        return;
    }

    memcpy(state->original_buf, state->sector_buf, state->device.logical_sector_size);
    state->dirty = false;
    state->edit_high_nibble = true;
    set_status(state, "Sector %" PRIu64 " re-read from device", state->current_lba);
}

static void confirm_quit(editor_state_t *state) {
    if (state->dirty) {
        set_status(state, "Unsaved changes: press q again to quit without saving");
        state->quit_requested = true;
    } else {
        state->quit_requested = false;
    }
}

int ui_run(editor_state_t *state) {
    WINDOW *meta_win;
    WINDOW *sector_win;
    WINDOW *status_win;
    int rows;
    int cols;
    int ch;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    rows = getmaxy(stdscr);
    cols = getmaxx(stdscr);
    if (rows < 20 || cols < 90) {
        endwin();
        fprintf(stderr, "Terminal must be at least 90x20\n");
        return -1;
    }

    meta_win = newwin(rows - 3, cols / 3, 0, 0);
    sector_win = newwin(rows - 3, cols - (cols / 3), 0, cols / 3);
    status_win = newwin(3, cols, rows - 3, 0);
    if (meta_win == NULL || sector_win == NULL || status_win == NULL) {
        endwin();
        fprintf(stderr, "Failed to initialize ncurses windows\n");
        return -1;
    }

    if (reload_sector(state, 0) != 0) {
        delwin(meta_win);
        delwin(sector_win);
        delwin(status_win);
        endwin();
        return -1;
    }

    while (1) {
        werase(meta_win);
        werase(sector_win);
        draw_metadata(meta_win, state, rows - 3, cols / 3);
        draw_sector(sector_win, state, rows - 3);
        draw_status(status_win, state, cols);
        wrefresh(meta_win);
        wrefresh(sector_win);
        wrefresh(status_win);

        ch = wgetch(stdscr);
        if (state->quit_requested && ch != 'q') {
            state->quit_requested = false;
        }

        switch (ch) {
            case KEY_LEFT:
                move_cursor(state, -1);
                break;
            case KEY_RIGHT:
                move_cursor(state, 1);
                break;
            case KEY_UP:
                move_cursor(state, -16);
                break;
            case KEY_DOWN:
                move_cursor(state, 16);
                break;
            case KEY_NPAGE:
                if (state->current_lba + 1 < state->device.total_sectors) {
                    (void)reload_sector(state, state->current_lba + 1);
                }
                break;
            case KEY_PPAGE:
                if (state->current_lba > 0) {
                    (void)reload_sector(state, state->current_lba - 1);
                }
                break;
            case 'g': {
                uint64_t lba;
                if (prompt_number("Go to sector (LBA)", &lba) == 0) {
                    if (lba < state->device.total_sectors) {
                        (void)reload_sector(state, lba);
                    } else {
                        set_status(state, "LBA out of range");
                    }
                } else {
                    set_status(state, "Go to sector cancelled");
                }
                break;
            }
            case 'r':
                handle_reread(state);
                break;
            case 'w':
                handle_write(state);
                break;
            case 'q':
                if (state->quit_requested || !state->dirty) {
                    delwin(meta_win);
                    delwin(sector_win);
                    delwin(status_win);
                    endwin();
                    return 0;
                }
                confirm_quit(state);
                break;
            default:
                edit_current_byte(state, ch);
                if (state->dirty) {
                    set_status(state, "Editing byte %zu", state->cursor);
                }
                break;
        }
    }
}
