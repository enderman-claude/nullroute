/*
 * coreutils.c - the GNU coreutils subset the virtual machines ship with.
 *
 * Each command gets the usual argv and a Ctx with stdin (c->in), stdout
 * (c->out) and stderr (c->err). Nothing here touches the real machine: every
 * path goes through the host's virtual filesystem.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr.h"

/* ---------- shared helpers ---------- */

static Node *root_of(Ctx *c)
{
    return c->s->host->fs;
}

static int resolve(Ctx *c, const char *arg, char *out)
{
    return path_join(c->s->cwd, arg, out, NR_PATH_MAX);
}

static void fail(Ctx *c, const char *cmd, const char *arg, const char *msg)
{
    if (arg != NULL) {
        buf_fmt(c->err, "%s: %s: %s\n", cmd, arg, msg);
    } else {
        buf_fmt(c->err, "%s: %s\n", cmd, msg);
    }
}

/* Protected parts of the rig: deleting these would break the game's saves. */
static int rig_protected(Ctx *c, const char *abs)
{
    if (c->s->host != c->g->rig) {
        return 0;
    }
    static const char *const keep[] = {"/home", "/home/ghost", "/home/ghost/loot", "/home/ghost/mail", NULL};
    for (int i = 0; keep[i] != NULL; i++) {
        if (strcmp(abs, keep[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Writing under /etc and /root needs root. Reading /root does too. */
static int denied(Ctx *c, const char *abs, int write)
{
    int system_dir = strncmp(abs, "/etc", 4) == 0 && (abs[4] == '\0' || abs[4] == '/');
    int root_dir = strncmp(abs, "/root", 5) == 0 && (abs[5] == '\0' || abs[5] == '/');
    if ((system_dir && write) || root_dir) {
        return !c->s->is_root;
    }
    return 0;
}

static char *read_all_stdin(Ctx *c, size_t *len)
{
    if (c->in == NULL) {
        *len = 0;
        return nr_strdup("");
    }
    *len = c->in->len;
    return nr_strndup(c->in->data ? c->in->data : "", c->in->len);
}

/* Reads a file argument, or stdin for NULL and "-". Returns a malloc'd copy. */
static char *read_input(Ctx *c, const char *cmd, const char *arg, size_t *len)
{
    if (arg == NULL || strcmp(arg, "-") == 0) {
        return read_all_stdin(c, len);
    }
    char abs[NR_PATH_MAX];
    if (resolve(c, arg, abs) != 0) {
        fail(c, cmd, arg, "No such file or directory");
        return NULL;
    }
    Node *n = vfs_get(root_of(c), abs);
    if (n == NULL) {
        fail(c, cmd, arg, "No such file or directory");
        return NULL;
    }
    if (n->is_dir) {
        fail(c, cmd, arg, "Is a directory");
        return NULL;
    }
    if (denied(c, abs, 0)) {
        fail(c, cmd, arg, "Permission denied");
        return NULL;
    }
    return vfs_read(root_of(c), abs, len);
}

static const char *base_name(const char *p)
{
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

/* Parses a count like "10" for head/tail. Returns -1 on error. */
static long parse_count(const char *s)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0) {
        return -1;
    }
    return v;
}

/* ---------- output and text ---------- */

static int cmd_echo(Ctx *c, int argc, char **argv)
{
    int i = 1;
    int newline = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        i = 2;
    }
    for (; i < argc; i++) {
        if (i > (newline ? 1 : 2)) {
            buf_char(c->out, ' ');
        }
        buf_str(c->out, argv[i]);
    }
    if (newline) {
        buf_char(c->out, '\n');
    }
    return 0;
}

static int cmd_printf(Ctx *c, int argc, char **argv)
{
    if (argc < 2) {
        fail(c, "printf", NULL, "missing format");
        return 1;
    }
    const char *fmt = argv[1];
    int ai = 2;
    do {
        for (const char *p = fmt; *p != '\0'; p++) {
            if (*p == '\\' && p[1] != '\0') {
                p++;
                switch (*p) {
                case 'n': buf_char(c->out, '\n'); break;
                case 't': buf_char(c->out, '\t'); break;
                case '\\': buf_char(c->out, '\\'); break;
                default: buf_char(c->out, '\\'); buf_char(c->out, *p); break;
                }
            } else if (*p == '%' && p[1] != '\0') {
                p++;
                if (*p == '%') {
                    buf_char(c->out, '%');
                } else if (*p == 's') {
                    buf_str(c->out, ai < argc ? argv[ai] : "");
                    ai++;
                } else if (*p == 'd' || *p == 'i') {
                    buf_fmt(c->out, "%ld", ai < argc ? strtol(argv[ai], NULL, 10) : 0L);
                    ai++;
                } else {
                    buf_char(c->out, '%');
                    buf_char(c->out, *p);
                }
            } else {
                buf_char(c->out, *p);
            }
        }
    } while (ai < argc && strstr(fmt, "%") != NULL);
    return 0;
}

static int cmd_cat(Ctx *c, int argc, char **argv)
{
    if (argc == 1) {
        size_t len = 0;
        char *d = read_all_stdin(c, &len);
        buf_put(c->out, d, len);
        free(d);
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        size_t len = 0;
        char *d = read_input(c, "cat", argv[i], &len);
        if (d == NULL) {
            rc = 1;
            continue;
        }
        buf_put(c->out, d, len);
        free(d);
    }
    return rc;
}

static int cmd_head_tail(Ctx *c, int argc, char **argv, int is_tail)
{
    const char *cmd = is_tail ? "tail" : "head";
    long count = 10;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = parse_count(argv[i + 1]);
            i += 2;
        } else if (argv[i][1] >= '0' && argv[i][1] <= '9') {
            count = parse_count(argv[i] + 1);
            i++;
        } else {
            fail(c, cmd, argv[i], "invalid option");
            return 1;
        }
        if (count < 0) {
            fail(c, cmd, "invalid number of lines", "");
            return 1;
        }
    }
    size_t len = 0;
    char *d = read_input(c, cmd, i < argc ? argv[i] : NULL, &len);
    if (d == NULL) {
        return 1;
    }
    /* Find the line boundaries. */
    size_t total = 0;
    for (size_t k = 0; k < len; k++) {
        if (d[k] == '\n') {
            total++;
        }
    }
    if (len > 0 && d[len - 1] != '\n') {
        total++;
    }
    long skip_lines = is_tail ? (long)total - count : 0;
    if (skip_lines < 0) {
        skip_lines = 0;
    }
    long want_lines = is_tail ? (long)total - skip_lines : count;
    size_t pos = 0;
    long line = 0;
    /* Advance to the first line we want. */
    while (pos < len && line < skip_lines) {
        if (d[pos] == '\n') {
            line++;
        }
        pos++;
    }
    long emitted = 0;
    while (pos < len && emitted < want_lines) {
        size_t start = pos;
        while (pos < len && d[pos] != '\n') {
            pos++;
        }
        if (pos < len) {
            pos++;
        }
        buf_put(c->out, d + start, pos - start);
        if (d[pos - 1] != '\n') {
            buf_char(c->out, '\n');
        }
        emitted++;
    }
    free(d);
    return 0;
}

static int cmd_head(Ctx *c, int argc, char **argv)
{
    return cmd_head_tail(c, argc, argv, 0);
}

static int cmd_tail(Ctx *c, int argc, char **argv)
{
    return cmd_head_tail(c, argc, argv, 1);
}

static int cmd_wc(Ctx *c, int argc, char **argv)
{
    int show_l = 0;
    int show_w = 0;
    int show_c = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        for (const char *p = argv[i] + 1; *p != '\0'; p++) {
            if (*p == 'l') {
                show_l = 1;
            } else if (*p == 'w') {
                show_w = 1;
            } else if (*p == 'c') {
                show_c = 1;
            } else {
                fail(c, "wc", argv[i], "invalid option");
                return 1;
            }
        }
        i++;
    }
    if (!show_l && !show_w && !show_c) {
        show_l = show_w = show_c = 1;
    }
    const char *file = i < argc ? argv[i] : NULL;
    size_t len = 0;
    char *d = read_input(c, "wc", file, &len);
    if (d == NULL) {
        return 1;
    }
    size_t lines = 0;
    size_t words = 0;
    int in_word = 0;
    for (size_t k = 0; k < len; k++) {
        if (d[k] == '\n') {
            lines++;
        }
        if (d[k] == ' ' || d[k] == '\t' || d[k] == '\n') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    free(d);
    if (show_l) {
        buf_fmt(c->out, "%7lu", (unsigned long)lines);
    }
    if (show_w) {
        buf_fmt(c->out, "%8lu", (unsigned long)words);
    }
    if (show_c) {
        buf_fmt(c->out, "%8lu", (unsigned long)len);
    }
    if (file != NULL) {
        buf_fmt(c->out, " %s", file);
    }
    buf_char(c->out, '\n');
    return 0;
}

/* sort: -r reverses, -n compares numerically, -u drops duplicates. */
static int sort_reverse;
static int sort_numeric;

static int sort_cmp(const void *a, const void *b)
{
    const char *x = *(const char *const *)a;
    const char *y = *(const char *const *)b;
    int r;
    if (sort_numeric) {
        long xv = strtol(x, NULL, 10);
        long yv = strtol(y, NULL, 10);
        r = xv < yv ? -1 : (xv > yv ? 1 : 0);
        if (r == 0) {
            r = strcmp(x, y);
        }
    } else {
        r = strcmp(x, y);
    }
    return sort_reverse ? -r : r;
}

static int cmd_sort(Ctx *c, int argc, char **argv)
{
    int unique = 0;
    sort_reverse = 0;
    sort_numeric = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        for (const char *p = argv[i] + 1; *p != '\0'; p++) {
            if (*p == 'r') {
                sort_reverse = 1;
            } else if (*p == 'n') {
                sort_numeric = 1;
            } else if (*p == 'u') {
                unique = 1;
            } else {
                fail(c, "sort", argv[i], "invalid option");
                return 2;
            }
        }
    }
    size_t len = 0;
    char *d = read_input(c, "sort", i < argc ? argv[i] : NULL, &len);
    if (d == NULL) {
        return 1;
    }
    char **lines = NULL;
    size_t n = 0;
    size_t start = 0;
    for (size_t k = 0; k <= len; k++) {
        if (k == len || d[k] == '\n') {
            if (k == len && start == len) {
                break;
            }
            d[k] = '\0';
            lines = realloc(lines, sizeof *lines * (n + 1));
            if (lines == NULL) {
                abort();
            }
            lines[n++] = d + start;
            start = k + 1;
        }
    }
    qsort(lines, n, sizeof *lines, sort_cmp);
    for (size_t k = 0; k < n; k++) {
        if (unique && k > 0 && strcmp(lines[k], lines[k - 1]) == 0) {
            continue;
        }
        buf_str(c->out, lines[k]);
        buf_char(c->out, '\n');
    }
    free(lines);
    free(d);
    return 0;
}

