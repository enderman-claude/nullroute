/*
 * main.c - NULLROUTE window and screen flow.
 *
 *   Menu  ->  Intro  ->  Desktop (terminal, network map, notepad)
 *                          |  Esc
 *                          v
 *                        Pause  ->  Menu
 *   Desktop after the last chapter  ->  Ending  ->  Menu
 *
 * The game takes no command-line arguments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "gui.h"

typedef enum { SCR_MENU, SCR_INTRO, SCR_GAME, SCR_PAUSE, SCR_ENDING } Screen;

typedef struct App {
    Screen scr;
    Screen back;          /* where Pause returns to on resume */
    Game g;
    int game_active;
    Term term;
    Notepad np;
    AppsView av;
    int focus;            /* 0 terminal, 1 apps panel */
    int sel;              /* selected menu entry */
    int has_save;
    int quit;
    char world_path[1024];
    char save_path[2048];
    char font_path[1024];
    char save_dir[1024];
    char note[256];
    float intro_t;
    double save_due;      /* time after which an autosave may run */
} App;

/* ---------- paths ---------- */

static void setup_paths(App *a)
{
    const char *app_dir = GetApplicationDirectory();
    snprintf(a->world_path, sizeof a->world_path, "%s%cassets%cworld.txt", app_dir, '/', '/');

    const char *home = getenv("HOME");
#ifdef _WIN32
    if (home == NULL) {
        home = getenv("USERPROFILE");
    }
#endif
    if (home == NULL) {
        home = ".";
    }
    snprintf(a->font_path, sizeof a->font_path, "%s%cassets%cfonts%cDejaVuSansMono.ttf", app_dir, '/', '/', '/');
    snprintf(a->save_dir, sizeof a->save_dir, "%s/.nullroute", home);
    snprintf(a->save_path, sizeof a->save_path, "%s/save.dat", a->save_dir);
}

static void refresh_save_state(App *a)
{
    a->has_save = FileExists(a->save_path);
}

/* ---------- drawing helpers ---------- */

static void text_centred(const char *s, float y, int size, Color col)
{
    Font f = gui_font();
    float sp = 0;
    Vector2 m = MeasureTextEx(f, s, (float)size, sp);
    DrawTextEx(f, s, (Vector2){(GetScreenWidth() - m.x) / 2.0f, y}, (float)size, sp, col);
}

