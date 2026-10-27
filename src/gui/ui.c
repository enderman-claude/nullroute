/* ui.c - the terminal, notepad and network map widgets. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define FONT_SIZE 20.0f
#define LINE_GAP 3.0f

static unsigned char g_key_seen[512];

static int g_ctrl_seen;

void input_begin_frame(void)
{
    memset(g_key_seen, 0, sizeof g_key_seen);
    for (int k = GetKeyPressed(); k != 0; k = GetKeyPressed()) {
        if (k > 0 && k < 512) {
            g_key_seen[k] = 1;
        }
    }
    /* Control counts as held if it is down now or was pressed this frame. */
    g_ctrl_seen = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                  g_key_seen[KEY_LEFT_CONTROL] || g_key_seen[KEY_RIGHT_CONTROL];
}

static int ctrl_down(void)
{
    return g_ctrl_seen;
}

int key_pressed(int key)
{
    if (key > 0 && key < 512 && g_key_seen[key]) {
        return 1;
    }
    return IsKeyPressed(key);
}

static Font g_font;
static int g_font_ready = 0;

/* Loads the bundled monospace font. Falls back to raylib's default font if
 * the file is missing, so the game still runs. Call once after InitWindow. */
void gui_font_init(const char *path)
{
    if (FileExists(path)) {
        g_font = LoadFontEx(path, 24, NULL, 0);
        if (g_font.texture.id != 0) {
            SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
            g_font_ready = 1;
            return;
        }
    }
    g_font = GetFontDefault();
    g_font_ready = 1;
}

Font gui_font(void)
{
    if (!g_font_ready) {
        g_font = GetFontDefault();
        g_font_ready = 1;
    }
    return g_font;
}

float gui_cell_width(void)
{
    return MeasureTextEx(gui_font(), "M", FONT_SIZE, 0).x;
}

float gui_cell_height(void)
{
    return FONT_SIZE + LINE_GAP;
}

static void draw_text_at(const char *s, size_t n, float x, float y, Color col)
{
    char tmp[1024];
    if (n >= sizeof tmp) {
        n = sizeof tmp - 1;
    }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    DrawTextEx(gui_font(), tmp, (Vector2){x, y}, FONT_SIZE, 0, col);
}

static Color line_colour(const char *s)
{
    if (strncmp(s, "[+]", 3) == 0 || strstr(s, "Welcome to") != NULL || strstr(s, "COMPLETE") != NULL) {
        return C_GOOD;
    }
    if (strncmp(s, "[-]", 3) == 0 || strncmp(s, "!!!", 3) == 0 || strncmp(s, "sh:", 3) == 0 ||
        strstr(s, "denied") != NULL || strstr(s, "Denied") != NULL || strstr(s, "not found") != NULL ||
        strstr(s, "Access denied") != NULL || strstr(s, "closed") != NULL) {
        return C_BAD;
    }
    if (strncmp(s, "[*]", 3) == 0 || strncmp(s, "[mail]", 6) == 0 || strncmp(s, "===", 3) == 0) {
        return C_INFO;
    }
    return C_TEXT;
}

void term_init(Term *t)
{
    memset(t, 0, sizeof *t);
    buf_init(&t->partial);
}

void term_free(Term *t)
{
    for (int i = 0; i < t->nlines; i++) {
        free(t->lines[i]);
    }
    free(t->lines);
    buf_free(&t->partial);
    memset(t, 0, sizeof *t);
}

static void push_line(Term *t, const char *s, size_t n)
{
    if (t->nlines == TERM_MAX_LINES) {
        free(t->lines[0]);
        memmove(t->lines, t->lines + 1, sizeof *t->lines * (TERM_MAX_LINES - 1));
        t->nlines--;
    }
    if (t->nlines == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 128;
        t->lines = realloc(t->lines, sizeof *t->lines * (size_t)t->cap);
        if (t->lines == NULL) {
            abort();
        }
    }
    t->lines[t->nlines++] = nr_strndup(s, n);
}

void term_write(Term *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            push_line(t, t->partial.data ? t->partial.data : "", t->partial.len);
            buf_free(&t->partial);
            buf_init(&t->partial);
        } else if (s[i] != '\r') {
            buf_char(&t->partial, s[i]);
        }
    }
    t->scroll = 0;
}

