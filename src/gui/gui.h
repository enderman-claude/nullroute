/* gui.h - widgets used by the game screens. Built on raylib; the core never
 * includes this header. */
#ifndef NR_GUI_H
#define NR_GUI_H

#include "raylib.h"
#include "nr.h"

#define TERM_MAX_LINES 2000

/* Colours of the terminal "phosphor" theme. */
#define C_BG      ((Color){10, 14, 12, 255})
#define C_PANEL   ((Color){16, 22, 19, 255})
#define C_EDGE    ((Color){40, 70, 52, 255})
#define C_TEXT    ((Color){150, 235, 160, 255})
#define C_PROMPT  ((Color){200, 255, 210, 255})
#define C_GOOD    ((Color){110, 255, 140, 255})
#define C_BAD     ((Color){255, 110, 100, 255})
#define C_INFO    ((Color){120, 220, 255, 255})
#define C_DIM     ((Color){80, 120, 95, 255})
#define C_ACCENT  ((Color){255, 200, 90, 255})

typedef struct Term {
    char **lines;
    int nlines;
    int cap;
    Buf partial;          /* output after the last newline */
    char input[512];
    int ilen;
    int cursor;
    char hist[64][512];
    int nhist;
    int hpos;
    int masked;           /* typing a password */
    int scroll;           /* rows scrolled up from the bottom */
    char prompt[160];
} Term;

void term_init(Term *t);
void term_free(Term *t);
void term_write(Term *t, const char *s, size_t n);
void term_draw(Term *t, Rectangle r, int focused);
/* Handles keys. Returns 1 if a command was submitted this frame. */
int term_update(Term *t, Game *g, int focused);

typedef struct Notepad {
    char *text;
    size_t len;
    size_t cap;
    size_t cursor;
    int open;
    Host *host;
    char path[NR_PATH_MAX];
    int dirty;
    char status[NR_PATH_MAX + 64];
    int scroll;
} Notepad;

void np_init(Notepad *n);
void np_free(Notepad *n);
void np_open(Notepad *n, Game *g, Host *h, const char *path);
void np_update(Notepad *n, Game *g, int focused);
void np_draw(Notepad *n, Rectangle r, int focused);

void netmap_draw(Game *g, Rectangle r);

/* Shared font helpers. */
void gui_font_init(const char *path);

/* Key presses are read from raylib's event queue, so a quick tap is never lost
 * between frames. Call input_begin_frame once per frame, before any widget. */
void input_begin_frame(void);
int key_pressed(int key);
Font gui_font(void);
float gui_cell_width(void);
float gui_cell_height(void);

#endif