static int cmd_uniq(Ctx *c, int argc, char **argv)
{
    int count = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-c") == 0) {
        count = 1;
        i++;
    }
    size_t len = 0;
    char *d = read_input(c, "uniq", i < argc ? argv[i] : NULL, &len);
    if (d == NULL) {
        return 1;
    }
    char *prev = NULL;
    int reps = 0;
    size_t start = 0;
    for (size_t k = 0; k <= len; k++) {
        if (k == len || d[k] == '\n') {
            if (k == len && start == len) {
                break;
            }
            char *line = nr_strndup(d + start, k - start);
            start = k + 1;
            if (prev != NULL && strcmp(prev, line) == 0) {
                reps++;
                free(line);
                continue;
            }
            if (prev != NULL) {
                if (count) {
                    buf_fmt(c->out, "%7d %s\n", reps, prev);
                } else {
                    buf_fmt(c->out, "%s\n", prev);
                }
                free(prev);
            }
            prev = line;
            reps = 1;
        }
    }
    if (prev != NULL) {
        if (count) {
            buf_fmt(c->out, "%7d %s\n", reps, prev);
        } else {
            buf_fmt(c->out, "%s\n", prev);
        }
        free(prev);
    }
    free(d);
    return 0;
}

/* Parses a cut field list such as "1", "1,3" or "2-4" (1-based) into a bit
 * mask. Returns 0 on success and -1 on a malformed list. */
