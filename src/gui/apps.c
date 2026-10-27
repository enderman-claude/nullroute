/* apps.c - the apps panel: a tab bar over cmpview, the notepad and the mail
 * reader, plus the syntax colouring used by the notepad. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define TAB_H 26.0f
#define LINE_H 22.0f
#define FONT_SIZE_TERM 20.0f

/* ---------- syntax colouring ---------- */

static const Color HL_KEYWORD = {200, 150, 255, 255};
static const Color HL_SHELL = {255, 170, 90, 255};
static const Color HL_COMMAND = {110, 190, 255, 255};
static const Color HL_STRING = {240, 220, 120, 255};
static const Color HL_VARIABLE = {120, 235, 235, 255};
static const Color HL_NUMBER = {250, 140, 140, 255};
static const Color HL_COMMENT = {90, 130, 100, 255};
static const Color HL_OPERATOR = {160, 170, 190, 255};

static const char *const SH_KEYWORDS[] = {
    "if", "then", "else", "elif", "fi", "for", "in", "do", "done", "while", "until",
    "case", "esac", "return", "break", "continue", NULL};
static const char *const SH_BUILTINS[] = {
    "cd", "export", "unset", "exit", "echo", "printf", "sh", "test", NULL};
static const char *const SH_COMMANDS[] = {
    "ls", "cat", "cp", "mv", "rm", "mkdir", "rmdir", "touch", "pwd", "head", "tail", "wc",
    "sort", "uniq", "cut", "tr", "tee", "seq", "basename", "dirname", "id", "whoami",
    "hostname", "uname", "env", "printenv", "date", "sleep", "true", "false", "chmod",
    "scan", "portscan", "crack", "ssh", "scp", "exploit", "notepad", "brief", "status",
    "publish", "sell", "help", "apps", "pin", "unpin", "close", "mail", "cmpview",
    "showserver", NULL};

Lang lang_for_path(const char *p)
{
    size_t l = strlen(p);
    if (strstr(p, "/mail/") != NULL) {
        return LANG_MAIL;
    }
    if ((l >= 3 && strcmp(p + l - 3, ".sh") == 0) || (l >= 8 && strcmp(p + l - 8, ".profile") == 0)) {
        return LANG_SH;
    }
    if ((l >= 4 && strcmp(p + l - 4, ".cfg") == 0) || (l >= 5 && strcmp(p + l - 5, ".conf") == 0) ||
        (l >= 4 && strcmp(p + l - 4, ".key") == 0)) {
        return LANG_CFG;
    }
    return LANG_TEXT;
}

static int in_list(const char *const *list, const char *word, size_t n)
{
    for (int i = 0; list[i] != NULL; i++) {
        if (strlen(list[i]) == n && strncmp(list[i], word, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Draws a run of text and moves x along it. Monospaced, so widths are exact. */
static void emit(const char *s, size_t n, Color col, float *x, float y)
{
    if (n == 0) {
        return;
    }
    char tmp[512];
    if (n >= sizeof tmp) {
        n = sizeof tmp - 1;
    }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    DrawTextEx(gui_font(), tmp, (Vector2){*x, y}, FONT_SIZE_TERM, 0, col);
    *x += gui_cell_width() * (float)n;
}

static void hl_shell(const char *s, size_t n, float *x, float y)
{
    size_t i = 0;
    size_t plain = 0; /* start of the pending plain run */
#define FLUSH_PLAIN(upto)                                                   \
    do {                                                                    \
        if ((upto) > plain) {                                               \
            emit(s + plain, (upto) - plain, C_TEXT, x, y);                  \
        }                                                                   \
    } while (0)

    while (i < n) {
        char c = s[i];
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == ';')) {
            FLUSH_PLAIN(i);
            emit(s + i, n - i, HL_COMMENT, x, y);
            return;
        }
        if (c == '"' || c == '\'') {
            FLUSH_PLAIN(i);
            size_t j = i + 1;
            while (j < n && s[j] != c) {
                j++;
            }
            if (j < n) {
                j++;
            }
            emit(s + i, j - i, HL_STRING, x, y);
            i = plain = j;
            continue;
        }
        if (c == '$') {
            FLUSH_PLAIN(i);
            size_t j = i + 1;
            if (j < n && s[j] == '{') {
                while (j < n && s[j] != '}') {
                    j++;
                }
                if (j < n) {
                    j++;
                }
            } else {
                while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) {
                    j++;
                }
            }
            emit(s + i, j - i, HL_VARIABLE, x, y);
            i = plain = j;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '-')) {
                j++;
            }
            size_t wl = j - i;
            Color col = C_TEXT;
            if (in_list(SH_KEYWORDS, s + i, wl)) {
                col = HL_KEYWORD;
            } else if (in_list(SH_BUILTINS, s + i, wl)) {
                col = HL_SHELL;
            } else if (in_list(SH_COMMANDS, s + i, wl)) {
                col = HL_COMMAND;
            }
            if (col.r != C_TEXT.r || col.g != C_TEXT.g || col.b != C_TEXT.b) {
                FLUSH_PLAIN(i);
                emit(s + i, wl, col, x, y);
                plain = j;
            }
            i = j;
            continue;
        }
        if (isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < n && isdigit((unsigned char)s[j])) {
                j++;
            }
            FLUSH_PLAIN(i);
            emit(s + i, j - i, HL_NUMBER, x, y);
            i = plain = j;
            continue;
        }
        if (strchr(";&|<>()=", c) != NULL) {
            FLUSH_PLAIN(i);
            emit(s + i, 1, HL_OPERATOR, x, y);
            i = plain = i + 1;
            continue;
        }
        i++;
    }
    FLUSH_PLAIN(n);