static void history_add(Term *t, const char *line)
{
    if (line[0] == '\0') {
        return;
    }
    if (t->nhist > 0 && strcmp(t->hist[t->nhist - 1], line) == 0) {
        return;
    }
    if (t->nhist == 64) {
        memmove(t->hist, t->hist + 1, sizeof t->hist[0] * 63);
        t->nhist--;
    }
    snprintf(t->hist[t->nhist++], sizeof t->hist[0], "%s", line);
}

static void input_set(Term *t, const char *s)
{
    snprintf(t->input, sizeof t->input, "%s", s);
    t->ilen = (int)strlen(t->input);
    t->cursor = t->ilen;
}

static void input_insert(Term *t, char ch)
{
    if (t->ilen >= (int)sizeof t->input - 2) {
        return;
    }
    memmove(t->input + t->cursor + 1, t->input + t->cursor, (size_t)(t->ilen - t->cursor + 1));
    t->input[t->cursor++] = ch;
    t->ilen++;
}

static void input_delete_at(Term *t, int pos)
{
    if (pos < 0 || pos >= t->ilen) {
        return;
    }
    memmove(t->input + pos, t->input + pos + 1, (size_t)(t->ilen - pos));
    t->ilen--;
}

/* Tab completion: command names for the first word, file names otherwise. */
static void complete(Term *t, Game *g)
{
    if (t->cursor != t->ilen) {
        return;
    }
    int start = t->ilen;
    while (start > 0 && t->input[start - 1] != ' ') {
        start--;
    }
    char prefix[256];
    snprintf(prefix, sizeof prefix, "%.*s", t->ilen - start, t->input + start);
    int first_word = start == 0;

    char matches[64][NR_NAME_MAX];
    int nm = 0;
    if (first_word) {
        for (int i = 0; nr_coreutils[i].name != NULL && nm < 64; i++) {
            if (strncmp(nr_coreutils[i].name, prefix, strlen(prefix)) == 0) {
                snprintf(matches[nm++], NR_NAME_MAX, "%s", nr_coreutils[i].name);
            }
        }
        for (int i = 0; nr_tools[i].name != NULL && nm < 64; i++) {
            if (strncmp(nr_tools[i].name, prefix, strlen(prefix)) == 0) {
                snprintf(matches[nm++], NR_NAME_MAX, "%s", nr_tools[i].name);
            }
        }
    } else {
        Session *s = g->top;
        char dir[NR_PATH_MAX];
        char leaf[256];
        const char *slash = strrchr(prefix, '/');
        if (slash != NULL) {
            snprintf(leaf, sizeof leaf, "%s", slash + 1);
            char dirarg[256];
            snprintf(dirarg, sizeof dirarg, "%.*s", (int)(slash - prefix) + (slash == prefix ? 1 : 0), prefix);
            if (path_join(s->cwd, dirarg, dir, sizeof dir) != 0) {
                return;
            }
        } else {
            snprintf(leaf, sizeof leaf, "%s", prefix);
            snprintf(dir, sizeof dir, "%s", s->cwd);
        }
        Node *d = vfs_get(s->host->fs, dir);
        if (d != NULL && d->is_dir) {
            for (Node *c = d->child; c != NULL && nm < 64; c = c->next) {
                if (c->name[0] == '.' && leaf[0] != '.') {
                    continue;
                }
                if (strncmp(c->name, leaf, strlen(leaf)) == 0) {
                    snprintf(matches[nm++], NR_NAME_MAX, "%s%s", c->name, c->is_dir ? "/" : "");
                }
            }
        }
    }
    if (nm == 0) {
        return;
    }
    if (nm == 1) {
        char rebuilt[512];
        snprintf(rebuilt, sizeof rebuilt, "%.*s%s%s", start, t->input, matches[0],
                 matches[0][strlen(matches[0]) - 1] == '/' ? "" : " ");
        input_set(t, rebuilt);
        return;
    }
    /* Several matches: list them and extend the common prefix. */
    Buf list;
    buf_init(&list);
    for (int i = 0; i < nm; i++) {
        buf_fmt(&list, "%s%s", i ? "  " : "", matches[i]);
    }
    term_write(t, list.data, list.len);
    term_write(t, "\n", 1);
    buf_free(&list);
}

static void submit(Term *t, Game *g)
{
    Buf out;
    buf_init(&out);
    if (t->masked) {
        /* The password is never echoed, not even into the scrollback. */
        term_write(t, "\n", 1);
        game_password(g, t->input, &out);
        term_write(t, out.data ? out.data : "", out.len);
        t->masked = 0;
        input_set(t, "");
        buf_free(&out);
        return;
    }
    buf_fmt(&out, "%s%s", t->prompt, t->input);
    term_write(t, out.data, out.len);
    term_write(t, "\n", 1);
    buf_free(&out);

    history_add(t, t->input);
    t->hpos = t->nhist;
    char line[512];
    snprintf(line, sizeof line, "%s", t->input);
    input_set(t, "");

    buf_init(&out);
    game_exec(g, line, &out);
    term_write(t, out.data ? out.data : "", out.len);
    buf_free(&out);
}