static int parse_fields(const char *list, unsigned long long *mask)
{
    char *copy = nr_strdup(list);
    char *tok = copy;
    int rc = 0;
    while (*tok != '\0') {
        char *end = tok;
        long lo = strtol(tok, &end, 10);
        long hi = lo;
        if (*end == '-') {
            hi = strtol(end + 1, &end, 10);
        }
        if (lo < 1 || hi < lo || hi > 64) {
            rc = -1;
            break;
        }
        for (long f = lo; f <= hi; f++) {
            *mask |= 1ULL << (f - 1);
        }
        tok = end;
        if (*tok == ',') {
            tok++;
        } else if (*tok != '\0') {
            rc = -1;
            break;
        }
    }
    free(copy);
    return rc;
}

/* cut -d DELIM -f LIST: LIST is "N", "N,M" or "N-M" (1-based). */
static int cmd_cut(Ctx *c, int argc, char **argv)
{
    char delim = '\t';
    unsigned long long mask = 0;
    int have_list = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[i + 1][0];
            i += 2;
        } else if (strncmp(argv[i], "-d", 2) == 0) {
            delim = argv[i][2];
            i++;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            have_list = 1;
            if (parse_fields(argv[i + 1], &mask) != 0) {
                fail(c, "cut", argv[i + 1], "invalid field list");
                return 1;
            }
            i += 2;
        } else if (strncmp(argv[i], "-f", 2) == 0) {
            have_list = 1;
            if (parse_fields(argv[i] + 2, &mask) != 0) {
                fail(c, "cut", argv[i] + 2, "invalid field list");
                return 1;
            }
            i++;
        } else {
            fail(c, "cut", argv[i], "invalid option");
            return 1;
        }
    }
    if (!have_list) {
        fail(c, "cut", NULL, "you must specify a list of fields");
        return 1;
    }
    size_t len = 0;
    char *d = read_input(c, "cut", i < argc ? argv[i] : NULL, &len);
    if (d == NULL) {
        return 1;
    }
    size_t start = 0;
    for (size_t k = 0; k <= len; k++) {
        if (k == len || d[k] == '\n') {
            if (k == len && start == len) {
                break;
            }
            char *line = nr_strndup(d + start, k - start);
            start = k + 1;
            if (strchr(line, delim) == NULL) {
                buf_fmt(c->out, "%s\n", line);
                free(line);
                continue;
            }
            int field = 1;
            char *p = line;
            int first = 1;
            for (;;) {
                char *stop = strchr(p, delim);
                size_t fl = stop ? (size_t)(stop - p) : strlen(p);
                if (field <= 64 && (mask & (1ULL << (field - 1)))) {
                    if (!first) {
                        buf_char(c->out, delim);
                    }
                    buf_put(c->out, p, fl);
                    first = 0;
                }
                if (stop == NULL) {
                    break;
                }
                p = stop + 1;
                field++;
            }
            buf_char(c->out, '\n');
            free(line);
        }
    }
    free(d);
    return 0;
}

