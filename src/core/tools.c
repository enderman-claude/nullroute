/*
 * tools.c - the hacking toolkit. These are game programs, not coreutils:
 * they live in /usr/bin on the rig and are carried along to every shell the
 * player opens.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr.h"

static int known(Game *g, Host *h)
{
    return h == g->rig || h->discovered;
}

static Host *lookup(Ctx *c, const char *id, const char *tool)
{
    Host *h = host_find(c->g->hosts, c->g->nhosts, id);
    if (h == NULL) {
        buf_fmt(c->err, "%s: %s: unknown host\n", tool, id);
        return NULL;
    }
    if (!known(c->g, h)) {
        buf_fmt(c->err, "%s: %s: host not found in your scan results\n", tool, id);
        return NULL;
    }
    return h;
}

static int tool_scan(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    Host *here = c->s->host;
    if (here->nlinks == 0) {
        buf_fmt(c->out, "scan: no outbound links from %s\n", here->id);
        return 0;
    }
    buf_fmt(c->out, "Scanning links from %s ...\n", here->id);
    int found = 0;
    for (int i = 0; i < here->nlinks; i++) {
        Host *h = host_find(c->g->hosts, c->g->nhosts, here->links[i]);
        if (h == NULL || h->unlock > c->g->mission) {
            continue;
        }
        if (!h->discovered) {
            h->discovered = 1;
            c->g->dirty = 1;
        }
        found++;
        buf_fmt(c->out, "  %-14s %-16s %s\n", h->id, h->ip, h->name);
    }
    if (found == 0) {
        buf_str(c->out, "  nothing new.\n");
    }
    return 0;
}

static int tool_portscan(Ctx *c, int argc, char **argv)
{
    if (argc != 2) {
        buf_str(c->err, "usage: portscan HOST\n");
        return 2;
    }
    Host *h = lookup(c, argv[1], "portscan");
    if (h == NULL) {
        return 1;
    }
    buf_fmt(c->out, "Services on %s (%s):\n", h->id, h->ip);
    if (h->nservices == 0) {
        buf_str(c->out, "  no open ports\n");
    }
    for (int i = 0; i < h->nservices; i++) {
        buf_fmt(c->out, "  %5d/tcp  %-8s %s\n", h->services[i].port, h->services[i].name,
                h->services[i].open ? "open" : "closed");
    }
    return 0;
}

static int tool_crack(Ctx *c, int argc, char **argv)
{
    if (argc != 3) {
        buf_str(c->err, "usage: crack HOST USER\n");
        return 2;
    }
    Host *h = lookup(c, argv[1], "crack");
    if (h == NULL) {
        return 1;
    }
    if (!host_service_open(h, "ssh")) {
        buf_fmt(c->err, "crack: ssh is closed on %s\n", h->id);
        return 1;
    }
    unsigned int want = 0;
    int found = 0;
    for (int i = 0; i < h->nusers; i++) {
        if (strcmp(h->users[i].name, argv[2]) == 0) {
            want = h->users[i].hash;
            found = 1;
        }
    }
    if (!found) {
        buf_fmt(c->err, "crack: no account '%s' on %s\n", argv[2], h->id);
        return 1;
    }
    buf_fmt(c->out, "[*] Dictionary attack on %s@%s (%d words)\n", argv[2], h->id, c->g->nwords);
    for (int i = 0; i < c->g->nwords; i++) {
        buf_fmt(c->out, "    try %3d  %s\n", i + 1, c->g->words[i]);
        if (nr_hash(c->g->words[i]) == want) {
            buf_fmt(c->out, "[+] Password found after %d attempts: %s\n", i + 1, c->g->words[i]);
            game_save_cred(c->g, h->id, argv[2], c->g->words[i]);
            return 0;
        }
    }
    buf_str(c->out, "[-] Wordlist exhausted. Try something else.\n");
    return 1;
}

/* ssh [user@]host: uses a saved password if there is one, otherwise asks. */
static int tool_ssh(Ctx *c, int argc, char **argv)
{
    if (argc != 2 || strchr(argv[1], '@') == NULL) {
        buf_str(c->err, "usage: ssh USER@HOST\n");
        return 2;
    }
    char user[32];
    const char *at = strchr(argv[1], '@');
    snprintf(user, sizeof user, "%.*s", (int)(at - argv[1]), argv[1]);
    Host *h = lookup(c, at + 1, "ssh");
    if (h == NULL) {
        return 1;
    }
    if (!host_service_open(h, "ssh")) {
        buf_fmt(c->err, "ssh: connect to %s port 22: Connection refused\n", h->id);
        return 255;
    }
    int has_user = 0;
    for (int i = 0; i < h->nusers; i++) {
        if (strcmp(h->users[i].name, user) == 0) {
            has_user = 1;
        }
    }
    if (!has_user) {
        buf_fmt(c->err, "%s@%s: Permission denied\n", user, h->id);
        return 255;
    }
    const char *saved = game_find_cred(c->g, h->id, user);
    unsigned int want = 0;
    for (int i = 0; i < h->nusers; i++) {
        if (strcmp(h->users[i].name, user) == 0) {
            want = h->users[i].hash;
        }
    }
    if (saved != NULL && nr_hash(saved) == want) {
        session_push(c->g, h, user, strcmp(user, "root") == 0);
        buf_fmt(c->out, "Welcome to %s (%s).\n", h->id, user);
        c->abort = 1; /* the rest of this line runs on the new host, so stop here */
        return 0;
    }
    c->g->password_pending = 1;
    c->g->pw_host = h;
    snprintf(c->g->pw_user, sizeof c->g->pw_user, "%s", user);
    buf_fmt(c->out, "%s@%s's password: ", user, h->id);
    c->abort = 1;
    return 0;
}