static int button(Rectangle r, const char *label, int selected, int enabled)
{
    Color bg = selected ? C_EDGE : C_PANEL;
    Color fg = enabled ? (selected ? C_PROMPT : C_TEXT) : C_DIM;
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 2, selected ? C_ACCENT : C_EDGE);
    Vector2 m = MeasureTextEx(gui_font(), label, 24, 0);
    DrawTextEx(gui_font(), label, (Vector2){r.x + (r.width - m.x) / 2, r.y + (r.height - m.y) / 2}, 24, 0, fg);
    if (!enabled) {
        return 0;
    }
    Vector2 mp = GetMousePosition();
    return CheckCollisionPointRec(mp, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

/* Hover only counts when the mouse has moved. A resting pointer over a button
 * must not steal the selection from the keyboard. */
static int hovered(Rectangle r)
{
    Vector2 d = GetMouseDelta();
    if (d.x == 0 && d.y == 0) {
        return 0;
    }
    return CheckCollisionPointRec(GetMousePosition(), r);
}

static void draw_backdrop(void)
{
    ClearBackground(C_BG);
    /* Faint scan lines: a cheap nod to the terminal look. */
    for (int y = 0; y < GetScreenHeight(); y += 4) {
        DrawRectangle(0, y, GetScreenWidth(), 1, (Color){0, 0, 0, 60});
    }
}

/* ---------- save and load ---------- */

static void autosave(App *a, int force)
{
    if (!a->game_active) {
        return;
    }
    if (!force && (!a->g.dirty || GetTime() < a->save_due)) {
        return;
    }
    MakeDirectory(a->save_dir);
    if (game_save(&a->g, a->save_path) == 0) {
        a->save_due = GetTime() + 2.0;
        snprintf(a->note, sizeof a->note, "saved");
        refresh_save_state(a);
    }
}

static void discard_game(App *a)
{
    if (a->game_active) {
        game_destroy(&a->g);
        a->game_active = 0;
    }
    term_free(&a->term);
    np_free(&a->np);
    term_init(&a->term);
    np_init(&a->np);
}

static void start_new(App *a)
{
    discard_game(a);
    remove(a->save_path);
    if (game_new(&a->g, a->world_path) != 0) {
        snprintf(a->note, sizeof a->note, "could not load the world file");
        return;
    }
    a->game_active = 1;
    a->av.active = APP_CMPVIEW;
    a->av.mail_sel = 0;
    a->intro_t = 0;
    a->scr = SCR_INTRO;
    a->focus = 0;
    refresh_save_state(a);
}

static void continue_game(App *a)
{
    discard_game(a);
    if (game_load(&a->g, a->world_path, a->save_path) != 0) {
        snprintf(a->note, sizeof a->note, "the save file could not be read");
        return;
    }
    a->game_active = 1;
    a->av.active = APP_CMPVIEW;
    a->av.mail_sel = 0;
    a->focus = 0;
    a->scr = SCR_GAME;
    /* Show any events that were waiting, and the rig's profile. */
    Buf out;
    buf_init(&out);
    game_login(&a->g, &out);
    term_write(&a->term, out.data ? out.data : "", out.len);
    buf_free(&out);
}

/* ---------- menu ---------- */

static void update_menu(App *a)
{
    int count = a->has_save ? 3 : 2;
    if (a->sel >= count) {
        a->sel = count - 1;
    }
    if (key_pressed(KEY_DOWN)) {
        a->sel = (a->sel + 1) % count;
    }
    if (key_pressed(KEY_UP)) {
        a->sel = (a->sel + count - 1) % count;
    }
    int activate = key_pressed(KEY_ENTER);
    int w = 340;
    int h = 52;
    int x = (GetScreenWidth() - w) / 2;
    int y = GetScreenHeight() / 2 - 20;
    const char *labels[3] = {"Continue", "New game", "Quit"};
    int order[3];
    int n = 0;
    if (a->has_save) {
        order[n++] = 0;
    }
    order[n++] = 1;
    order[n++] = 2;
    for (int i = 0; i < n; i++) {
        Rectangle r = {(float)x, (float)(y + i * (h + 14)), (float)w, (float)h};
        if (hovered(r)) {
            a->sel = i;
        }
        int pick = button(r, labels[order[i]], a->sel == i, 1);
        if (pick || (activate && a->sel == i)) {
            activate = 0;
            if (order[i] == 0) {
                continue_game(a);
            } else if (order[i] == 1) {
                start_new(a);
            } else {
                a->quit = 1;
            }
        }
    }
}

static void draw_menu(App *a)
{
    draw_backdrop();
    text_centred("NULLROUTE", GetScreenHeight() / 2.0f - 180, 80, C_PROMPT);
    text_centred("a hacking game inside a fake POSIX machine", GetScreenHeight() / 2.0f - 80, 20, C_DIM);
    int n = 0;
    const char *labels[3];
    if (a->has_save) {
        labels[n++] = "Continue";
    }
    labels[n++] = "New game";
    labels[n++] = "Quit";
    int w = 340;
    int h = 52;
    int x = (GetScreenWidth() - w) / 2;
    int y = GetScreenHeight() / 2 - 20;
    for (int i = 0; i < n; i++) {
        Rectangle r = {(float)x, (float)(y + i * (h + 14)), (float)w, (float)h};
        DrawRectangleRec(r, a->sel == i ? C_EDGE : C_PANEL);
        DrawRectangleLinesEx(r, 2, a->sel == i ? C_ACCENT : C_EDGE);
        Vector2 m = MeasureTextEx(gui_font(), labels[i], 24, 0);
        DrawTextEx(gui_font(), labels[i], (Vector2){r.x + (r.width - m.x) / 2, r.y + (r.height - m.y) / 2},
                   24, 0, a->sel == i ? C_PROMPT : C_TEXT);
    }
    if (a->note[0] != '\0') {
        text_centred(a->note, GetScreenHeight() - 60, 18, C_INFO);
    }
    text_centred("Up/Down and Enter, or click", GetScreenHeight() - 30, 16, C_DIM);
}

/* ---------- intro ---------- */

static void update_intro(App *a)
{
    const char *text = game_text(&a->g, "intro");
    size_t total = text ? strlen(text) : 0;
    size_t shown = (size_t)(a->intro_t * 90.0f);
    a->intro_t += GetFrameTime();
    if (key_pressed(KEY_ENTER) || key_pressed(KEY_SPACE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (shown < total) {
            a->intro_t = (float)total / 90.0f;
        } else {
            a->scr = SCR_GAME;
            Buf out;
            buf_init(&out);
            game_login(&a->g, &out);
            term_write(&a->term, out.data ? out.data : "", out.len);
            buf_free(&out);
        }
    }
}

static void draw_intro(App *a)
{
    draw_backdrop();
    const char *text = game_text(&a->g, "intro");
    size_t total = text ? strlen(text) : 0;
    size_t shown = (size_t)(a->intro_t * 90.0f);
    if (shown > total) {
        shown = total;
    }
    Rectangle box = {GetScreenWidth() * 0.15f, GetScreenHeight() * 0.12f,
                     GetScreenWidth() * 0.70f, GetScreenHeight() * 0.76f};
    DrawRectangleRec(box, C_PANEL);
    DrawRectangleLinesEx(box, 2, C_EDGE);
    if (text != NULL) {
        float cw = gui_cell_width();
        int cols = (int)((box.width - 60) / cw);
        int row = 0;
        float y = box.y + 30;
        size_t pos = 0;
        while (pos < shown) {
            size_t end = pos;
            while (end < shown && text[end] != '\n' && (int)(end - pos) < cols) {
                end++;
            }
            char tmp[512];
            size_t l = end - pos;
            if (l >= sizeof tmp) {
                l = sizeof tmp - 1;
            }
            memcpy(tmp, text + pos, l);
            tmp[l] = '\0';
            DrawTextEx(gui_font(), tmp, (Vector2){box.x + 30, y + (float)row * (gui_cell_height() + 2)}, 22, 0, C_TEXT);
            row++;
            pos = end;
            if (pos < shown && text[pos] == '\n') {
                pos++;
            }
        }
    }
    text_centred(shown < total ? "Enter: show all" : "Enter: begin", GetScreenHeight() - 60, 18, C_DIM);
}

/* ---------- desktop ---------- */

typedef struct Layout {
    Rectangle bar;
    Rectangle term;
    Rectangle map;
    Rectangle np;
} Layout;

static Layout layout(void)
{
    Layout l;
    float W = (float)GetScreenWidth();
    float H = (float)GetScreenHeight();
    float bar_h = 34;
    l.bar = (Rectangle){0, 0, W, bar_h};
    float split = W * 0.58f;
    l.term = (Rectangle){6, bar_h + 6, split - 12, H - bar_h - 12};
    float right_x = split + 6;
    float right_w = W - right_x - 6;
    float map_h = (H - bar_h - 18) * 0.36f;
    l.map = (Rectangle){right_x, bar_h + 6, right_w, map_h};
    l.np = (Rectangle){right_x, bar_h + 12 + map_h, right_w, H - bar_h - 18 - map_h};
    return l;
}

static void draw_topbar(App *a, Rectangle r)
{
    DrawRectangleRec(r, C_EDGE);
    Game *g = &a->g;
    int m = g->mission;
    char left[256];
    if (g->won) {
        snprintf(left, sizeof left, "NULLROUTE   all chapters complete");
    } else {
        snprintf(left, sizeof left, "NULLROUTE   chapter %d/%d: %s", m, g->missions, g->mission_title[m - 1]);
    }
    DrawTextEx(gui_font(), left, (Vector2){r.x + 10, r.y + 7}, 20, 0, C_PROMPT);

    char right[256];
    Session *s = g->top;
    if (s->host == g->rig) {
        snprintf(right, sizeof right, "%s@rig    Esc: menu   F2: switch panel", s->user);
    } else {
        snprintf(right, sizeof right, "%s@%s   trace %d%%   Esc: menu   F2: switch panel",
                 s->user, s->host->id, s->trace);
    }
    Vector2 mr = MeasureTextEx(gui_font(), right, 20, 0);
    DrawTextEx(gui_font(), right, (Vector2){r.x + r.width - mr.x - 10, r.y + 7}, 20, 0,
               s->host == g->rig ? C_TEXT : C_ACCENT);
}

static void update_game(App *a)
{
    Layout l = layout();
    Game *g = &a->g;

    if (key_pressed(KEY_ESCAPE)) {
        a->scr = SCR_PAUSE;
        a->sel = 0;
        return;
    }
    if (key_pressed(KEY_F2)) {
        a->focus = !a->focus;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Vector2 mp = GetMousePosition();
        if (CheckCollisionPointRec(mp, l.np)) {
            a->focus = 1;
        } else if (CheckCollisionPointRec(mp, l.term)) {
            a->focus = 0;
        }
    }

    if (g->notepad.pending) {
        np_open(&a->np, g, g->notepad.host, g->notepad.path);
        g->notepad.pending = 0;
    }
    /* A command asked for an app: show its tab and focus the panel. */
    if (g->app_focus) {
        a->av.active = g->app_focus - 1;
        /* Notepad and mail take the keyboard; cmpview only switches tab, so
         * the terminal keeps typing. */
        if (a->av.active != APP_CMPVIEW) {
            a->focus = 1;
        }
        g->app_focus = 0;
    }

    if (term_update(&a->term, g, a->focus == 0)) {
        autosave(a, 0);
    }
    if (apps_update(g, &a->np, &a->av, l.np, a->focus == 1)) {
        /* Typing in a read-only app goes to the terminal. raylib's character
         * queue only lasts this frame, so the terminal reads it now. */
        a->focus = 0;
        if (term_update(&a->term, g, 1)) {
            autosave(a, 0);
        }
    }

    if (g->ending[0] != '\0') {
        /* The last choice has been made. The ending screen takes over. */
        autosave(a, 1);
        a->scr = SCR_ENDING;
        return;
    }
    autosave(a, 0);
}

static void draw_game(App *a)
{
    Layout l = layout();
    ClearBackground(C_BG);
    draw_topbar(a, l.bar);
    term_draw(&a->term, l.term, a->focus == 0);
    netmap_draw(&a->g, l.map);
    apps_draw(&a->g, &a->np, &a->av, l.np, a->focus == 1);
}

/* ---------- pause ---------- */

static void update_pause(App *a)
{
    if (key_pressed(KEY_ESCAPE)) {
        a->scr = SCR_GAME;
        return;
    }
    int count = 3;
    if (key_pressed(KEY_DOWN)) {
        a->sel = (a->sel + 1) % count;
    }
    if (key_pressed(KEY_UP)) {
        a->sel = (a->sel + count - 1) % count;
    }
    int w = 360;
    int h = 52;
    int x = (GetScreenWidth() - w) / 2;
    int y = GetScreenHeight() / 2 - 40;
    const char *labels[3] = {"Resume", "Save and return to menu", "Quit to desktop"};
    int activate = key_pressed(KEY_ENTER);
    for (int i = 0; i < count; i++) {
        Rectangle r = {(float)x, (float)(y + i * (h + 14)), (float)w, (float)h};
        if (hovered(r)) {
            a->sel = i;
        }
        int pick = button(r, labels[i], a->sel == i, 1);
        if (pick || (activate && a->sel == i)) {
            activate = 0;
            if (i == 0) {
                a->scr = SCR_GAME;
            } else if (i == 1) {
                autosave(a, 1);
                discard_game(a);
                refresh_save_state(a);
                a->scr = SCR_MENU;
                a->sel = 0;
            } else {
                autosave(a, 1);
                a->quit = 1;
            }
            return;
        }
    }
}

static void draw_pause(App *a)
{
    draw_game(a);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){0, 0, 0, 170});
    text_centred("PAUSED", GetScreenHeight() / 2.0f - 110, 48, C_PROMPT);
    int w = 360;
    int h = 52;
    int x = (GetScreenWidth() - w) / 2;
    int y = GetScreenHeight() / 2 - 40;
    const char *labels[3] = {"Resume", "Save and return to menu", "Quit to desktop"};
    for (int i = 0; i < 3; i++) {
        Rectangle r = {(float)x, (float)(y + i * (h + 14)), (float)w, (float)h};
        DrawRectangleRec(r, a->sel == i ? C_EDGE : C_PANEL);
        DrawRectangleLinesEx(r, 2, a->sel == i ? C_ACCENT : C_EDGE);
        Vector2 m = MeasureTextEx(gui_font(), labels[i], 22, 0);
        DrawTextEx(gui_font(), labels[i], (Vector2){r.x + (r.width - m.x) / 2, r.y + (r.height - m.y) / 2},
                   22, 0, a->sel == i ? C_PROMPT : C_TEXT);
    }
}

