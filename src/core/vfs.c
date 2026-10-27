/* vfs.c - an in-memory POSIX-style filesystem tree, one per host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr.h"

static Node *node_new(const char *name, int is_dir, Node *parent)
{
    Node *n = calloc(1, sizeof *n);
    if (n == NULL) {
        abort();
    }
    n->name = nr_strdup(name);
    n->is_dir = is_dir;
    n->mode = is_dir ? 0755 : 0644;
    n->parent = parent;
    return n;
}

Node *vfs_new(void)
{
    return node_new("/", 1, NULL);
}

void vfs_destroy(Node *n)
{
    if (n == NULL) {
        return;
    }
    Node *c = n->child;
    while (c != NULL) {
        Node *next = c->next;
        vfs_destroy(c);
        c = next;
    }
    free(n->name);
    free(n->data);
    free(n);
}

int path_join(const char *base, const char *rel, char *out, size_t n)
{
    char tmp[NR_PATH_MAX * 2];
    int k;

    if (rel[0] == '/') {
        k = snprintf(tmp, sizeof tmp, "%s", rel);
    } else {
        k = snprintf(tmp, sizeof tmp, "%s/%s", base, rel);
    }
    if (k < 0 || (size_t)k >= sizeof tmp) {
        return -1;
    }

    /* Walk the components with a stack. Each kept name is a private copy, so
     * ".." can pop it safely. */
    char *stack[NR_PATH_MAX / 2];
    int depth = 0;
    const char *q = tmp;
    while (*q != '\0') {
        while (*q == '/') {
            q++;
        }
        if (*q == '\0') {
            break;
        }
        const char *end = strchr(q, '/');
        size_t l = end ? (size_t)(end - q) : strlen(q);
        if (l == 1 && q[0] == '.') {
            /* stay where we are */
        } else if (l == 2 && q[0] == '.' && q[1] == '.') {
            if (depth > 0) {
                free(stack[--depth]);
            }
        } else {
            if (depth >= (int)(sizeof stack / sizeof stack[0])) {
                while (depth > 0) {
                    free(stack[--depth]);
                }
                return -1;
            }
            stack[depth++] = nr_strndup(q, l);
        }
        if (end == NULL) {
            break;
        }
        q = end + 1;
    }

    size_t used = 0;
    int rc = 0;
    if (depth == 0) {
        if (n < 2) {
            rc = -1;
        } else {
            out[used++] = '/';
        }
    }
    for (int i = 0; rc == 0 && i < depth; i++) {
        size_t l = strlen(stack[i]);
        if (used + l + 2 > n) {
            rc = -1;
        } else {
            out[used++] = '/';
            memcpy(out + used, stack[i], l);
            used += l;
        }
    }
    if (rc == 0) {
        out[used] = '\0';
    }
    for (int i = 0; i < depth; i++) {
        free(stack[i]);
    }
    return rc;
}

static Node *child_named(Node *dir, const char *name, size_t len)
{
    for (Node *c = dir->child; c != NULL; c = c->next) {
        if (strlen(c->name) == len && strncmp(c->name, name, len) == 0) {
            return c;
        }
    }
    return NULL;
}

/* Finds the node for an absolute path. Returns NULL if it does not exist. */
Node *vfs_get(Node *root, const char *abs)
{
    Node *cur = root;
    const char *p = abs;
    while (*p == '/') {
        p++;
    }
    while (*p != '\0' && cur != NULL) {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!cur->is_dir) {
            return NULL;
        }
        cur = child_named(cur, p, len);
        if (end == NULL) {
            break;
        }
        p = end + 1;
        while (*p == '/') {
            p++;
        }
    }
    return cur;
}

/* Splits abs into parent path and last name. Returns the parent node. */
static Node *parent_of(Node *root, const char *abs, const char **leaf)
{
    const char *slash = strrchr(abs, '/');
    if (slash == NULL || slash[1] == '\0') {
        return NULL;
    }
    *leaf = slash + 1;
    if (slash == abs) {
        return root;
    }
    char *par = nr_strndup(abs, (size_t)(slash - abs));
    Node *n = vfs_get(root, par);
    free(par);
    if (n == NULL || !n->is_dir) {
        return NULL;
    }
    return n;
}

static void attach(Node *dir, Node *n)
{
    n->parent = dir;
    n->next = dir->child;
    dir->child = n;
}

static void detach(Node *n)
{
    Node *par = n->parent;
    if (par == NULL) {
        return;
    }
    if (par->child == n) {
        par->child = n->next;
    } else {
        for (Node *c = par->child; c != NULL; c = c->next) {
            if (c->next == n) {
                c->next = n->next;
                break;
            }
        }
    }
    n->next = NULL;
    n->parent = NULL;
}

int vfs_mkdir(Node *root, const char *abs)
{
    const char *leaf;
    Node *par = parent_of(root, abs, &leaf);
    if (par == NULL || child_named(par, leaf, strlen(leaf))) {
        return -1;
    }
    attach(par, node_new(leaf, 1, par));
    return 0;
}

int vfs_mkdir_p(Node *root, const char *abs)
{
    Node *cur = root;
    const char *p = abs;
    while (*p == '/') {
        p++;
    }
    while (*p != '\0') {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        Node *next = child_named(cur, p, len);
        if (next == NULL) {
            char *name = nr_strndup(p, len);
            next = node_new(name, 1, cur);
            free(name);
            attach(cur, next);
        } else if (!next->is_dir) {
            return -1;
        }
        cur = next;
        if (end == NULL) {
            break;
        }
        p = end + 1;
        while (*p == '/') {
            p++;
        }
    }
    return 0;
}