#undef FLUSH_PLAIN
}

static void hl_mail(const char *s, size_t n, float *x, float y)
{
    /* "Key: value" header lines get a coloured key. */
    size_t k = 0;
    while (k < n && (isalpha((unsigned char)s[k]) || s[k] == '-') && k < 16) {
        k++;
    }
    if (k > 0 && k < n && s[k] == ':') {
        emit(s, k + 1, HL_COMMAND, x, y);
        emit(s + k + 1, n - k - 1, C_TEXT, x, y);
        return;
    }
    if (n >= 2 && s[0] == '=' && s[1] == '=') {
        emit(s, n, C_ACCENT, x, y);
        return;
    }
    emit(s, n, C_TEXT, x, y);
}

static void hl_cfg(const char *s, size_t n, float *x, float y)
{
    if (n > 0 && s[0] == '#') {
        emit(s, n, HL_COMMENT, x, y);
        return;
    }
    const char *eq = memchr(s, '=', n);
    if (eq == NULL) {
        emit(s, n, C_TEXT, x, y);
        return;
    }
    size_t kl = (size_t)(eq - s);
    emit(s, kl, HL_VARIABLE, x, y);
    emit(eq, 1, HL_OPERATOR, x, y);
    emit(eq + 1, n - kl - 1, HL_STRING, x, y);
}

static void hl_text(const char *s, size_t n, float *x, float y)
{
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && !isdigit((unsigned char)s[j])) {
            j++;
        }
        emit(s + i, j - i, C_TEXT, x, y);
        size_t k = j;
        while (k < n && isdigit((unsigned char)s[k])) {
            k++;
        }
        emit(s + j, k - j, HL_NUMBER, x, y);
        i = k;
    }
}

void hl_draw_line(Lang lang, const char *s, size_t n, float x, float y)
{
    switch (lang) {
    case LANG_SH:
        hl_shell(s, n, &x, y);
        break;
    case LANG_MAIL:
        hl_mail(s, n, &x, y);
        break;
    case LANG_CFG:
        hl_cfg(s, n, &x, y);
        break;
    default:
        hl_text(s, n, &x, y);
        break;
    }
}

/* ---------- tabs ---------- */

/* Lays out one tab per open app. Returns how many there are. */
static int tab_layout(Game *g, Rectangle r, Rectangle out[APP_COUNT], int ids[APP_COUNT])
{
    int n = 0;
    float x = r.x + 8;
    for (int i = 0; i < APP_COUNT; i++) {
        if (!g->app_open[i]) {
            continue;
        }
        char label[32];
        snprintf(label, sizeof label, "%s%s", nr_app_names[i], g->app_pinned[i] ? " *" : "");
        Vector2 m = MeasureTextEx(gui_font(), label, 18, 0);
        out[n] = (Rectangle){x, r.y + 4, m.x + 20, TAB_H - 2};
        ids[n] = i;
        x += out[n].width + 6;
        n++;
    }
    return n;
}