/* ---------- ending ---------- */

static void draw_ending(App *a)
{
    draw_backdrop();
    const char *key = a->g.ending[0] ? a->g.ending : "publish";
    char name[64];
    snprintf(name, sizeof name, "ending-%s", key);
    const char *text = game_text(&a->g, name);
    text_centred("THE END", GetScreenHeight() / 2.0f - 200, 52, C_PROMPT);
    if (text != NULL) {
        float y = GetScreenHeight() / 2.0f - 120;
        size_t pos = 0;
        size_t total = strlen(text);
        float cw = gui_cell_width();
        int cols = (int)((GetScreenWidth() * 0.7f) / cw);
        while (pos < total) {
            size_t end = pos;
            while (end < total && text[end] != '\n' && (int)(end - pos) < cols) {
                end++;
            }
            char tmp[512];
            size_t l = end - pos;
            if (l >= sizeof tmp) {
                l = sizeof tmp - 1;
            }
            memcpy(tmp, text + pos, l);
            tmp[l] = '\0';
            text_centred(tmp, y, 20, C_TEXT);
            y += gui_cell_height() + 2;
            pos = end + (end < total && text[end] == '\n' ? 1 : 0);
        }
    }
    text_centred("Press Enter to return to the menu", GetScreenHeight() - 60, 20, C_DIM);
}

