/*
 * game.c - game state: sessions, trace, passwords, missions, mail, saving
 * and loading, and the entry points the terminal uses.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr.h"

#define SAVE_HEADER "NULLROUTE-SAVE 2"

/* ---------- hosts ---------- */

Host *host_find(Host *hosts, int n, const char *id)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(hosts[i].id, id) == 0) {
            return &hosts[i];
        }
    }
    return NULL;
}

int host_service_open(const Host *h, const char *name)
{
    for (int i = 0; i < h->nservices; i++) {
        if (strcmp(h->services[i].name, name) == 0) {
            return h->services[i].open;
        }
    }
    return 0;
}

/* ---------- sessions ---------- */

static void session_init_env(Session *s)
{
    char home[NR_PATH_MAX];
    if (s->is_root) {
        snprintf(home, sizeof home, "/root");
    } else {
        snprintf(home, sizeof home, "/home/%s", s->user);
    }
    session_setvar(s, "HOME", home);
    session_setvar(s, "USER", s->user);
    session_setvar(s, "LOGNAME", s->user);
    session_setvar(s, "PATH", "/usr/local/bin:/usr/bin:/bin");
    session_setvar(s, "SHELL", "/bin/sh");
    session_setvar(s, "PWD", s->cwd);
    session_setvar(s, "TERM", "nullroute-256");
}

static Session *session_alloc(Host *h, const char *user, int is_root)
{
    Session *s = calloc(1, sizeof *s);
    if (s == NULL) {
        abort();
    }
    s->host = h;
    snprintf(s->user, sizeof s->user, "%s", user);
    s->is_root = is_root;
    return s;
}

Session *session_new_rig(Game *g)
{
    Session *s = session_alloc(g->rig, NR_PLAYER, 0);
    snprintf(s->cwd, sizeof s->cwd, "/home/%s", NR_PLAYER);
    session_init_env(s);
    session_setvar(s, "isfirstrun", g->first_run ? "1" : "0");
    session_setvar(s, "tutorial", "1");
    return s;
}

Session *session_push(Game *g, Host *h, const char *user, int is_root)
{
    Session *s = session_alloc(h, user, is_root);
    s->parent = g->top;
    if (is_root) {
        snprintf(s->cwd, sizeof s->cwd, "/root");
    } else {
        snprintf(s->cwd, sizeof s->cwd, "/home/%s", user);
    }
    if (vfs_get(h->fs, s->cwd) == NULL || !vfs_get(h->fs, s->cwd)->is_dir) {
        snprintf(s->cwd, sizeof s->cwd, "/");
    }
    session_init_env(s);
    g->top = s;
    return s;
}

/* Popped sessions are not freed: a shell may still hold a pointer to one
 * while it unwinds. They are small and the game is short-lived. */
void session_pop_to_rig(Game *g, Buf *out)
{
    (void)out;
    Session *s = g->top;
    while (s->parent != NULL) {
        s = s->parent;
    }
    g->top = s;
}

/* Trace: remote commands add the host's security rating. */
int game_tick(Game *g, Session *s)
{
    if (s->parent == NULL || s->host == g->rig) {
        return 0;
    }
    s->trace += s->host->security;
    if (s->trace < NR_TRACE_LIMIT) {
        return 0;
    }
    buf_fmt(&g->notices, "\n!!! TRACE COMPLETE: the owner of %s located your rig.\n"
                         "!!! Connection dropped by the remote host.\n",
            s->host->id);
    session_pop_to_rig(g, NULL);
    g->dirty = 1;
    return 1;
}

/* ---------- credentials ---------- */

void game_save_cred(Game *g, const char *host, const char *user, const char *pass)
{
    for (int i = 0; i < g->ncreds; i++) {
        if (strcmp(g->creds[i].host, host) == 0 && strcmp(g->creds[i].user, user) == 0) {
            snprintf(g->creds[i].pass, sizeof g->creds[i].pass, "%s", pass);
            g->dirty = 1;
            return;
        }
    }
    if (g->ncreds >= NR_MAX_CREDS) {
        return;
    }
    Cred *c = &g->creds[g->ncreds++];
    snprintf(c->host, sizeof c->host, "%s", host);
    snprintf(c->user, sizeof c->user, "%s", user);
    snprintf(c->pass, sizeof c->pass, "%s", pass);
    g->dirty = 1;
}