/* Expands tr ranges such as a-z into a plain character list. */
static char *tr_set(const char *spec)
{
    Buf b;
    buf_init(&b);
    for (const char *p = spec; *p != '\0'; p++) {
        char ch = *p;
        if (ch == '\\' && p[1] != '\0') {
            p++;
            ch = *p == 'n' ? '\n' : (*p == 't' ? '\t' : *p);
            buf_char(&b, ch);
            continue;
        }
        if (p[1] == '-' && p[2] != '\0') {
            for (int x = (unsigned char)ch; x <= (unsigned char)p[2]; x++) {
                buf_char(&b, (char)x);
            }
            p += 2;
            continue;
        }
        buf_char(&b, ch);
    }
    return buf_steal(&b);
}

static int cmd_tr(Ctx *c, int argc, char **argv)
{
    int del = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-d") == 0) {
        del = 1;
        i++;
    }
    if (i >= argc || (!del && i + 1 >= argc)) {
        fail(c, "tr", NULL, "missing operand");
        return 1;
    }
    char *set1 = tr_set(argv[i]);
    char *set2 = del ? NULL : tr_set(argv[i + 1]);
    size_t len = 0;
    char *d = read_all_stdin(c, &len);
    for (size_t k = 0; k < len; k++) {
        const char *hit = strchr(set1, d[k]);
        if (d[k] == '\0' || hit == NULL) {
            buf_char(c->out, d[k]);
            continue;
        }
        if (del) {
            continue;
        }
        size_t idx = (size_t)(hit - set1);
        size_t l2 = strlen(set2);
        if (l2 == 0) {
            continue;
        }
        buf_char(c->out, set2[idx < l2 ? idx : l2 - 1]);
    }
    free(set1);
    free(set2);
    free(d);
    return 0;
}

static int cmd_tee(Ctx *c, int argc, char **argv)
{
    int append = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-a") == 0) {
        append = 1;
        i++;
    }
    size_t len = 0;
    char *d = read_all_stdin(c, &len);
    buf_put(c->out, d, len);
    int rc = 0;
    for (; i < argc; i++) {
        char abs[NR_PATH_MAX];
        if (resolve(c, argv[i], abs) != 0 || vfs_put(root_of(c), abs, d, len, append) != 0) {
            fail(c, "tee", argv[i], "cannot create file");
            rc = 1;
        }
    }
    free(d);
    return rc;
}

/* ---------- files and directories ---------- */

static int cmd_pwd(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    buf_fmt(c->out, "%s\n", c->s->cwd);
    return 0;
}

