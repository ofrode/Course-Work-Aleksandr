#ifndef CURSES_COMPAT_H
#define CURSES_COMPAT_H

#if defined(__has_include)
#if __has_include(<ncurses.h>)
#include <ncurses.h>
#elif __has_include(<curses.h>)
#include <curses.h>
#else
#define CURSES_COMPAT_STUB 1
#endif
#else
#define CURSES_COMPAT_STUB 1
#endif

#ifdef CURSES_COMPAT_STUB

typedef unsigned int chtype;
typedef unsigned int attr_t;
typedef struct _win_st WINDOW;

extern WINDOW *stdscr;

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#define A_REVERSE 0U

#define KEY_DOWN 258
#define KEY_UP 259
#define KEY_LEFT 260
#define KEY_RIGHT 261
#define KEY_BACKSPACE 263
#define KEY_NPAGE 338
#define KEY_PPAGE 339
#define KEY_ENTER 343

WINDOW *initscr(void);
int endwin(void);
int cbreak(void);
int noecho(void);
int keypad(WINDOW *win, int bf);
int curs_set(int visibility);
WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x);
int delwin(WINDOW *win);
int werase(WINDOW *win);
int wrefresh(WINDOW *win);
int wgetch(WINDOW *win);
int wmove(WINDOW *win, int y, int x);
int box(WINDOW *win, chtype verch, chtype horch);
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
int mvwaddch(WINDOW *win, int y, int x, const chtype ch);
int wattron(WINDOW *win, int attrs);
int wattroff(WINDOW *win, int attrs);
int getmaxx(const WINDOW *win);
int getmaxy(const WINDOW *win);

#endif

#endif