const char *game_find_cred(Game *g, const char *host, const char *user)
{
    for (int i = 0; i < g->ncreds; i++) {
        if (strcmp(g->creds[i].host, host) == 0 && strcmp(g->creds[i].user, user) == 0) {
            return g->creds[i].pass;
        }
    }
    return NULL;
}

/* ---------- passwords and ssh ---------- */

int game_password(Game *g, const char *pw, Buf *out)
{
    if (!g->password_pending) {
        return -1;
    }
    g->password_pending = 0;
    Host *h = g->pw_host;
    const char *user = g->pw_user;
    unsigned int want = 0;
    int found = 0;
    for (int i = 0; i < h->nusers; i++) {
        if (strcmp(h->users[i].name, user) == 0) {
            want = h->users[i].hash;
            found = 1;
        }
    }
    if (found && want == nr_hash(pw)) {
        game_save_cred(g, h->id, user, pw);
        session_push(g, h, user, strcmp(user, "root") == 0);
        buf_fmt(out, "Welcome to %s (%s).\n", h->id, user);
        return 0;
    }
    buf_fmt(out, "Permission denied, please try again.\n");
    return 1;
}

/* ---------- prompt ---------- */

void game_prompt(Game *g, Buf *out)
{
    Session *s = g->top;
    const char *home = session_getvar(s, "HOME");
    const char *shown = s->cwd;
    char tilde[NR_PATH_MAX];
    if (home != NULL && strcmp(s->cwd, home) == 0) {
        shown = "~";
    } else if (home != NULL && strncmp(s->cwd, home, strlen(home)) == 0 && s->cwd[strlen(home)] == '/') {
        snprintf(tilde, sizeof tilde, "~%s", s->cwd + strlen(home));
        shown = tilde;
    }
    buf_fmt(out, "%s@%s:%s%c ", s->user, s->host->id, shown, s->is_root ? '#' : '$');
}

/* ---------- mail and missions ---------- */

static void deliver_mail(Game *g, int n)
{
    char key[NR_NAME_MAX];
    snprintf(key, sizeof key, "mail-%d", n);
    const char *body = game_text(g, key);
    if (body == NULL) {
        return;
    }
    char path[NR_PATH_MAX];
    snprintf(path, sizeof path, "/home/%s/mail/%03d.txt", NR_PLAYER, n);
    vfs_put(g->rig->fs, path, body, strlen(body), 0);
    buf_fmt(&g->notices, "\n[mail] new message in ~/mail (%03d.txt)\n", n);
}

const char *game_text(Game *g, const char *name)
{
    for (int i = 0; i < g->ntexts; i++) {
        if (strcmp(g->texts[i].name, name) == 0) {
            return g->texts[i].body;
        }
    }
    return NULL;
}

static int loot_present(Game *g, const char *file)
{
    char path[NR_PATH_MAX];
    snprintf(path, sizeof path, "/home/%s/loot/%s", NR_PLAYER, file);
    Node *n = vfs_get(g->rig->fs, path);
    return n != NULL && !n->is_dir;
}

/* Completes chapters whose loot is on the rig. Runs after every command. */
void game_check_mission(Game *g, Buf *out)
{
    (void)out;
    while (!g->won && g->mission >= 1 && g->mission <= g->missions) {
        int m = g->mission;
        if (!loot_present(g, g->mission_loot[m - 1])) {
            break;
        }
        char key[NR_NAME_MAX];
        snprintf(key, sizeof key, "complete-%d", m);
        const char *done = game_text(g, key);
        buf_fmt(&g->notices, "\n=== CHAPTER %d COMPLETE: %s ===\n", m, g->mission_title[m - 1]);
        if (done != NULL) {
            buf_str(&g->notices, done);
        }
        g->dirty = 1;
        if (m >= g->missions) {
            g->won = 1;
            buf_str(&g->notices, "\nYour evidence is ready. Decide what to do with it:\n"
                                 "  publish   send it to the press\n"
                                 "  sell      take the chair's offer\n");
            break;
        }
        g->mission = m + 1;
        deliver_mail(g, g->mission);
        snprintf(key, sizeof key, "brief-%d", g->mission);
        const char *brief = game_text(g, key);
        buf_fmt(&g->notices, "\nA new chapter has started (%s). Type 'brief' to reread it.\n",
                g->mission_title[g->mission - 1]);
        if (brief != NULL) {
            buf_str(&g->notices, brief);
        }
    }
}