/* exploit HOST SERVICE: a working exploit gives a root shell. */
static int tool_exploit(Ctx *c, int argc, char **argv)
{
    if (argc != 3) {
        buf_str(c->err, "usage: exploit HOST SERVICE\n");
        return 2;
    }
    Host *h = lookup(c, argv[1], "exploit");
    if (h == NULL) {
        return 1;
    }
    buf_fmt(c->out, "[*] Probing %s on %s ...\n", argv[2], h->id);
    if (!host_service_open(h, argv[2])) {
        buf_fmt(c->out, "[-] %s is closed or filtered on %s.\n", argv[2], h->id);
        return 1;
    }
    if (strcmp(h->vuln, argv[2]) != 0) {
        buf_fmt(c->out, "[-] No working exploit for %s on %s.\n", argv[2], h->id);
        return 1;
    }
    buf_str(c->out, "[*] Injecting payload ...\n");
    h->owned = 1;
    c->g->dirty = 1;
    session_push(c->g, h, "root", 1);
    buf_fmt(c->out, "[+] Root shell on %s.\n", h->id);
    c->abort = 1;
    return 0;
}

/* scp HOST:PATH DEST: copies a file from a host into the rig's home. */
static int tool_scp(Ctx *c, int argc, char **argv)
{
    if (argc != 3 || strchr(argv[1], ':') == NULL) {
        buf_str(c->err, "usage: scp HOST:PATH DEST\n");
        return 2;
    }
    /* Copies always land on the rig. Run scp from the rig, not from a remote
     * shell, so that ~ means the rig's home. */
    if (c->s->host != c->g->rig) {
        buf_str(c->err, "scp: run scp from your rig (type 'exit' until you are back on it)\n");
        return 1;
    }
    char hostid[NR_NAME_MAX];
    const char *colon = strchr(argv[1], ':');
    snprintf(hostid, sizeof hostid, "%.*s", (int)(colon - argv[1]), argv[1]);
    const char *remote = colon + 1;
    Host *h = lookup(c, hostid, "scp");
    if (h == NULL) {
        return 1;
    }
    /* Access follows what the player has: root after an exploit, otherwise a
     * saved account on that host. */
    const char *user = NULL;
    int is_root = 0;
    if (h->owned) {
        user = "root";
        is_root = 1;
    } else {
        for (int i = 0; i < c->g->ncreds; i++) {
            if (strcmp(c->g->creds[i].host, h->id) == 0) {
                user = c->g->creds[i].user;
                break;
            }
        }
    }
    if (user == NULL) {
        buf_fmt(c->err, "scp: %s: Permission denied (no credentials)\n", hostid);
        return 1;
    }
    char abs[NR_PATH_MAX];
    if (path_join("/", remote, abs, sizeof abs) != 0) {
        return 1;
    }
    if (!is_root && strncmp(abs, "/root", 5) == 0) {
        buf_fmt(c->err, "scp: %s: Permission denied\n", remote);
        return 1;
    }
    Node *n = vfs_get(h->fs, abs);
    if (n == NULL || n->is_dir) {
        buf_fmt(c->err, "scp: %s: No such file\n", remote);
        return 1;
    }
    size_t len = 0;
    char *data = vfs_read(h->fs, abs, &len);
    char dst[NR_PATH_MAX];
    char home[NR_PATH_MAX];
    snprintf(home, sizeof home, "/home/%s", NR_PLAYER);
    if (path_join(home, argv[2], dst, sizeof dst) != 0) {
        free(data);
        return 1;
    }
    Node *dn = vfs_get(c->g->rig->fs, dst);
    char final_path[NR_PATH_MAX * 2];
    if (dn != NULL && dn->is_dir) {
        const char *base = strrchr(abs, '/');
        snprintf(final_path, sizeof final_path, "%s/%s", dst, base ? base + 1 : abs);
    } else {
        snprintf(final_path, sizeof final_path, "%s", dst);
    }
    if (vfs_put(c->g->rig->fs, final_path, data ? data : "", len, 0) != 0) {
        buf_fmt(c->err, "scp: cannot write %s\n", argv[2]);
        free(data);
        return 1;
    }
    free(data);
    c->g->dirty = 1;
    buf_fmt(c->out, "%s  100%%  %lu bytes\n", remote, (unsigned long)len);
    return 0;
}

