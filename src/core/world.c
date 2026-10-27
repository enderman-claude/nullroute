/*
 * world.c - loads the hand-written world (hosts, file systems, story text,
 * missions and the wordlist) from assets/world.txt.
 *
 * Format, line by line:
 *   # comment                         (outside blocks only)
 *   @host ID                          start a host; the keys below apply to it
 *     name: Display Name
 *     ip: 10.0.0.1
 *     pos: 0.30 0.50                  map position, 0..1
 *     unlock: 1                       chapter in which the host appears
 *     security: 2                     trace added per command
 *     links: a, b                     hosts reachable from here
 *     service: ssh 22 open
 *     vuln: ssh                       service with a working exploit
 *     user: NAME PASSWORD             an account and its password
 *   @file HOST /absolute/path         file content follows, ended by @end
 *   @dir HOST /absolute/path          an empty directory
 *   @text NAME                        story text, ended by @end
 *   @mission N|Title|loot-file        one chapter
 *   @wordlist                         one word per line, ended by @end
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr.h"

extern const Cmd nr_tools[];

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) {
        s[--l] = '\0';
    }
    return s;
}

static int set_error(char *err, size_t errlen, int line, const char *msg)
{
    snprintf(err, errlen, "world.txt line %d: %s", line, msg);
    return -1;
}

/* Reads one line into buf. Returns 0 at end of file. */
static int read_line(FILE *f, char *buf, size_t n)
{
    if (fgets(buf, (int)n, f) == NULL) {
        return 0;
    }
    return 1;
}

static Host *current_host(Game *g, const char *id)
{
    return host_find(g->hosts, g->nhosts, id);
}

static void add_text(Game *g, const char *name, const Buf *body)
{
    if (g->ntexts >= NR_MAX_TEXTS) {
        return;
    }
    Text *t = &g->texts[g->ntexts++];
    snprintf(t->name, sizeof t->name, "%s", name);
    t->body = buf_steal((Buf *)body);
}

/* Creates the default layout and installs the coreutils stubs in /bin. */
static void install_system(Host *h, int with_tools, const Cmd *tools)
{
    vfs_mkdir_p(h->fs, "/bin");
    vfs_mkdir_p(h->fs, "/usr/bin");
    vfs_mkdir_p(h->fs, "/tmp");
    vfs_mkdir_p(h->fs, "/etc");
    vfs_mkdir_p(h->fs, "/var/log");
    vfs_mkdir_p(h->fs, "/home");
    vfs_mkdir_p(h->fs, "/root");
    for (int i = 0; nr_coreutils[i].name != NULL; i++) {
        char path[NR_PATH_MAX];
        char body[NR_NAME_MAX + 32];
        snprintf(path, sizeof path, "/bin/%s", nr_coreutils[i].name);
        snprintf(body, sizeof body, "NULLROUTE-BINARY %s\n", nr_coreutils[i].name);
        vfs_put(h->fs, path, body, strlen(body), 0);
    }
    if (with_tools) {
        for (int i = 0; tools[i].name != NULL; i++) {
            char path[NR_PATH_MAX];
            char body[NR_NAME_MAX + 32];
            snprintf(path, sizeof path, "/usr/bin/%s", tools[i].name);
            snprintf(body, sizeof body, "NULLROUTE-BINARY %s\n", tools[i].name);
            vfs_put(h->fs, path, body, strlen(body), 0);
        }
    }
}