static void update_ending(App *a)
{
    if (key_pressed(KEY_ENTER)) {
        remove(a->save_path);
        discard_game(a);
        refresh_save_state(a);
        a->scr = SCR_MENU;
        a->sel = 0;
    }
}

/* ---------- main ---------- */

int main(void)
{
    App app;
    memset(&app, 0, sizeof app);
    setup_paths(&app);
    refresh_save_state(&app);
    term_init(&app.term);
    np_init(&app.np);

    InitWindow(1280, 800, "NULLROUTE");
    SetWindowMinSize(960, 600);
    SetExitKey(KEY_NULL); /* Esc is for the pause menu, not for quitting */
    SetTargetFPS(60);
    gui_font_init(app.font_path);

    app.scr = SCR_MENU;
    while (!WindowShouldClose() && !app.quit) {
        input_begin_frame();
        switch (app.scr) {
        case SCR_MENU:
            update_menu(&app);
            break;
        case SCR_INTRO:
            update_intro(&app);
            break;
        case SCR_GAME:
            update_game(&app);
            break;
        case SCR_PAUSE:
            update_pause(&app);
            break;
        case SCR_ENDING:
            update_ending(&app);
            break;
        }

        BeginDrawing();
        switch (app.scr) {
        case SCR_MENU:
            draw_menu(&app);
            break;
        case SCR_INTRO:
            draw_intro(&app);
            break;
        case SCR_GAME:
            draw_game(&app);
            break;
        case SCR_PAUSE:
            draw_pause(&app);
            break;
        case SCR_ENDING:
            draw_ending(&app);
            break;
        }
        EndDrawing();
    }

    autosave(&app, 1);
    discard_game(&app);
    CloseWindow();
    return 0;
}