static void ensure_active(Game *g, AppsView *av)
{
    if (av->active < 0 || av->active >= APP_COUNT || !g->app_open[av->active]) {
        av->active = APP_CMPVIEW;
    }
}

/* ---------- cmpview ---------- */

static void draw_cmpview(Game *g, Rectangle body)
{
    Buf b;
    buf_init(&b);
    game_cmpview(g, &b);
    float y = body.y + 4;
    size_t pos = 0;
    while (pos < b.len) {
        const char *nl = memchr(b.data + pos, '\n', b.len - pos);
        size_t end = nl ? (size_t)(nl - b.data) : b.len;
        Color col = C_TEXT;
        const char *line = b.data + pos;
        size_t len = end - pos;
        if (len >= 2 && line[0] == '=' && line[1] == '=') {
            col = C_ACCENT;
        } else if (len >= 6 && strncmp(line, "server", 6) == 0) {
            col = C_INFO;
        } else if (len >= 6 && strncmp(line, "banner", 6) == 0) {
            col = C_INFO;
        } else if (len >= 3 && strncmp(line, "tip", 3) == 0) {
            col = C_GOOD;
        } else if (len >= 6 && strstr(line, "ROOTED") != NULL) {
            col = C_BAD;
        }
        char tmp[256];
        if (len >= sizeof tmp) {
            len = sizeof tmp - 1;
        }
        memcpy(tmp, line, len);
        tmp[len] = '\0';
        DrawTextEx(gui_font(), tmp, (Vector2){body.x + 8, y}, 19, 0, col);
        y += LINE_H;
        pos = end + 1;
    }
    buf_free(&b);
}

/* ---------- mail ---------- */

static int mail_names(Game *g, char names[32][NR_NAME_MAX])
{
    Node *dir = vfs_get(g->rig->fs, "/home/ghost/mail");
    int n = 0;
    if (dir == NULL || !dir->is_dir) {
        return 0;
    }
    for (Node *c = dir->child; c != NULL && n < 32; c = c->next) {
        if (!c->is_dir) {
            snprintf(names[n++], NR_NAME_MAX, "%s", c->name);
        }
    }
    for (int a = 1; a < n; a++) {
        for (int b = a; b > 0 && strcmp(names[b - 1], names[b]) > 0; b--) {
            char t[NR_NAME_MAX];
            memcpy(t, names[b - 1], NR_NAME_MAX);
            memcpy(names[b - 1], names[b], NR_NAME_MAX);
            memcpy(names[b], t, NR_NAME_MAX);
        }
    }
    return n;
}

static void draw_mail(Game *g, AppsView *av, Rectangle body)
{
    char names[32][NR_NAME_MAX];
    int n = mail_names(g, names);
    if (n == 0) {
        DrawTextEx(gui_font(), "No mail.", (Vector2){body.x + 8, body.y + 4}, 19, 0, C_DIM);
        return;
    }
    if (av->mail_sel >= n) {
        av->mail_sel = n - 1;
    }
    float y = body.y + 4;
    int list_rows = n < 5 ? n : 5;
    for (int i = 0; i < list_rows; i++) {
        int sel = i == av->mail_sel;
        Rectangle row = {body.x, y - 2, body.width, LINE_H};
        if (sel) {
            DrawRectangleRec(row, C_EDGE);
        }
        char subj[128] = "";
        char path[NR_PATH_MAX];
        snprintf(path, sizeof path, "/home/%s/mail/%s", NR_PLAYER, names[i]);
        size_t l = 0;
        char *data = game_fs_read(g->rig, path, &l);
        if (data != NULL) {
            const char *sp = strstr(data, "Subject: ");
            if (sp != NULL) {
                size_t k = 0;
                while (sp[9 + k] != '\0' && sp[9 + k] != '\n' && k < sizeof subj - 1) {
                    subj[k] = sp[9 + k];
                    k++;
                }
                subj[k] = '\0';
            }
            free(data);
        }
        char line[200];
        snprintf(line, sizeof line, "%s  %s", names[i], subj);
        DrawTextEx(gui_font(), line, (Vector2){body.x + 8, y}, 19, 0, sel ? C_PROMPT : C_TEXT);
        y += LINE_H;
    }
    y += 6;
    DrawLineEx((Vector2){body.x, y}, (Vector2){body.x + body.width, y}, 1, C_EDGE);
    y += 8;

    char path[NR_PATH_MAX];
    snprintf(path, sizeof path, "/home/%s/mail/%s", NR_PLAYER, names[av->mail_sel]);
    size_t len = 0;
    char *data = game_fs_read(g->rig, path, &len);
    if (data == NULL) {
        return;
    }
    size_t pos = 0;
    float bottom = body.y + body.height - LINE_H;
    while (pos < len && y < bottom) {
        const char *nl = memchr(data + pos, '\n', len - pos);
        size_t end = nl ? (size_t)(nl - data) : len;
        float x = body.x + 8;
        hl_draw_line(LANG_MAIL, data + pos, end - pos, x, y);
        y += LINE_H;
        pos = end + 1;
    }
    free(data);
}