/* ---------- filesystem helpers for the notepad ---------- */

char *game_fs_read(Host *h, const char *abs, size_t *len)
{
    return vfs_read(h->fs, abs, len);
}

int game_fs_write(Game *g, Host *h, const char *abs, const char *data, size_t len)
{
    int rc = vfs_put(h->fs, abs, data, len, 0);
    if (rc == 0) {
        g->dirty = 1;
    }
    return rc;
}

/* ---------- terminal entry point ---------- */

void game_exec(Game *g, const char *line, Buf *out)
{
    Ctx c;
    memset(&c, 0, sizeof c);
    c.g = g;
    c.s = g->top;
    c.out = out;
    c.err = out;
    c.in = NULL;
    g->dirty = 1;
    shell_run(&c, line);
    if (c.exiting && g->top->parent != NULL) {
        buf_fmt(out, "Connection to %s closed.\n", g->top->host->id);
        g->top = g->top->parent;
    }
    if (g->notices.len > 0) {
        buf_put(out, g->notices.data, g->notices.len);
        buf_free(&g->notices);
    }
    game_check_mission(g, out);
    if (g->notices.len > 0) {
        buf_put(out, g->notices.data, g->notices.len);
        buf_free(&g->notices);
    }
}

/* Runs ~/.profile on the rig. The first time, isfirstrun is 1 and the
 * profile starts the tutorial. */
void game_login(Game *g, Buf *out)
{
    Ctx c;
    memset(&c, 0, sizeof c);
    c.g = g;
    c.s = g->top;
    c.out = out;
    c.err = out;
    session_setvar(c.s, "isfirstrun", g->first_run ? "1" : "0");
    shell_run(&c, "sh /home/ghost/.profile");
    if (g->first_run) {
        g->first_run = 0;
        session_setvar(c.s, "isfirstrun", "0");
        g->dirty = 1;
    }
    if (g->notices.len > 0) {
        buf_put(out, g->notices.data, g->notices.len);
        buf_free(&g->notices);
    }
    game_check_mission(g, out);
    if (g->notices.len > 0) {
        buf_put(out, g->notices.data, g->notices.len);
        buf_free(&g->notices);
    }
}

/* ---------- save and load ---------- */

int game_save(Game *g, const char *save_path)
{
    Buf b;
    buf_init(&b);
    buf_fmt(&b, "%s\n", SAVE_HEADER);
    buf_fmt(&b, "mission %d\n", g->mission);
    buf_fmt(&b, "won %d\n", g->won);
    buf_fmt(&b, "firstrun %d\n", g->first_run);
    buf_fmt(&b, "apps %d %d %d %d %d %d\n", g->app_open[0], g->app_open[1], g->app_open[2],
            g->app_pinned[0], g->app_pinned[1], g->app_pinned[2]);
    buf_fmt(&b, "srv %s %d\n", g->srv_host[0] ? g->srv_host : "-", g->srv_port);
    buf_fmt(&b, "ending %s\n", g->ending);
    for (int i = 0; i < g->nhosts; i++) {
        Host *h = &g->hosts[i];
        buf_fmt(&b, "host %s discovered %d owned %d\n", h->id, h->discovered, h->owned);
    }
    for (int i = 0; i < g->ncreds; i++) {
        buf_fmt(&b, "cred %s %s %s\n", g->creds[i].host, g->creds[i].user, g->creds[i].pass);
    }
    for (int i = 0; i < g->nhosts; i++) {
        Buf fs;
        buf_init(&fs);
        vfs_serialize(&fs, g->hosts[i].fs);
        buf_fmt(&b, "fs %s %lu\n", g->hosts[i].id, (unsigned long)fs.len);
        buf_put(&b, fs.data ? fs.data : "", fs.len);
        buf_char(&b, '\n');
        buf_free(&fs);
    }

    char tmp[NR_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", save_path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        buf_free(&b);
        return -1;
    }
    size_t written = fwrite(b.data ? b.data : "", 1, b.len, f);
    int ok = written == b.len && fclose(f) == 0;
    buf_free(&b);
    if (!ok) {
        remove(tmp);
        return -1;
    }
    remove(save_path);
    if (rename(tmp, save_path) != 0) {
        return -1;
    }
    g->dirty = 0;
    return 0;
}