static int pressed(int key)
{
    return key_pressed(key) || IsKeyPressedRepeat(key);
}

int term_update(Term *t, Game *g, int focused)
{
    int ran = 0;
    int ctrl = ctrl_down();

    /* Ask the game for the prompt, or the password prompt while masked. */
    if (g->password_pending && !t->masked) {
        t->masked = 1;
        input_set(t, "");
    }
    if (t->masked) {
        snprintf(t->prompt, sizeof t->prompt, "Password: ");
    } else {
        Buf p;
        buf_init(&p);
        game_prompt(g, &p);
        snprintf(t->prompt, sizeof t->prompt, "%s", p.data ? p.data : "$ ");
        buf_free(&p);
    }

    /* Only the focused widget reads typed characters. */
    if (!focused) {
        return 0;
    }

    for (int ch = GetCharPressed(); ch != 0; ch = GetCharPressed()) {
        if (ctrl || ch < 32 || ch > 126) {
            continue;
        }
        input_insert(t, (char)ch);
    }

    if (ctrl && pressed(KEY_L)) {
        for (int i = 0; i < t->nlines; i++) {
            free(t->lines[i]);
        }
        t->nlines = 0;
    } else if (ctrl && pressed(KEY_C)) {
        Buf line;
        buf_init(&line);
        buf_fmt(&line, "%s%s^C", t->prompt, t->masked ? "" : t->input);
        term_write(t, line.data, line.len);
        term_write(t, "\n", 1);
        buf_free(&line);
        t->masked = 0;
        input_set(t, "");
    }

    if (pressed(KEY_ENTER) || pressed(KEY_KP_ENTER)) {
        submit(t, g);
        ran = 1;
    } else if (pressed(KEY_BACKSPACE)) {
        if (t->cursor > 0) {
            input_delete_at(t, t->cursor - 1);
            t->cursor--;
        }
    } else if (pressed(KEY_DELETE)) {
        input_delete_at(t, t->cursor);
    } else if (pressed(KEY_LEFT)) {
        if (t->cursor > 0) {
            t->cursor--;
        }
    } else if (pressed(KEY_RIGHT)) {
        if (t->cursor < t->ilen) {
            t->cursor++;
        }
    } else if (pressed(KEY_HOME)) {
        t->cursor = 0;
    } else if (pressed(KEY_END)) {
        t->cursor = t->ilen;
    } else if (pressed(KEY_UP) && !t->masked) {
        if (t->hpos > 0) {
            t->hpos--;
            input_set(t, t->hist[t->hpos]);
        }
    } else if (pressed(KEY_DOWN) && !t->masked) {
        if (t->hpos < t->nhist) {
            t->hpos++;
            input_set(t, t->hpos < t->nhist ? t->hist[t->hpos] : "");
        }
    } else if (pressed(KEY_TAB) && !t->masked) {
        complete(t, g);
    } else if (pressed(KEY_PAGE_UP)) {
        t->scroll += 10;
    } else if (pressed(KEY_PAGE_DOWN)) {
        t->scroll -= 10;
        if (t->scroll < 0) {
            t->scroll = 0;
        }
    }

    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        t->scroll += (int)(wheel * 3);
        if (t->scroll < 0) {
            t->scroll = 0;
        }
    }
    if (g->password_pending && !t->masked) {
        t->masked = 1;
    }
    return ran;
}