/* ---------- public ---------- */

void apps_draw(Game *g, Notepad *np, AppsView *av, Rectangle r, int focused)
{
    ensure_active(g, av);
    DrawRectangleRec(r, C_PANEL);
    DrawRectangleLinesEx(r, 2, focused ? C_ACCENT : C_EDGE);

    Rectangle tabs[APP_COUNT];
    int ids[APP_COUNT];
    int nt = tab_layout(g, r, tabs, ids);
    for (int t = 0; t < nt; t++) {
        int active = av->active == ids[t];
        char label[32];
        snprintf(label, sizeof label, "%s%s", nr_app_names[ids[t]], g->app_pinned[ids[t]] ? " *" : "");
        DrawRectangleRec(tabs[t], active ? C_EDGE : C_PANEL);
        DrawRectangleLinesEx(tabs[t], 1, active ? C_ACCENT : C_EDGE);
        DrawTextEx(gui_font(), label, (Vector2){tabs[t].x + 10, tabs[t].y + 3}, 18, 0,
                   active ? C_PROMPT : C_DIM);
    }
    DrawTextEx(gui_font(), "F3 next app", (Vector2){r.x + r.width - 110, r.y + 6}, 14, 0, C_DIM);

    Rectangle body = {r.x + 6, r.y + TAB_H + 8, r.width - 12, r.height - TAB_H - 14};
    switch (av->active) {
    case APP_NOTEPAD:
        np_draw(np, body, focused);
        break;
    case APP_MAIL:
        draw_mail(g, av, body);
        break;
    default:
        draw_cmpview(g, body);
        break;
    }
}

/* Returns 1 when the panel should hand the keyboard back to the terminal:
 * read-only apps do not take typing, so a letter or digit goes to the shell. */
int apps_update(Game *g, Notepad *np, AppsView *av, Rectangle r, int focused)
{
    ensure_active(g, av);
    Rectangle tabs[APP_COUNT];
    int ids[APP_COUNT];
    int nt = tab_layout(g, r, tabs, ids);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Vector2 mp = GetMousePosition();
        for (int t = 0; t < nt; t++) {
            if (CheckCollisionPointRec(mp, tabs[t])) {
                av->active = ids[t];
            }
        }
    }
    if (!focused) {
        return 0;
    }
    if (av->active != APP_NOTEPAD) {
        for (int k = 'A'; k <= 'Z'; k++) {
            if (key_pressed(k)) {
                return 1;
            }
        }
        for (int k = '0'; k <= '9'; k++) {
            if (key_pressed(k)) {
                return 1;
            }
        }
    }
    if (key_pressed(KEY_F3) && nt > 0) {
        int pos = 0;
        for (int t = 0; t < nt; t++) {
            if (ids[t] == av->active) {
                pos = t;
            }
        }
        av->active = ids[(pos + 1) % nt];
    }
    if (av->active == APP_NOTEPAD) {
        np_update(np, g, 1);
    } else if (av->active == APP_MAIL) {
        if (key_pressed(KEY_DOWN)) {
            av->mail_sel++;
        }
        if (key_pressed(KEY_UP) && av->mail_sel > 0) {
            av->mail_sel--;
        }
    }
    return 0;
}