static int cmd_ls(Ctx *c, int argc, char **argv)
{
    int all = 0;
    int longfmt = 0;
    int onepercol = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        for (const char *p = argv[i] + 1; *p != '\0'; p++) {
            if (*p == 'a') {
                all = 1;
            } else if (*p == 'l') {
                longfmt = 1;
            } else if (*p == '1') {
                onepercol = 1;
            } else {
                fail(c, "ls", argv[i], "invalid option");
                return 2;
            }
        }
    }
    int nargs = argc - i;
    static const char *const dot_only[] = {".", NULL};
    const char *const *targets = nargs == 0 ? dot_only : (const char *const *)(argv + i);
    int total = nargs == 0 ? 1 : nargs;
    int rc = 0;
    for (int k = 0; k < total; k++) {
        const char *arg = targets[k];
        char abs[NR_PATH_MAX];
        if (resolve(c, arg, abs) != 0) {
            fail(c, "ls", arg, "No such file or directory");
            rc = 2;
            continue;
        }
        Node *n = vfs_get(root_of(c), abs);
        if (n == NULL) {
            fail(c, "ls", arg, "No such file or directory");
            rc = 2;
            continue;
        }
        if (denied(c, abs, 0)) {
            fail(c, "ls", arg, "Permission denied");
            rc = 2;
            continue;
        }
        if (total > 1 && n->is_dir) {
            buf_fmt(c->out, "%s%s:\n", k ? "\n" : "", arg);
        }
        if (!n->is_dir) {
            buf_fmt(c->out, "%s\n", arg);
            continue;
        }
        size_t count = 0;
        for (Node *ch = n->child; ch != NULL; ch = ch->next) {
            count++;
        }
        const char **names = calloc(count + 2, sizeof *names);
        Node **nodes = calloc(count + 2, sizeof *nodes);
        size_t m = 0;
        if (all) {
            names[m] = ".";
            nodes[m++] = n;
            names[m] = "..";
            nodes[m++] = n->parent ? n->parent : n;
        }
        for (Node *ch = n->child; ch != NULL; ch = ch->next) {
            if (!all && ch->name[0] == '.') {
                continue;
            }
            names[m] = ch->name;
            nodes[m++] = ch;
        }
        /* Insertion sort by name keeps the output stable and simple. */
        for (size_t a = 1; a < m; a++) {
            for (size_t b = a; b > 0 && strcmp(names[b - 1], names[b]) > 0; b--) {
                const char *tn = names[b - 1];
                names[b - 1] = names[b];
                names[b] = tn;
                Node *tv = nodes[b - 1];
                nodes[b - 1] = nodes[b];
                nodes[b] = tv;
            }
        }
        for (size_t x = 0; x < m; x++) {
            if (longfmt) {
                char perm[11] = "----------";
                perm[0] = nodes[x]->is_dir ? 'd' : '-';
                for (int bit = 0; bit < 9; bit++) {
                    if (nodes[x]->mode & (1 << (8 - bit))) {
                        perm[bit + 1] = "rwxrwxrwx"[bit];
                    }
                }
                buf_fmt(c->out, "%s 1 %s %s %6lu %s\n", perm,
                        c->s->is_root ? "root" : c->s->user, c->s->is_root ? "root" : c->s->user,
                        (unsigned long)nodes[x]->len, names[x]);
            } else if (onepercol) {
                buf_fmt(c->out, "%s\n", names[x]);
            } else {
                buf_fmt(c->out, "%s%s", x ? "  " : "", names[x]);
            }
        }
        if (!longfmt && !onepercol && m > 0) {
            buf_char(c->out, '\n');
        }
        free(names);
        free(nodes);
    }
    return rc;
}

static int cmd_mkdir(Ctx *c, int argc, char **argv)
{
    int parents = 0;
    int rc = 0;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-p") == 0) {
        parents = 1;
        i++;
    }
    if (i >= argc) {
        fail(c, "mkdir", NULL, "missing operand");
        return 1;
    }
    for (; i < argc; i++) {
        char abs[NR_PATH_MAX];
        if (resolve(c, argv[i], abs) != 0 || denied(c, abs, 1)) {
            fail(c, "mkdir", argv[i], "Permission denied");
            rc = 1;
            continue;
        }
        if (parents) {
            if (vfs_mkdir_p(root_of(c), abs) != 0) {
                fail(c, "mkdir", argv[i], "cannot create directory");
                rc = 1;
            }
        } else if (vfs_get(root_of(c), abs) != NULL) {
            fail(c, "mkdir", argv[i], "File exists");
            rc = 1;
        } else if (vfs_mkdir(root_of(c), abs) != 0) {
            fail(c, "mkdir", argv[i], "No such file or directory");
            rc = 1;
        }
    }
    return rc;
}

static int cmd_rmdir(Ctx *c, int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char abs[NR_PATH_MAX];
        if (resolve(c, argv[i], abs) != 0 || rig_protected(c, abs) || denied(c, abs, 1)) {
            fail(c, "rmdir", argv[i], "Permission denied");
            rc = 1;
            continue;
        }
        Node *n = vfs_get(root_of(c), abs);
        if (n == NULL) {
            fail(c, "rmdir", argv[i], "No such file or directory");
            rc = 1;
        } else if (!n->is_dir) {
            fail(c, "rmdir", argv[i], "Not a directory");
            rc = 1;
        } else if (vfs_remove(root_of(c), abs, 0) != 0) {
            fail(c, "rmdir", argv[i], "Directory not empty");
            rc = 1;
        }
    }
    return rc;
}