static int tool_notepad(Ctx *c, int argc, char **argv)
{
    if (argc != 2) {
        buf_str(c->err, "usage: notepad FILE\n");
        return 2;
    }
    char abs[NR_PATH_MAX];
    if (path_join(c->s->cwd, argv[1], abs, sizeof abs) != 0) {
        return 1;
    }
    Node *n = vfs_get(c->s->host->fs, abs);
    if (n != NULL && n->is_dir) {
        buf_fmt(c->err, "notepad: %s is a directory\n", argv[1]);
        return 1;
    }
    c->g->notepad.pending = 1;
    c->g->notepad.host = c->s->host;
    snprintf(c->g->notepad.path, sizeof c->g->notepad.path, "%s", abs);
    c->g->app_open[APP_NOTEPAD] = 1;
    c->g->app_focus = APP_NOTEPAD + 1;
    buf_fmt(c->out, "notepad: opening %s\n", abs);
    return 0;
}

static int tool_brief(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    Game *g = c->g;
    if (g->won) {
        buf_str(c->out, "All chapters complete. Type 'publish' or 'sell'.\n");
        return 0;
    }
    char key[NR_NAME_MAX];
    snprintf(key, sizeof key, "brief-%d", g->mission);
    buf_fmt(c->out, "Chapter %d: %s\n", g->mission, g->mission_title[g->mission - 1]);
    const char *body = game_text(g, key);
    if (body != NULL) {
        buf_str(c->out, body);
    }
    return 0;
}

static int tool_status(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    Game *g = c->g;
    int nd = 0;
    int no = 0;
    for (int i = 0; i < g->nhosts; i++) {
        nd += g->hosts[i].discovered;
        no += g->hosts[i].owned;
    }
    buf_fmt(c->out, "Chapter:     %d of %d - %s\n", g->mission, g->missions, g->mission_title[g->mission - 1]);
    buf_fmt(c->out, "Discovered:  %d hosts\n", nd);
    buf_fmt(c->out, "Rooted:      %d hosts\n", no);
    buf_fmt(c->out, "Passwords:   %d saved\n", g->ncreds);
    buf_fmt(c->out, "Session:     %s@%s (trace %d%%)\n", c->s->user, c->s->host->id, c->s->trace);
    return 0;
}

