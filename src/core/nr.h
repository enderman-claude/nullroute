/*
 * nr.h - shared declarations for the NULLROUTE core.
 *
 * The core is plain C99 with no dependencies beyond the C library. It knows
 * nothing about the window: the GUI calls into it and renders the results.
 */
#ifndef NR_H
#define NR_H

#include <stddef.h>

#define NR_VERSION "0.2.0"
#define NR_PATH_MAX 512
#define NR_NAME_MAX 64
#define NR_TRACE_LIMIT 100
#define NR_MAX_HOSTS 16
#define NR_MAX_LINKS 8
#define NR_MAX_SERVICES 8
#define NR_MAX_USERS 8
#define NR_MAX_TEXTS 96
#define NR_MAX_MISSIONS 16
#define NR_MAX_WORDS 256
#define NR_SAVE_VERSION 2
#define NR_MAX_CREDS 64
#define NR_PLAYER "ghost"  /* account name on the rig */
#define NR_RIG "rig"        /* host id of the rig */

/* ---------- growable byte buffer ---------- */

typedef struct Buf {
    char *data;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_put(Buf *b, const char *s, size_t n);
void buf_str(Buf *b, const char *s);
void buf_char(Buf *b, char c);
void buf_fmt(Buf *b, const char *fmt, ...);
char *buf_steal(Buf *b); /* returns a NUL-terminated copy; caller frees */

char *nr_strdup(const char *s);
char *nr_strndup(const char *s, size_t n);

/* ---------- virtual filesystem ---------- */

typedef struct Node Node;
struct Node {
    char *name;
    int is_dir;
    int mode;
    char *data;
    size_t len;
    Node *parent;
    Node *child; /* first child (directories only) */
    Node *next;  /* next sibling */
};

Node *vfs_new(void);
void vfs_destroy(Node *n);

/* Normalise P relative to BASE (absolute). Writes an absolute path with no
 * "." or ".." components. ".." never climbs above "/". Returns 0 on success. */
int path_join(const char *base, const char *p, char *out, size_t n);

Node *vfs_get(Node *root, const char *abs);
int vfs_mkdir(Node *root, const char *abs);    /* parent must exist */
int vfs_mkdir_p(Node *root, const char *abs);  /* creates parents too */
int vfs_put(Node *root, const char *abs, const char *data, size_t len, int append);
int vfs_remove(Node *root, const char *abs, int recursive);
int vfs_move(Node *root, const char *src, const char *dst);
int vfs_copy(Node *root, const char *src, const char *dst, int recursive);
char *vfs_read(Node *root, const char *abs, size_t *len);
void vfs_serialize(Buf *out, Node *root);
int vfs_deserialize(Node *root, const char *data, size_t len);

/* ---------- hosts ---------- */

typedef struct Service {
    char name[16];
    int port;
    int open;
} Service;

typedef struct User {
    char name[32];
    unsigned int hash;
} User;

typedef struct Host {
    char id[NR_NAME_MAX];
    char name[NR_NAME_MAX];
    char ip[32];
    float x, y;            /* map position, 0..1 */
    int unlock;            /* chapter at which the host becomes visible */
    int security;          /* trace added per command */
    char links[NR_MAX_LINKS][NR_NAME_MAX];
    int nlinks;
    Service services[NR_MAX_SERVICES];
    int nservices;
    char vuln[16];         /* service that can be exploited, or "" */
    User users[NR_MAX_USERS];
    int nusers;
    Node *fs;
    int discovered;        /* saved */
    int owned;             /* saved: root has been obtained */
} Host;

Host *host_find(Host *hosts, int n, const char *id);
int host_service_open(const Host *h, const char *name);

/* ---------- sessions and the command context ---------- */

typedef struct Var {
    char *name;
    char *value;
    struct Var *next;
} Var;

typedef struct Session Session;
struct Session {
    Host *host;
    char user[32];
    int is_root;
    char cwd[NR_PATH_MAX];
    Var *vars;
    int status;     /* $? */
    int trace;      /* percent, for remote sessions */
    Session *parent;
};

typedef struct Game Game;

/* Positional parameters for a running script (sh file args...). */
typedef struct Frame {
    int argc;
    char **argv;
} Frame;

typedef struct Ctx {
    Game *g;
    Session *s;
    Frame *fr;
    const Buf *in;   /* stdin; NULL means an empty stream */
    Buf *out;
    Buf *err;
    int exiting;     /* set by the exit builtin */
    int exit_status;
    int abort;       /* trace dropped or password requested: stop running */
} Ctx;

typedef int (*CmdFn)(Ctx *c, int argc, char **argv);
typedef struct Cmd {
    const char *name;
    CmdFn fn;
} Cmd;

/* Coreutils-equivalent commands (core/coreutils.c) and game tools (game/tools.c).
 * Both tables end with an entry whose name is NULL. */
extern const Cmd nr_coreutils[];
extern const Cmd nr_tools[];

/* Looks a command up by name in the coreutils table, then the game tools. */
CmdFn nr_find_impl(const char *name);
/* Looks a command up in the game tools table only. */
CmdFn nr_find_tool(const char *name);

/* ---------- shell ---------- */

/* Runs a script (a string of shell source) in the session's context. Returns
 * the exit status of the last command. Syntax errors go to c->err. */
int shell_run(Ctx *c, const char *src);

/* Expands a variable value for the session (for prompts and printenv). */
const char *session_getvar(Session *s, const char *name);
void session_setvar(Session *s, const char *name, const char *value);

/* ---------- game state ---------- */

typedef struct Cred {
    char host[NR_NAME_MAX];
    char user[32];
    char pass[64];
} Cred;

typedef struct Text {
    char name[NR_NAME_MAX];
    char *body;
} Text;

typedef struct NotepadReq {
    int pending;
    Host *host;
    char path[NR_PATH_MAX];
} NotepadReq;

struct Game {
    Host hosts[NR_MAX_HOSTS];
    int nhosts;
    Host *rig;

    Text texts[NR_MAX_TEXTS];
    int ntexts;

    char *words[NR_MAX_WORDS];
    int nwords;

    int missions;          /* number of chapters */
    int mission;           /* current chapter, 1-based */
    char mission_title[NR_MAX_MISSIONS][NR_NAME_MAX];
    char mission_loot[NR_MAX_MISSIONS][NR_NAME_MAX];
    int won;               /* all chapters done, waiting for publish/sell */
    char ending[16];       /* "publish" or "sell" once the game is over */

    Session *top;          /* the session the terminal talks to */
    int password_pending;  /* ssh is waiting for a password */
    Host *pw_host;
    char pw_user[32];

    Cred creds[NR_MAX_CREDS];
    int ncreds;
    int first_run;         /* 1 until the rig's profile has run once */

    NotepadReq notepad;
    Buf notices;           /* events shown in the terminal after a command */
    int dirty;             /* state changed since the last save */
};

/* Game lifecycle (core/game.c). */
int game_new(Game *g, const char *world_path);
int game_load(Game *g, const char *world_path, const char *save_path);
int game_save(Game *g, const char *save_path);
void game_destroy(Game *g);

/* Runs the rig's ~/.profile when a shell opens. */
void game_login(Game *g, Buf *out);

/* Terminal interface. */
void game_prompt(Game *g, Buf *out);
void game_exec(Game *g, const char *line, Buf *out);
int game_password(Game *g, const char *pw, Buf *out);
const char *game_text(Game *g, const char *name);
int game_mission_text(Game *g, const char *kind, int n, Buf *out);

/* Filesystem access for the notepad. */
char *game_fs_read(Host *h, const char *abs, size_t *len);
int game_fs_write(Game *g, Host *h, const char *abs, const char *data, size_t len);

/* Tracing and scoring. */
int game_tick(Game *g, Session *s); /* returns 1 if the trace dropped the player */

/* Credentials recovered by cracking or logging in. */
void game_save_cred(Game *g, const char *host, const char *user, const char *pass);
const char *game_find_cred(Game *g, const char *host, const char *user);

/* Session management used by the shell and tools. */
Session *session_new_rig(Game *g);
Session *session_push(Game *g, Host *h, const char *user, int is_root);
void session_pop_to_rig(Game *g, Buf *out);

/* World loader (core/world.c). */
int world_load(Game *g, const char *path, char *err, size_t errlen);

/* Mission progression (core/game.c). */
void game_check_mission(Game *g, Buf *out);

/* Hash used for stored passwords. */
unsigned int nr_hash(const char *s);

/* Host-side lookups used by the tools. */
const char *game_text(Game *g, const char *name);

#endif