void term_draw(Term *t, Rectangle r, int focused)
{
    DrawRectangleRec(r, C_BG);
    DrawRectangleLinesEx(r, 2, focused ? C_ACCENT : C_EDGE);

    float cw = gui_cell_width();
    float ch = gui_cell_height();
    int cols = (int)((r.width - 20) / cw);
    int rows_visible = (int)((r.height - 16) / ch);
    if (cols < 10 || rows_visible < 1) {
        return;
    }

    /* Logical lines: scrollback, the partial line, then the input line. */
    Buf input_line;
    buf_init(&input_line);
    buf_str(&input_line, t->prompt);
    if (t->masked) {
        for (int i = 0; i < t->ilen; i++) {
            buf_char(&input_line, '*');
        }
    } else {
        buf_put(&input_line, t->input, (size_t)t->ilen);
    }
    int cursor_in_input = (int)strlen(t->prompt) + t->cursor;

    int total = 0;
    for (int i = 0; i < t->nlines; i++) {
        size_t l = strlen(t->lines[i]);
        total += l == 0 ? 1 : (int)((l + (size_t)cols - 1) / (size_t)cols);
    }
    if (t->partial.len > 0) {
        total += (int)((t->partial.len + (size_t)cols - 1) / (size_t)cols);
    }
    total += (int)((input_line.len + (size_t)cols) / (size_t)cols) + 1;

    int max_scroll = total - rows_visible;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (t->scroll > max_scroll) {
        t->scroll = max_scroll;
    }
    int first = total - rows_visible - t->scroll;
    if (first < 0) {
        first = 0;
    }

    int row = 0;
    float x0 = r.x + 10;
    float y0 = r.y + 8;

#define EMIT_ROWS(str, len, colour)                                                   \
    do {                                                                              \
        size_t _n = (len);                                                            \
        size_t _off = 0;                                                              \
        do {                                                                          \
            size_t _take = _n - _off;                                                 \
            if (_take > (size_t)cols) {                                               \
                _take = (size_t)cols;                                                 \
            }                                                                         \
            if (row >= first && row < first + rows_visible) {                         \
                draw_text_at((str) + _off, _take, x0, y0 + (float)(row - first) * ch, colour); \
            }                                                                         \
            row++;                                                                    \
            _off += _take;                                                            \
        } while (_off < _n);                                                          \
    } while (0)

    for (int i = 0; i < t->nlines; i++) {
        size_t l = strlen(t->lines[i]);
        if (l == 0) {
            row++;
            continue;
        }
        EMIT_ROWS(t->lines[i], l, line_colour(t->lines[i]));
    }
    if (t->partial.len > 0) {
        EMIT_ROWS(t->partial.data, t->partial.len, C_TEXT);
    }
    int cursor_row = row + cursor_in_input / cols;
    int cursor_col = cursor_in_input % cols;
    EMIT_ROWS(input_line.data, input_line.len, C_PROMPT);
#undef EMIT_ROWS

    if (focused && ((int)(GetTime() * 2) % 2) == 0) {
        int cr = cursor_row;
        if (cr >= first && cr < first + rows_visible) {
            DrawRectangle((int)(x0 + (float)cursor_col * cw), (int)(y0 + (float)(cr - first) * ch),
                          (int)cw, (int)ch - 2, C_PROMPT);
        }
    }
    buf_free(&input_line);
}

/* ---------- notepad ---------- */

void np_init(Notepad *n)
{
    memset(n, 0, sizeof *n);
}

void np_free(Notepad *n)
{
    free(n->text);
    memset(n, 0, sizeof *n);
}

void np_open(Notepad *n, Game *g, Host *h, const char *path)
{
    free(n->text);
    n->text = NULL;
    n->len = n->cap = n->cursor = 0;
    n->scroll = 0;
    n->host = h;
    snprintf(n->path, sizeof n->path, "%s", path);
    size_t len = 0;
    char *data = game_fs_read(h, path, &len);
    if (data != NULL) {
        n->text = data;
        n->len = len;
        n->cap = len + 1;
        snprintf(n->status, sizeof n->status, "opened %s", path);
    } else {
        snprintf(n->status, sizeof n->status, "new file %s", path);
    }
    if (n->text == NULL) {
        n->text = malloc(256);
        n->cap = 256;
        n->len = 0;
        if (n->text == NULL) {
            abort();
        }
    }
    (void)g;
    n->open = 1;
    n->dirty = 0;
}

static void np_insert(Notepad *n, char ch)
{
    if (n->len + 2 >= n->cap) {
        size_t cap = n->cap ? n->cap * 2 : 256;
        char *nt = realloc(n->text, cap);
        if (nt == NULL) {
            abort();
        }
        n->text = nt;
        n->cap = cap;
    }
    memmove(n->text + n->cursor + 1, n->text + n->cursor, n->len - n->cursor);
    n->text[n->cursor++] = ch;
    n->len++;
    n->text[n->len] = '\0';
    n->dirty = 1;
}

static void np_delete_before(Notepad *n)
{
    if (n->cursor == 0) {
        return;
    }
    memmove(n->text + n->cursor - 1, n->text + n->cursor, n->len - n->cursor);
    n->cursor--;
    n->len--;
    n->text[n->len] = '\0';
    n->dirty = 1;
}