static int tool_ending(Ctx *c, const char *which)
{
    Game *g = c->g;
    if (!g->won) {
        buf_fmt(c->err, "Nothing to %s yet. Keep digging.\n", which);
        return 1;
    }
    char key[NR_NAME_MAX];
    snprintf(key, sizeof key, "ending-%s", which);
    const char *body = game_text(g, key);
    if (body != NULL) {
        buf_str(c->out, body);
    }
    snprintf(g->ending, sizeof g->ending, "%s", which);
    g->dirty = 1;
    return 0;
}

static int tool_publish(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return tool_ending(c, "publish");
}

static int tool_sell(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return tool_ending(c, "sell");
}

static int tool_help(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    buf_str(c->out, "Coreutils (GNU-style):\n  ");
    int col = 2;
    for (int i = 0; nr_coreutils[i].name != NULL; i++) {
        int l = (int)strlen(nr_coreutils[i].name);
        if (col + l + 2 > 76) {
            buf_str(c->out, "\n  ");
            col = 2;
        }
        buf_fmt(c->out, "%s  ", nr_coreutils[i].name);
        col += l + 2;
    }
    buf_str(c->out, "\n\nHacking toolkit:\n");
    buf_str(c->out, "  scan                 find hosts linked from this machine\n"
                    "  portscan HOST        list services on a discovered host\n"
                    "  crack HOST USER      dictionary attack on an account\n"
                    "  ssh USER@HOST        log in (asks for the password)\n"
                    "  exploit HOST SVC     root shell through a vulnerable service\n"
                    "  scp HOST:PATH DEST   copy a file from a host to your rig\n"
                    "  notepad FILE         open FILE in the notepad window\n"
                    "  brief                reread the current chapter brief\n"
                    "  status               progress and session\n"
                    "  publish | sell       final choice, after the last chapter\n"
                    "Shell builtins: cd export unset exit sh : (and & not supported)\n");
    return 0;
}


/* ---------- apps ---------- */

static int tool_apps(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int i = 0; i < APP_COUNT; i++) {
        const char *state = c->g->app_open[i] ? "open" : "closed";
        const char *pin = c->g->app_pinned[i] ? (i == APP_CMPVIEW ? ", always open" : ", pinned") : "";
        buf_fmt(c->out, "  %-8s %s%s\n", nr_app_names[i], state, pin);
    }
    return 0;
}

static int app_arg(Ctx *c, int argc, char **argv, const char *usage)
{
    if (argc != 2) {
        buf_fmt(c->err, "usage: %s\n", usage);
        return -1;
    }
    int id = game_app_by_name(argv[1]);
    if (id < 0) {
        buf_fmt(c->err, "%s: unknown app '%s' (try: cmpview notepad mail)\n", argv[0], argv[1]);
        return -1;
    }
    return id;
}

static int tool_pin(Ctx *c, int argc, char **argv)
{
    int id = app_arg(c, argc, argv, "pin APP");
    if (id < 0) {
        return 2;
    }
    c->g->app_open[id] = 1;
    c->g->app_pinned[id] = 1;
    c->g->dirty = 1;
    buf_fmt(c->out, "pinned %s: it stays open.\n", nr_app_names[id]);
    return 0;
}

static int tool_unpin(Ctx *c, int argc, char **argv)
{
    int id = app_arg(c, argc, argv, "unpin APP");
    if (id < 0) {
        return 2;
    }
    if (id == APP_CMPVIEW) {
        buf_str(c->err, "cmpview is always open and cannot be unpinned.\n");
        return 1;
    }
    c->g->app_pinned[id] = 0;
    c->g->dirty = 1;
    buf_fmt(c->out, "unpinned %s: close it with 'close %s' when you are done.\n", nr_app_names[id], nr_app_names[id]);
    return 0;
}