static int cmd_rm(Ctx *c, int argc, char **argv)
{
    int recursive = 0;
    int force = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        for (const char *p = argv[i] + 1; *p != '\0'; p++) {
            if (*p == 'r' || *p == 'R') {
                recursive = 1;
            } else if (*p == 'f') {
                force = 1;
            } else {
                fail(c, "rm", argv[i], "invalid option");
                return 1;
            }
        }
    }
    if (i >= argc) {
        if (force) {
            return 0;
        }
        fail(c, "rm", NULL, "missing operand");
        return 1;
    }
    int rc = 0;
    for (; i < argc; i++) {
        char abs[NR_PATH_MAX];
        if (resolve(c, argv[i], abs) != 0) {
            fail(c, "rm", argv[i], "No such file or directory");
            rc = 1;
            continue;
        }
        Node *n = vfs_get(root_of(c), abs);
        if (n == NULL) {
            if (!force) {
                fail(c, "rm", argv[i], "No such file or directory");
                rc = 1;
            }
            continue;
        }
        if (strcmp(abs, "/") == 0) {
            fail(c, "rm", argv[i], "refusing to remove '/'");
            rc = 1;
            continue;
        }
        if (rig_protected(c, abs) || denied(c, abs, 1)) {
            fail(c, "rm", argv[i], "Permission denied");
            rc = 1;
            continue;
        }
        if (n->is_dir && !recursive) {
            fail(c, "rm", argv[i], "is a directory");
            rc = 1;
            continue;
        }
        if (vfs_remove(root_of(c), abs, recursive) != 0) {
            fail(c, "rm", argv[i], "cannot remove");
            rc = 1;
        }
    }
    return rc;
}

static int cmd_touch(Ctx *c, int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char abs[NR_PATH_MAX];
        if (resolve(c, argv[i], abs) != 0 || denied(c, abs, 1)) {
            fail(c, "touch", argv[i], "Permission denied");
            rc = 1;
            continue;
        }
        if (vfs_get(root_of(c), abs) == NULL && vfs_put(root_of(c), abs, "", 0, 0) != 0) {
            fail(c, "touch", argv[i], "No such file or directory");
            rc = 1;
        }
    }
    return rc;
}

static int is_dir_path(Ctx *c, const char *abs)
{
    Node *n = vfs_get(root_of(c), abs);
    return n != NULL && n->is_dir;
}

static int cmd_cp_mv(Ctx *c, int argc, char **argv, int move)
{
    const char *cmd = move ? "mv" : "cp";
    int recursive = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        for (const char *p = argv[i] + 1; *p != '\0'; p++) {
            if (*p == 'r' || *p == 'R') {
                recursive = 1;
            } else if (move) {
                fail(c, cmd, argv[i], "invalid option");
                return 1;
            } else {
                fail(c, cmd, argv[i], "invalid option");
                return 1;
            }
        }
        i++;
    }
    if (argc - i < 2) {
        fail(c, cmd, NULL, "missing file operand");
        return 1;
    }
    char dst[NR_PATH_MAX];
    if (resolve(c, argv[argc - 1], dst) != 0) {
        fail(c, cmd, argv[argc - 1], "bad destination");
        return 1;
    }
    int dst_is_dir = is_dir_path(c, dst);
    if (argc - i > 2 && !dst_is_dir) {
        fail(c, cmd, argv[argc - 1], "target is not a directory");
        return 1;
    }
    int rc = 0;
    for (; i < argc - 1; i++) {
        char src[NR_PATH_MAX];
        if (resolve(c, argv[i], src) != 0 || vfs_get(root_of(c), src) == NULL) {
            fail(c, cmd, argv[i], "No such file or directory");
            rc = 1;
            continue;
        }
        if (rig_protected(c, src) || denied(c, src, move) || denied(c, dst, 1)) {
            fail(c, cmd, argv[i], "Permission denied");
            rc = 1;
            continue;
        }
        char target[NR_PATH_MAX];
        if (dst_is_dir) {
            if (snprintf(target, sizeof target, "%s/%s", strcmp(dst, "/") == 0 ? "" : dst, base_name(src)) >= (int)sizeof target) {
                rc = 1;
                continue;
            }
        } else {
            snprintf(target, sizeof target, "%s", dst);
        }
        int r;
        if (move) {
            r = vfs_move(root_of(c), src, target);
        } else {
            r = vfs_copy(root_of(c), src, target, recursive);
        }
        if (r == -2) {
            fail(c, cmd, argv[i], "-r not specified; omitting directory");
            rc = 1;
        } else if (r != 0) {
            fail(c, cmd, argv[i], "cannot complete the operation");
            rc = 1;
        }
    }
    return rc;
}

static int cmd_cp(Ctx *c, int argc, char **argv)
{
    return cmd_cp_mv(c, argc, argv, 0);
}

static int cmd_mv(Ctx *c, int argc, char **argv)
{
    return cmd_cp_mv(c, argc, argv, 1);
}