int game_load(Game *g, const char *world_path, const char *save_path)
{
    char err[256];
    if (world_load(g, world_path, err, sizeof err) != 0) {
        return -1;
    }
    FILE *f = fopen(save_path, "rb");
    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return -1;
    }
    char *data = malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(f);
        return -1;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[got] = '\0';

    size_t pos = 0;
    int ok = 1;
    int header = 0;
    while (ok && pos < got) {
        char *eol = memchr(data + pos, '\n', got - pos);
        if (eol == NULL) {
            break;
        }
        size_t linelen = (size_t)(eol - (data + pos));
        char *line = nr_strndup(data + pos, linelen);
        pos += linelen + 1;

        if (!header) {
            header = 1;
            if (strcmp(line, SAVE_HEADER) != 0) {
                ok = 0;
            }
        } else if (strncmp(line, "mission ", 8) == 0) {
            g->mission = atoi(line + 8);
        } else if (strncmp(line, "won ", 4) == 0) {
            g->won = atoi(line + 4);
        } else if (strncmp(line, "firstrun ", 9) == 0) {
            g->first_run = atoi(line + 9);
        } else if (strncmp(line, "apps ", 5) == 0) {
            int v[6] = {0, 0, 0, 0, 0, 0};
            if (sscanf(line + 5, "%d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                for (int i = 0; i < APP_COUNT; i++) {
                    g->app_open[i] = v[i];
                    g->app_pinned[i] = v[i + 3];
                }
            }
        } else if (strncmp(line, "srv ", 4) == 0) {
            char host[NR_NAME_MAX];
            int port = 0;
            if (sscanf(line + 4, "%63s %d", host, &port) == 2 && strcmp(host, "-") != 0) {
                snprintf(g->srv_host, sizeof g->srv_host, "%s", host);
                g->srv_port = port;
            }
        } else if (strncmp(line, "ending ", 7) == 0) {
            snprintf(g->ending, sizeof g->ending, "%s", line + 7);
        } else if (strncmp(line, "host ", 5) == 0) {
            char id[NR_NAME_MAX];
            int disc = 0;
            int own = 0;
            if (sscanf(line + 5, "%63s discovered %d owned %d", id, &disc, &own) == 3) {
                Host *h = host_find(g->hosts, g->nhosts, id);
                if (h != NULL) {
                    h->discovered = disc;
                    h->owned = own;
                }
            }
        } else if (strncmp(line, "cred ", 5) == 0) {
            char host[NR_NAME_MAX];
            char user[32];
            char pass[64];
            if (sscanf(line + 5, "%63s %31s %63s", host, user, pass) == 3 && g->ncreds < NR_MAX_CREDS) {
                snprintf(g->creds[g->ncreds].host, sizeof g->creds[0].host, "%s", host);
                snprintf(g->creds[g->ncreds].user, sizeof g->creds[0].user, "%s", user);
                snprintf(g->creds[g->ncreds].pass, sizeof g->creds[0].pass, "%s", pass);
                g->ncreds++;
            }
        } else if (strncmp(line, "fs ", 3) == 0) {
            char id[NR_NAME_MAX];
            unsigned long flen = 0;
            if (sscanf(line + 3, "%63s %lu", id, &flen) != 2 || pos + flen > got) {
                ok = 0;
            } else {
                Host *h = host_find(g->hosts, g->nhosts, id);
                if (h == NULL) {
                    ok = 0;
                } else {
                    vfs_destroy(h->fs);
                    h->fs = vfs_new();
                    if (vfs_deserialize(h->fs, data + pos, flen) != 0) {
                        ok = 0;
                    }
                }
                pos += flen + 1;
            }
        }
        free(line);
    }
    free(data);
    if (!ok) {
        return -1;
    }
    /* cmpview is always open, whatever the save says. */
    g->app_open[APP_CMPVIEW] = 1;
    g->app_pinned[APP_CMPVIEW] = 1;
    g->top = session_new_rig(g);
    g->dirty = 0;
    return 0;
}

/* ---------- apps ---------- */

const char *const nr_app_names[APP_COUNT] = {"cmpview", "notepad", "mail"};

void game_apps_reset(Game *g)
{
    memset(g->app_open, 0, sizeof g->app_open);
    memset(g->app_pinned, 0, sizeof g->app_pinned);
    g->app_open[APP_CMPVIEW] = 1;
    g->app_pinned[APP_CMPVIEW] = 1;
    g->app_focus = 0;
    g->srv_host[0] = '\0';
    g->srv_port = 0;
}