static int tool_close(Ctx *c, int argc, char **argv)
{
    int id = app_arg(c, argc, argv, "close APP");
    if (id < 0) {
        return 2;
    }
    if (id == APP_CMPVIEW) {
        buf_str(c->err, "cmpview is always open and cannot be closed.\n");
        return 1;
    }
    if (c->g->app_pinned[id]) {
        buf_fmt(c->err, "%s is pinned. Run 'unpin %s' first.\n", nr_app_names[id], nr_app_names[id]);
        return 1;
    }
    c->g->app_open[id] = 0;
    c->g->dirty = 1;
    buf_fmt(c->out, "closed %s.\n", nr_app_names[id]);
    return 0;
}

static int tool_mail(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    c->g->app_open[APP_MAIL] = 1;
    c->g->app_focus = APP_MAIL + 1;
    c->g->dirty = 1;
    buf_str(c->out, "mail: opening your inbox.\n");
    return 0;
}

static int tool_cmpview(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    c->g->app_focus = APP_CMPVIEW + 1;
    return 0;
}

/* showserver [IP|HOST@]PORT: point cmpview at a service. Without an address
 * the service is looked up on the machine you are on. */
static int tool_showserver(Ctx *c, int argc, char **argv)
{
    if (argc != 2) {
        buf_str(c->err, "usage: showserver [IP|HOST@]PORT\n");
        return 2;
    }
    const char *arg = argv[1];
    const char *at = strchr(arg, '@');
    const char *portstr = at ? at + 1 : arg;
    char *end = NULL;
    long port = strtol(portstr, &end, 10);
    if (end == portstr || *end != '\0' || port <= 0 || port > 65535) {
        buf_fmt(c->err, "showserver: bad port '%s'\n", portstr);
        return 2;
    }
    Host *h = c->s->host;
    if (at != NULL) {
        char who[NR_NAME_MAX];
        snprintf(who, sizeof who, "%.*s", (int)(at - arg), arg);
        h = NULL;
        for (int i = 0; i < c->g->nhosts; i++) {
            Host *cand = &c->g->hosts[i];
            if (strcmp(cand->id, who) == 0 || strcmp(cand->ip, who) == 0) {
                h = cand;
            }
        }
        if (h == NULL) {
            buf_fmt(c->err, "showserver: unknown host '%s'\n", who);
            return 1;
        }
    }
    if (h != c->g->rig && !h->discovered) {
        buf_fmt(c->err, "showserver: %s is not in your scan results\n", h->id);
        return 1;
    }
    const char *svc = NULL;
    for (int i = 0; i < h->nservices; i++) {
        if (h->services[i].port == port) {
            svc = h->services[i].name;
        }
    }
    if (svc == NULL) {
        buf_fmt(c->err, "showserver: no service on port %ld at %s\n", port, h->id);
        return 1;
    }
    snprintf(c->g->srv_host, sizeof c->g->srv_host, "%s", h->id);
    c->g->srv_port = (int)port;
    c->g->app_focus = APP_CMPVIEW + 1;
    c->g->dirty = 1;
    buf_fmt(c->out, "cmpview: showing %s:%ld (%s)\n", h->id, port, svc);
    return 0;
}

const Cmd nr_tools[] = {
    {"scan", tool_scan},
    {"portscan", tool_portscan},
    {"crack", tool_crack},
    {"ssh", tool_ssh},
    {"scp", tool_scp},
    {"exploit", tool_exploit},
    {"notepad", tool_notepad},
    {"brief", tool_brief},
    {"status", tool_status},
    {"publish", tool_publish},
    {"sell", tool_sell},
    {"help", tool_help},
    {"apps", tool_apps},
    {"pin", tool_pin},
    {"unpin", tool_unpin},
    {"close", tool_close},
    {"mail", tool_mail},
    {"cmpview", tool_cmpview},
    {"showserver", tool_showserver},
    {NULL, NULL}
};

CmdFn nr_find_tool(const char *name)
{
    for (int i = 0; nr_tools[i].name != NULL; i++) {
        if (strcmp(nr_tools[i].name, name) == 0) {
            return nr_tools[i].fn;
        }
    }
    return NULL;
}