static void np_delete_at(Notepad *n)
{
    if (n->cursor >= n->len) {
        return;
    }
    memmove(n->text + n->cursor, n->text + n->cursor + 1, n->len - n->cursor - 1);
    n->len--;
    n->text[n->len] = '\0';
    n->dirty = 1;
}

static size_t line_start(const Notepad *n, size_t pos)
{
    while (pos > 0 && n->text[pos - 1] != '\n') {
        pos--;
    }
    return pos;
}

static size_t line_end(const Notepad *n, size_t pos)
{
    while (pos < n->len && n->text[pos] != '\n') {
        pos++;
    }
    return pos;
}

static void np_move_vertical(Notepad *n, int dir)
{
    size_t start = line_start(n, n->cursor);
    size_t col = n->cursor - start;
    if (dir < 0) {
        if (start == 0) {
            return;
        }
        size_t prev_end = start - 1;
        size_t prev_start = line_start(n, prev_end);
        size_t plen = prev_end - prev_start;
        n->cursor = prev_start + (col < plen ? col : plen);
    } else {
        size_t end = line_end(n, n->cursor);
        if (end >= n->len) {
            return;
        }
        size_t next_start = end + 1;
        size_t next_end = line_end(n, next_start);
        size_t nlen = next_end - next_start;
        n->cursor = next_start + (col < nlen ? col : nlen);
    }
}

static int np_save(Notepad *n, Game *g)
{
    if (!n->open || n->host == NULL) {
        return -1;
    }
    if (game_fs_write(g, n->host, n->path, n->text ? n->text : "", n->len) != 0) {
        snprintf(n->status, sizeof n->status, "could not save %s", n->path);
        return -1;
    }
    n->dirty = 0;
    snprintf(n->status, sizeof n->status, "saved %s (%lu bytes)", n->path, (unsigned long)n->len);
    return 0;
}

void np_update(Notepad *n, Game *g, int focused)
{
    if (!n->open || !focused) {
        return;
    }
    int ctrl = ctrl_down();
    for (int ch = GetCharPressed(); ch != 0; ch = GetCharPressed()) {
        if (!ctrl && (ch == '\t' || (ch >= 32 && ch <= 126))) {
            np_insert(n, (char)ch);
        }
    }
    if (ctrl && (key_pressed(KEY_S))) {
        np_save(n, g);
        return;
    }
    if (key_pressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER)) {
        np_insert(n, '\n');
    } else if (key_pressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        np_delete_before(n);
    } else if (key_pressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) {
        np_delete_at(n);
    } else if (key_pressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
        if (n->cursor > 0) {
            n->cursor--;
        }
    } else if (key_pressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        if (n->cursor < n->len) {
            n->cursor++;
        }
    } else if (key_pressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
        np_move_vertical(n, -1);
    } else if (key_pressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
        np_move_vertical(n, 1);
    } else if (key_pressed(KEY_HOME)) {
        n->cursor = line_start(n, n->cursor);
    } else if (key_pressed(KEY_END)) {
        n->cursor = line_end(n, n->cursor);
    }
}