int game_app_by_name(const char *name)
{
    for (int i = 0; i < APP_COUNT; i++) {
        if (strcmp(nr_app_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/* What a service says when you connect to it. */
const char *service_banner(const char *service)
{
    if (strcmp(service, "ssh") == 0) {
        return "SSH-2.0-NullOS_4.1";
    }
    if (strcmp(service, "http") == 0) {
        return "HTTP/1.1 200 OK  Server: nullroute-httpd";
    }
    if (strcmp(service, "ftp") == 0) {
        return "220 NullOS FTP ready";
    }
    return "(no banner)";
}

/* cmpview: a summary of whatever the player is looking at right now. It
 * follows the session: the machine you are on, your folder, your trace. */
void game_cmpview(Game *g, Buf *out)
{
    Session *s = g->top;
    Host *h = s->host;
    buf_str(out, "== cmpview ==\n");
    buf_fmt(out, "session  %s@%s  %s\n", s->user, h->id, s->cwd);
    buf_fmt(out, "machine  %s (%s)  security %d\n", h->name, h->ip, h->security);
    if (h == g->rig) {
        buf_str(out, "status   your rig\n");
    } else {
        buf_fmt(out, "status   %s  trace %d%%\n", h->owned ? "ROOTED" : "remote", s->trace);
    }
    buf_str(out, "services\n");
    if (h->nservices == 0) {
        buf_str(out, "  none\n");
    }
    for (int i = 0; i < h->nservices; i++) {
        buf_fmt(out, "  %-6s %5d  %s\n", h->services[i].name, h->services[i].port,
                h->services[i].open ? "open" : "closed");
    }
    if (g->srv_port > 0) {
        Host *sv = host_find(g->hosts, g->nhosts, g->srv_host);
        const char *svc = "?";
        int open = 0;
        if (sv != NULL) {
            for (int i = 0; i < sv->nservices; i++) {
                if (sv->services[i].port == g->srv_port) {
                    svc = sv->services[i].name;
                    open = sv->services[i].open;
                }
            }
        }
        buf_fmt(out, "server   %s:%d %s (%s)\n", g->srv_host, g->srv_port, svc, open ? "open" : "closed");
        buf_fmt(out, "banner   %s\n", service_banner(svc));
    }
    buf_fmt(out, "chapter  %d/%d %s\n", g->mission, g->missions, g->mission_title[g->mission - 1]);

    /* Context-aware hints: what the machine in front of you suggests next. */
    if (h == g->rig && !(h->discovered)) {
        int any = 0;
        for (int i = 0; i < g->nhosts; i++) {
            any |= g->hosts[i].discovered;
        }
        if (!any) {
            buf_str(out, "tip      scan finds the hosts this rig can reach.\n");
        }
    }
    if (h != g->rig && !h->owned && h->vuln[0] != '\0') {
        buf_fmt(out, "tip      %s may be exploitable: exploit %s %s\n", h->id, h->id, h->vuln);
    }
    if (h != g->rig && !h->owned && h->nusers > 0 && game_find_cred(g, h->id, h->users[0].name) == NULL) {
        buf_fmt(out, "tip      you have no password for %s yet: crack %s %s\n", h->id, h->id, h->users[0].name);
    }
}

/* ---------- lifecycle ---------- */

int game_new(Game *g, const char *world_path)
{
    char err[256];
    if (world_load(g, world_path, err, sizeof err) != 0) {
        buf_fmt(&g->notices, "%s\n", err);
        return -1;
    }
    g->mission = 1;
    g->won = 0;
    g->first_run = 1;
    game_apps_reset(g);
    g->ending[0] = '\0';
    g->top = session_new_rig(g);
    deliver_mail(g, 1);
    g->dirty = 1;
    return 0;
}

void game_destroy(Game *g)
{
    for (int i = 0; i < g->nhosts; i++) {
        vfs_destroy(g->hosts[i].fs);
        g->hosts[i].fs = NULL;
    }
    for (int i = 0; i < g->ntexts; i++) {
        free(g->texts[i].body);
    }
    for (int i = 0; i < g->nwords; i++) {
        free(g->words[i]);
    }
    buf_free(&g->notices);
    g->ntexts = 0;
    g->nwords = 0;
}