static int cmd_chmod(Ctx *c, int argc, char **argv)
{
    if (argc < 3) {
        fail(c, "chmod", NULL, "missing operand");
        return 1;
    }
    char *end = NULL;
    long mode = strtol(argv[1], &end, 8);
    if (end == argv[1] || *end != '\0' || mode < 0 || mode > 0777) {
        fail(c, "chmod", argv[1], "invalid mode");
        return 1;
    }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        char abs[NR_PATH_MAX];
        Node *n = NULL;
        if (resolve(c, argv[i], abs) == 0) {
            n = vfs_get(root_of(c), abs);
        }
        if (n == NULL || denied(c, abs, 1)) {
            fail(c, "chmod", argv[i], "cannot access");
            rc = 1;
            continue;
        }
        n->mode = (int)mode;
    }
    return rc;
}

static int cmd_basename(Ctx *c, int argc, char **argv)
{
    if (argc < 2) {
        fail(c, "basename", NULL, "missing operand");
        return 1;
    }
    char *name = nr_strdup(argv[1]);
    size_t l = strlen(name);
    while (l > 1 && name[l - 1] == '/') {
        name[--l] = '\0';
    }
    const char *b = base_name(name);
    if (argc > 2 && strlen(b) > strlen(argv[2]) &&
        strcmp(b + strlen(b) - strlen(argv[2]), argv[2]) == 0) {
        buf_fmt(c->out, "%.*s\n", (int)(strlen(b) - strlen(argv[2])), b);
    } else {
        buf_fmt(c->out, "%s\n", b);
    }
    free(name);
    return 0;
}

static int cmd_dirname(Ctx *c, int argc, char **argv)
{
    if (argc < 2) {
        fail(c, "dirname", NULL, "missing operand");
        return 1;
    }
    const char *p = argv[1];
    const char *slash = strrchr(p, '/');
    if (slash == NULL) {
        buf_str(c->out, ".\n");
    } else if (slash == p) {
        buf_str(c->out, "/\n");
    } else {
        buf_fmt(c->out, "%.*s\n", (int)(slash - p), p);
    }
    return 0;
}

static int cmd_seq(Ctx *c, int argc, char **argv)
{
    long first = 1;
    long step = 1;
    long last;
    if (argc == 2) {
        last = strtol(argv[1], NULL, 10);
    } else if (argc == 3) {
        first = strtol(argv[1], NULL, 10);
        last = strtol(argv[2], NULL, 10);
    } else if (argc == 4) {
        first = strtol(argv[1], NULL, 10);
        step = strtol(argv[2], NULL, 10);
        last = strtol(argv[3], NULL, 10);
    } else {
        fail(c, "seq", NULL, "missing operand");
        return 1;
    }
    if (step == 0) {
        fail(c, "seq", NULL, "increment must not be zero");
        return 1;
    }
    for (long v = first; step > 0 ? v <= last : v >= last; v += step) {
        buf_fmt(c->out, "%ld\n", v);
    }
    return 0;
}

/* ---------- system information ---------- */

static int cmd_true(Ctx *c, int argc, char **argv)
{
    (void)c;
    (void)argc;
    (void)argv;
    return 0;
}

static int cmd_false(Ctx *c, int argc, char **argv)
{
    (void)c;
    (void)argc;
    (void)argv;
    return 1;
}

static int cmd_sleep(Ctx *c, int argc, char **argv)
{
    (void)argv;
    if (argc < 2) {
        fail(c, "sleep", NULL, "missing operand");
        return 1;
    }
    /* Time passes instantly inside the game. */
    return 0;
}

static int cmd_date(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    buf_str(c->out, "Thu Oct 14 06:20:00 UTC 2041\n");
    return 0;
}

static int cmd_whoami(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    buf_fmt(c->out, "%s\n", c->s->is_root ? "root" : c->s->user);
    return 0;
}

static int cmd_id(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (c->s->is_root) {
        buf_str(c->out, "uid=0(root) gid=0(root) groups=0(root)\n");
    } else {
        buf_fmt(c->out, "uid=1000(%s) gid=1000(%s) groups=1000(%s)\n", c->s->user, c->s->user, c->s->user);
    }
    return 0;
}

static int cmd_hostname(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    buf_fmt(c->out, "%s\n", c->s->host->id);
    return 0;
}

static int cmd_uname(Ctx *c, int argc, char **argv)
{
    int all = argc > 1 && strcmp(argv[1], "-a") == 0;
    int node = argc > 1 && strcmp(argv[1], "-n") == 0;
    if (all) {
        buf_fmt(c->out, "NullOS %s %s 4.1-nr #1 SMP x86_64 NullOS\n", c->s->host->id, NR_VERSION);
    } else if (node) {
        buf_fmt(c->out, "%s\n", c->s->host->id);
    } else {
        buf_str(c->out, "NullOS\n");
    }
    return 0;
}

static int cmd_env(Ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (Var *v = c->s->vars; v != NULL; v = v->next) {
        buf_fmt(c->out, "%s=%s\n", v->name, v->value);
    }
    return 0;
}