int world_load(Game *g, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }

    char line[2048];
    int lineno = 0;
    Host *cur = NULL;
    int in_block = 0;       /* 0 none, 1 file, 2 text, 3 wordlist */
    char block_path[NR_PATH_MAX];
    char block_host[NR_NAME_MAX];
    char block_name[NR_NAME_MAX];
    Buf body;
    buf_init(&body);
    int rc = 0;

    memset(g, 0, sizeof *g);
    g->missions = 0;

    while (read_line(f, line, sizeof line)) {
        lineno++;
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) {
            line[--ll] = '\0';
        }

        if (in_block) {
            if (strcmp(line, "@end") == 0) {
                if (in_block == 1) {
                    Host *h = current_host(g, block_host);
                    if (h == NULL) {
                        rc = set_error(err, errlen, lineno, "@file for an unknown host");
                        break;
                    }
                    vfs_mkdir_p(h->fs, "/");
                    char dir[NR_PATH_MAX];
                    snprintf(dir, sizeof dir, "%s", block_path);
                    char *slash = strrchr(dir, '/');
                    if (slash != NULL && slash != dir) {
                        *slash = '\0';
                        vfs_mkdir_p(h->fs, dir);
                    }
                    vfs_put(h->fs, block_path, body.data ? body.data : "", body.len, 0);
                } else if (in_block == 2) {
                    add_text(g, block_name, &body);
                    body.data = NULL;
                    body.len = body.cap = 0;
                } else if (in_block == 3) {
                    /* wordlist entries were stored as lines in body */
                    size_t st = 0;
                    for (size_t k = 0; k <= body.len; k++) {
                        if (k == body.len || body.data[k] == '\n') {
                            if (k > st && g->nwords < NR_MAX_WORDS) {
                                g->words[g->nwords++] = nr_strndup(body.data + st, k - st);
                            }
                            st = k + 1;
                        }
                    }
                }
                buf_free(&body);
                in_block = 0;
                continue;
            }
            if (in_block == 3) {
                buf_str(&body, line);
                buf_char(&body, '\n');
            } else {
                buf_str(&body, line);
                buf_char(&body, '\n');
            }
            continue;
        }

        char *t = trim(line);
        if (t[0] == '\0' || t[0] == '#') {
            continue;
        }

        if (t[0] == '@') {
            if (strncmp(t, "@host ", 6) == 0) {
                char *id = trim(t + 6);
                if (g->nhosts >= NR_MAX_HOSTS) {
                    rc = set_error(err, errlen, lineno, "too many hosts");
                    break;
                }
                cur = &g->hosts[g->nhosts++];
                memset(cur, 0, sizeof *cur);
                snprintf(cur->id, sizeof cur->id, "%s", id);
                snprintf(cur->name, sizeof cur->name, "%s", id);
                cur->unlock = 1;
                cur->fs = vfs_new();
            } else if (strncmp(t, "@user ", 6) == 0) {
                if (cur == NULL || cur->nusers >= NR_MAX_USERS) {
                    rc = set_error(err, errlen, lineno, "@user outside a host");
                    break;
                }
                char name[32];
                char pass[64];
                if (sscanf(t + 6, "%31s %63s", name, pass) != 2) {
                    rc = set_error(err, errlen, lineno, "@user needs NAME PASSWORD");
                    break;
                }
                snprintf(cur->users[cur->nusers].name, sizeof cur->users[0].name, "%s", name);
                cur->users[cur->nusers].hash = nr_hash(pass);
                cur->nusers++;
            } else if (strncmp(t, "@file ", 6) == 0) {
                if (sscanf(t + 6, "%63s %511[^\n]", block_host, block_path) != 2) {
                    rc = set_error(err, errlen, lineno, "@file needs HOST PATH");
                    break;
                }
                in_block = 1;
                buf_init(&body);
            } else if (strncmp(t, "@dir ", 5) == 0) {
                char host_id[NR_NAME_MAX];
                char dpath[NR_PATH_MAX];
                if (sscanf(t + 5, "%63s %511[^\n]", host_id, dpath) != 2) {
                    rc = set_error(err, errlen, lineno, "@dir needs HOST PATH");
                    break;
                }
                Host *h = current_host(g, host_id);
                if (h == NULL) {
                    rc = set_error(err, errlen, lineno, "@dir for an unknown host");
                    break;
                }
                vfs_mkdir_p(h->fs, dpath);
            } else if (strncmp(t, "@text ", 6) == 0) {
                snprintf(block_name, sizeof block_name, "%s", trim(t + 6));
                in_block = 2;
                buf_init(&body);
            } else if (strncmp(t, "@mission ", 9) == 0) {
                char *p = t + 9;
                char *bar1 = strchr(p, '|');
                if (bar1 == NULL) {
                    rc = set_error(err, errlen, lineno, "@mission needs N|Title|loot");
                    break;
                }
                char *bar2 = strchr(bar1 + 1, '|');
                if (bar2 == NULL) {
                    rc = set_error(err, errlen, lineno, "@mission needs N|Title|loot");
                    break;
                }
                int n = atoi(p);
                if (n < 1 || n > NR_MAX_MISSIONS) {
                    rc = set_error(err, errlen, lineno, "mission number out of range");
                    break;
                }
                *bar1 = '\0';
                *bar2 = '\0';
                snprintf(g->mission_title[n - 1], sizeof g->mission_title[0], "%s", trim(bar1 + 1));
                snprintf(g->mission_loot[n - 1], sizeof g->mission_loot[0], "%s", trim(bar2 + 1));
                if (n > g->missions) {
                    g->missions = n;
                }
            } else if (strcmp(t, "@wordlist") == 0) {
                in_block = 3;
                buf_init(&body);
            } else {
                rc = set_error(err, errlen, lineno, "unknown directive");
                break;
            }
            continue;
        }

        /* key: value lines belong to the current host */
        char *colon = strchr(t, ':');
        if (cur == NULL || colon == NULL) {
            rc = set_error(err, errlen, lineno, "expected 'key: value' inside a host");
            break;
        }
        *colon = '\0';
        char *key = trim(t);
        char *val = trim(colon + 1);
        if (strcmp(key, "name") == 0) {
            snprintf(cur->name, sizeof cur->name, "%s", val);
        } else if (strcmp(key, "ip") == 0) {
            snprintf(cur->ip, sizeof cur->ip, "%s", val);
        } else if (strcmp(key, "pos") == 0) {
            if (sscanf(val, "%f %f", &cur->x, &cur->y) != 2) {
                rc = set_error(err, errlen, lineno, "pos needs two numbers");
                break;
            }
        } else if (strcmp(key, "unlock") == 0) {
            cur->unlock = atoi(val);
        } else if (strcmp(key, "security") == 0) {
            cur->security = atoi(val);
        } else if (strcmp(key, "links") == 0) {
            char copy[512];
            snprintf(copy, sizeof copy, "%s", val);
            char *save = NULL;
            char *tok = strtok(copy, ",");
            (void)save;
            while (tok != NULL && cur->nlinks < NR_MAX_LINKS) {
                snprintf(cur->links[cur->nlinks++], NR_NAME_MAX, "%s", trim(tok));
                tok = strtok(NULL, ",");
            }
        } else if (strcmp(key, "service") == 0) {
            char sname[16];
            int port = 0;
            char state[16];
            if (sscanf(val, "%15s %d %15s", sname, &port, state) != 3 || cur->nservices >= NR_MAX_SERVICES) {
                rc = set_error(err, errlen, lineno, "service needs NAME PORT open|closed");
                break;
            }
            Service *sv = &cur->services[cur->nservices++];
            snprintf(sv->name, sizeof sv->name, "%s", sname);
            sv->port = port;
            sv->open = strcmp(state, "open") == 0;
        } else if (strcmp(key, "vuln") == 0) {
            snprintf(cur->vuln, sizeof cur->vuln, "%s", val);
        } else {
            rc = set_error(err, errlen, lineno, "unknown key");
            break;
        }
    }
    fclose(f);
    buf_free(&body);
    if (rc != 0) {
        return rc;
    }
    if (in_block != 0) {
        snprintf(err, errlen, "world.txt: unterminated block (missing @end)");
        return -1;
    }

    g->rig = current_host(g, "rig");
    if (g->rig == NULL) {
        snprintf(err, errlen, "world.txt: no host named 'rig'");
        return -1;
    }

    /* Each host gets the coreutils in /bin and a home directory per account. */
    for (int i = 0; i < g->nhosts; i++) {
        Host *h = &g->hosts[i];
        install_system(h, h == g->rig, nr_tools);
        for (int u = 0; u < h->nusers; u++) {
            char home[NR_PATH_MAX];
            if (strcmp(h->users[u].name, "root") == 0) {
                snprintf(home, sizeof home, "/root");
            } else {
                snprintf(home, sizeof home, "/home/%s", h->users[u].name);
            }
            vfs_mkdir_p(h->fs, home);
        }
    }
    vfs_mkdir_p(g->rig->fs, "/home/ghost/loot");
    vfs_mkdir_p(g->rig->fs, "/home/ghost/mail");
    return 0;
}