int vfs_put(Node *root, const char *abs, const char *data, size_t len, int append)
{
    const char *leaf;
    Node *par = parent_of(root, abs, &leaf);
    if (par == NULL) {
        return -1;
    }
    Node *n = child_named(par, leaf, strlen(leaf));
    if (n != NULL && n->is_dir) {
        return -1;
    }
    if (n == NULL) {
        n = node_new(leaf, 0, par);
        attach(par, n);
    }
    if (append && n->data != NULL) {
        char *nd = realloc(n->data, n->len + len + 1);
        if (nd == NULL) {
            abort();
        }
        memcpy(nd + n->len, data, len);
        n->len += len;
        nd[n->len] = '\0';
        n->data = nd;
    } else {
        free(n->data);
        n->data = nr_strndup(data, len);
        n->len = len;
    }
    return 0;
}

int vfs_remove(Node *root, const char *abs, int recursive)
{
    Node *n = vfs_get(root, abs);
    if (n == NULL || n == root) {
        return -1;
    }
    if (n->is_dir && n->child != NULL && !recursive) {
        return -2;
    }
    detach(n);
    vfs_destroy(n);
    return 0;
}

static int is_inside(const char *dir, const char *path)
{
    size_t l = strlen(dir);
    if (strcmp(dir, "/") == 0) {
        return 1;
    }
    return strncmp(dir, path, l) == 0 && (path[l] == '\0' || path[l] == '/');
}

int vfs_move(Node *root, const char *src, const char *dst)
{
    Node *n = vfs_get(root, src);
    if (n == NULL || n == root) {
        return -1;
    }
    if (is_inside(src, dst)) {
        return -2; /* cannot move a directory into itself */
    }
    const char *leaf;
    Node *par = parent_of(root, dst, &leaf);
    if (par == NULL) {
        return -1;
    }
    Node *old = child_named(par, leaf, strlen(leaf));
    if (old != NULL) {
        if (old == n) {
            return 0;
        }
        if (old->is_dir != n->is_dir) {
            return -3;
        }
        detach(old);
        vfs_destroy(old);
    }
    detach(n);
    free(n->name);
    n->name = nr_strdup(leaf);
    attach(par, n);
    return 0;
}

static int copy_node(Node *root, Node *from, const char *dst, int recursive)
{
    if (!from->is_dir) {
        return vfs_put(root, dst, from->data ? from->data : "", from->len, 0);
    }
    if (!recursive) {
        return -2;
    }
    if (vfs_mkdir(root, dst) != 0 && vfs_get(root, dst) == NULL) {
        return -1;
    }
    for (Node *c = from->child; c != NULL; c = c->next) {
        char child[NR_PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", dst, c->name) >= (int)sizeof child) {
            return -1;
        }
        int rc = copy_node(root, c, child, recursive);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

int vfs_copy(Node *root, const char *src, const char *dst, int recursive)
{
    Node *n = vfs_get(root, src);
    if (n == NULL) {
        return -1;
    }
    if (n->is_dir && is_inside(src, dst)) {
        return -3;
    }
    return copy_node(root, n, dst, recursive);
}

char *vfs_read(Node *root, const char *abs, size_t *len)
{
    Node *n = vfs_get(root, abs);
    if (n == NULL || n->is_dir) {
        return NULL;
    }
    if (len != NULL) {
        *len = n->len;
    }
    return nr_strndup(n->data ? n->data : "", n->len);
}

/* Serialisation format, one record per entry:
 *   D <path>\n
 *   F <length> <path>\n<length bytes>\n
 */
static void serialize_dir(Buf *out, Node *dir, const char *prefix)
{
    for (Node *c = dir->child; c != NULL; c = c->next) {
        char path[NR_PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", strcmp(prefix, "/") == 0 ? "" : prefix, c->name);
        if (c->is_dir) {
            buf_fmt(out, "D %s\n", path);
            serialize_dir(out, c, path);
        } else {
            buf_fmt(out, "F %lu %s\n", (unsigned long)c->len, path);
            buf_put(out, c->data ? c->data : "", c->len);
            buf_char(out, '\n');
        }
    }
}

void vfs_serialize(Buf *out, Node *root)
{
    serialize_dir(out, root, "/");
}

int vfs_deserialize(Node *root, const char *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        const char *eol = memchr(data + i, '\n', len - i);
        if (eol == NULL) {
            return -1;
        }
        size_t line_len = (size_t)(eol - (data + i));
        char *line = nr_strndup(data + i, line_len);
        i += line_len + 1;

        if (line[0] == 'D' && line[1] == ' ') {
            int rc = vfs_mkdir_p(root, line + 2);
            free(line);
            if (rc != 0) {
                return -1;
            }
        } else if (line[0] == 'F' && line[1] == ' ') {
            unsigned long flen = 0;
            char path[NR_PATH_MAX];
            if (sscanf(line + 2, "%lu %511[^\n]", &flen, path) != 2) {
                free(line);
                return -1;
            }
            free(line);
            if (i + flen > len) {
                return -1;
            }
            if (vfs_put(root, path, data + i, flen, 0) != 0) {
                return -1;
            }
            i += flen + 1; /* skip the trailing newline */
        } else {
            free(line);
            return -1;
        }
    }
    return 0;
}