void np_draw(Notepad *n, Rectangle r, int focused)
{
    DrawRectangleRec(r, C_PANEL);
    DrawRectangleLinesEx(r, 2, focused ? C_ACCENT : C_EDGE);
    float cw = gui_cell_width();
    float ch = gui_cell_height();
    DrawTextEx(gui_font(), "NOTEPAD", (Vector2){r.x + 10, r.y + 6}, 16, 0, C_DIM);
    DrawTextEx(gui_font(), "Ctrl+S save   F2 switch", (Vector2){r.x + r.width - 210, r.y + 6}, 14, 0, C_DIM);
    if (!n->open) {
        DrawTextEx(gui_font(), "No file open. Type  notepad FILE  in the terminal.",
                   (Vector2){r.x + 10, r.y + 34}, FONT_SIZE, 0, C_DIM);
        return;
    }
    char title[NR_PATH_MAX + 64];
    snprintf(title, sizeof title, "%s%s", n->path, n->dirty ? "  [modified]" : "");
    DrawTextEx(gui_font(), title, (Vector2){r.x + 90, r.y + 6}, 16, 0, C_INFO);

    int cols = (int)((r.width - 20) / cw);
    int rows = (int)((r.height - 60) / ch);
    if (cols < 4 || rows < 1) {
        return;
    }

    /* Keep the cursor line on screen. */
    int cur_line = 0;
    size_t cur_start = 0;
    for (size_t i = 0; i < n->cursor && i < n->len; i++) {
        if (n->text[i] == '\n') {
            cur_line++;
            cur_start = i + 1;
        }
    }
    int cur_col = (int)(n->cursor - cur_start);
    if (cur_line < n->scroll) {
        n->scroll = cur_line;
    }
    if (cur_line >= n->scroll + rows) {
        n->scroll = cur_line - rows + 1;
    }

    size_t pos = 0;
    int line = 0;
    while (pos <= n->len && line < n->scroll) {
        char *nl = memchr(n->text + pos, '\n', n->len - pos);
        if (nl == NULL) {
            pos = n->len + 1;
            break;
        }
        pos = (size_t)(nl - n->text) + 1;
        line++;
    }
    float y = r.y + 34;
    for (int shown = 0; shown < rows && pos <= n->len; shown++) {
        char *nl = memchr(n->text + pos, '\n', n->len - pos);
        size_t end = nl ? (size_t)(nl - n->text) : n->len;
        size_t l = end - pos;
        if (l > (size_t)cols) {
            l = (size_t)cols;
        }
        char tmp[512];
        if (l >= sizeof tmp) {
            l = sizeof tmp - 1;
        }
        memcpy(tmp, n->text + pos, l);
        tmp[l] = '\0';
        char num[16];
        snprintf(num, sizeof num, "%3d ", n->scroll + shown + 1);
        DrawTextEx(gui_font(), num, (Vector2){r.x + 6, y}, FONT_SIZE, 0, C_DIM);
        DrawTextEx(gui_font(), tmp, (Vector2){r.x + 50, y}, FONT_SIZE, 0, C_TEXT);
        if (n->scroll + shown == cur_line && focused && ((int)(GetTime() * 2) % 2) == 0) {
            DrawRectangle((int)(r.x + 50 + (float)cur_col * cw), (int)y, (int)cw, (int)ch - 2, C_PROMPT);
        }
        y += ch;
        if (nl == NULL) {
            break;
        }
        pos = end + 1;
    }
    DrawTextEx(gui_font(), n->status, (Vector2){r.x + 10, r.y + r.height - 22}, 16, 0, C_DIM);
}

/* ---------- network map ---------- */

void netmap_draw(Game *g, Rectangle r)
{
    DrawRectangleRec(r, C_PANEL);
    DrawRectangleLinesEx(r, 2, C_EDGE);
    DrawTextEx(gui_font(), "NETWORK", (Vector2){r.x + 10, r.y + 6}, 16, 0, C_DIM);

    float pad = 16;
    float w = 118;
    float h = 42;
    float ax = r.x + pad;
    float ay = r.y + 36;
    float aw = r.width - 2 * pad - w;
    float ah = r.height - 36 - h - 10;
    if (aw < 10 || ah < 10) {
        return;
    }

    /* Links between discovered hosts. */
    for (int i = 0; i < g->nhosts; i++) {
        Host *a = &g->hosts[i];
        if (!(a == g->rig || a->discovered)) {
            continue;
        }
        for (int k = 0; k < a->nlinks; k++) {
            Host *b = host_find(g->hosts, g->nhosts, a->links[k]);
            if (b == NULL || !(b == g->rig || b->discovered)) {
                continue;
            }
            float x1 = ax + a->x * aw + w / 2;
            float y1 = ay + a->y * ah + h / 2;
            float x2 = ax + b->x * aw + w / 2;
            float y2 = ay + b->y * ah + h / 2;
            DrawLineEx((Vector2){x1, y1}, (Vector2){x2, y2}, 2, C_EDGE);
        }
    }

    for (int i = 0; i < g->nhosts; i++) {
        Host *hst = &g->hosts[i];
        if (!(hst == g->rig || hst->discovered)) {
            continue;
        }
        float x = ax + hst->x * aw;
        float y = ay + hst->y * ah;
        Color edge = hst->owned ? C_BAD : (hst == g->rig ? C_INFO : C_GOOD);
        DrawRectangle((int)x, (int)y, (int)w, (int)h, C_BG);
        DrawRectangleLinesEx((Rectangle){x, y, w, h}, 2, edge);
        DrawTextEx(gui_font(), hst->id, (Vector2){x + 6, y + 4}, 16, 0, C_PROMPT);
        DrawTextEx(gui_font(), hst->owned ? "ROOTED" : (hst == g->rig ? "your rig" : hst->ip),
                   (Vector2){x + 6, y + 23}, 14, 0, edge);
    }
}