static int cmd_printenv(Ctx *c, int argc, char **argv)
{
    if (argc == 1) {
        return cmd_env(c, argc, argv);
    }
    int rc = 1;
    for (int i = 1; i < argc; i++) {
        const char *v = session_getvar(c->s, argv[i]);
        if (v != NULL) {
            buf_fmt(c->out, "%s\n", v);
            rc = 0;
        }
    }
    return rc;
}

/* test and [: a practical subset (-e -f -d -z -n, = != -eq -ne -lt -gt -le -ge, !). */
static int test_eval(Ctx *c, int argc, char **argv, int *err)
{
    if (argc == 0) {
        return 1;
    }
    if (strcmp(argv[0], "!") == 0) {
        return !test_eval(c, argc - 1, argv + 1, err);
    }
    if (argc == 1) {
        return argv[0][0] != '\0';
    }
    if (argc == 2) {
        const char *op = argv[0];
        const char *arg = argv[1];
        if (strcmp(op, "-z") == 0) {
            return arg[0] == '\0';
        }
        if (strcmp(op, "-n") == 0) {
            return arg[0] != '\0';
        }
        if (strcmp(op, "-e") == 0 || strcmp(op, "-f") == 0 || strcmp(op, "-d") == 0) {
            char abs[NR_PATH_MAX];
            Node *n = NULL;
            if (resolve(c, arg, abs) == 0) {
                n = vfs_get(root_of(c), abs);
            }
            if (strcmp(op, "-e") == 0) {
                return n != NULL;
            }
            if (strcmp(op, "-f") == 0) {
                return n != NULL && !n->is_dir;
            }
            return n != NULL && n->is_dir;
        }
        *err = 1;
        return 0;
    }
    if (argc == 3) {
        const char *a = argv[0];
        const char *op = argv[1];
        const char *b = argv[2];
        if (strcmp(op, "=") == 0) {
            return strcmp(a, b) == 0;
        }
        if (strcmp(op, "!=") == 0) {
            return strcmp(a, b) != 0;
        }
        if (strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 || strcmp(op, "-lt") == 0 ||
            strcmp(op, "-gt") == 0 || strcmp(op, "-le") == 0 || strcmp(op, "-ge") == 0) {
            long x = strtol(a, NULL, 10);
            long y = strtol(b, NULL, 10);
            if (strcmp(op, "-eq") == 0) {
                return x == y;
            }
            if (strcmp(op, "-ne") == 0) {
                return x != y;
            }
            if (strcmp(op, "-lt") == 0) {
                return x < y;
            }
            if (strcmp(op, "-gt") == 0) {
                return x > y;
            }
            if (strcmp(op, "-le") == 0) {
                return x <= y;
            }
            return x >= y;
        }
    }
    *err = 1;
    return 0;
}

static int cmd_test(Ctx *c, int argc, char **argv)
{
    int n = argc - 1;
    char **args = argv + 1;
    if (strcmp(argv[0], "[") == 0) {
        if (n < 1 || strcmp(args[n - 1], "]") != 0) {
            fail(c, "[", NULL, "missing ']'");
            return 2;
        }
        n--;
    }
    int err = 0;
    int r = test_eval(c, n, args, &err);
    if (err) {
        fail(c, argv[0], NULL, "unsupported expression");
        return 2;
    }
    return r ? 0 : 1;
}

const Cmd nr_coreutils[] = {
    {"basename", cmd_basename},
    {"cat", cmd_cat},
    {"chmod", cmd_chmod},
    {"cp", cmd_cp},
    {"cut", cmd_cut},
    {"date", cmd_date},
    {"dirname", cmd_dirname},
    {"echo", cmd_echo},
    {"env", cmd_env},
    {"false", cmd_false},
    {"head", cmd_head},
    {"hostname", cmd_hostname},
    {"id", cmd_id},
    {"ls", cmd_ls},
    {"mkdir", cmd_mkdir},
    {"mv", cmd_mv},
    {"printenv", cmd_printenv},
    {"printf", cmd_printf},
    {"pwd", cmd_pwd},
    {"rm", cmd_rm},
    {"rmdir", cmd_rmdir},
    {"seq", cmd_seq},
    {"sleep", cmd_sleep},
    {"sort", cmd_sort},
    {"tail", cmd_tail},
    {"tee", cmd_tee},
    {"test", cmd_test},
    {"[", cmd_test},
    {"touch", cmd_touch},
    {"tr", cmd_tr},
    {"true", cmd_true},
    {"uname", cmd_uname},
    {"uniq", cmd_uniq},
    {"wc", cmd_wc},
    {"whoami", cmd_whoami},
    {NULL, NULL}
};

CmdFn nr_find_impl(const char *name)
{
    for (int i = 0; nr_coreutils[i].name != NULL; i++) {
        if (strcmp(nr_coreutils[i].name, name) == 0) {
            return nr_coreutils[i].fn;
        }
    }
    return nr_find_tool(name);
}
