/*
 * test_core.c - tests for the NULLROUTE core, with no window.
 *
 * Usage: nr_tests PATH/TO/assets/world.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr.h"

static int tests_run = 0;
static int tests_failed = 0;
static int current_failed = 0;
static const char *world_path;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            if (!current_failed) {                                           \
                fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            }                                                                \
            current_failed = 1;                                              \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        const char *_a = (actual);                                           \
        if (_a == NULL || strcmp(_a, (expected)) != 0) {                     \
            if (!current_failed) {                                           \
                fprintf(stderr, "  FAIL %s:%d: got \"%s\", want \"%s\"\n",   \
                        __FILE__, __LINE__, _a ? _a : "(null)", (expected)); \
            }                                                                \
            current_failed = 1;                                              \
        }                                                                    \
    } while (0)

#define TEST(name)                                                           \
    static void name(void);                                                  \
    static void run_##name(void)                                             \
    {                                                                        \
        current_failed = 0;                                                  \
        tests_run++;                                                         \
        name();                                                              \
        if (current_failed) {                                                \
            tests_failed++;                                                  \
            fprintf(stderr, "not ok - %s\n", #name);                         \
        } else {                                                             \
            printf("ok - %s\n", #name);                                      \
        }                                                                    \
    }                                                                        \
    static void name(void)

/* A game in progress, with the output of the last command kept. */
typedef struct Run {
    Game g;
    Buf out;
} Run;

static void run_init(Run *r)
{
    memset(r, 0, sizeof *r);
    if (game_new(&r->g, world_path) != 0) {
        fprintf(stderr, "could not load %s\n", world_path);
        exit(2);
    }
    buf_init(&r->out);
}

static void run_done(Run *r)
{
    buf_free(&r->out);
    game_destroy(&r->g);
}

/* Types a line at the terminal and returns the output. */
static const char *typed(Run *r, const char *line)
{
    buf_free(&r->out);
    buf_init(&r->out);
    game_exec(&r->g, line, &r->out);
    return r->out.data ? r->out.data : "";
}

/* Runs shell source with the given stdout and stderr buffers. */
static int sh(Run *r, const char *src, Buf *out, Buf *err)
{
    Ctx c;
    memset(&c, 0, sizeof c);
    c.g = &r->g;
    c.s = r->g.top;
    c.out = out;
    c.err = err;
    return shell_run(&c, src);
}

/* ======================================================================
 * Paths and the virtual filesystem
 * ====================================================================== */

TEST(path_normalisation)
{
    char out[NR_PATH_MAX];
    CHECK(path_join("/home/ghost", "notes.txt", out, sizeof out) == 0);
    CHECK_STR(out, "/home/ghost/notes.txt");
    CHECK(path_join("/home/ghost", "../clerk/./x", out, sizeof out) == 0);
    CHECK_STR(out, "/home/clerk/x");
    CHECK(path_join("/home/ghost", "/etc//passwd", out, sizeof out) == 0);
    CHECK_STR(out, "/etc/passwd");
    CHECK(path_join("/", "../../../../etc", out, sizeof out) == 0);
    CHECK_STR(out, "/etc");
    CHECK(path_join("/a/b", "..", out, sizeof out) == 0);
    CHECK_STR(out, "/a");
    CHECK(path_join("/a", ".", out, sizeof out) == 0);
    CHECK_STR(out, "/a");
}

TEST(vfs_basic_operations)
{
    Node *root = vfs_new();
    CHECK(vfs_mkdir_p(root, "/a/b") == 0);
    CHECK(vfs_put(root, "/a/b/f.txt", "hello", 5, 0) == 0);
    CHECK(vfs_put(root, "/a/b/f.txt", " there", 6, 1) == 0);
    size_t len = 0;
    char *d = vfs_read(root, "/a/b/f.txt", &len);
    CHECK(d != NULL && len == 11 && memcmp(d, "hello there", 11) == 0);
    free(d);
    CHECK(vfs_move(root, "/a/b/f.txt", "/a/g.txt") == 0);
    CHECK(vfs_get(root, "/a/b/f.txt") == NULL);
    CHECK(vfs_get(root, "/a/g.txt") != NULL);
    CHECK(vfs_copy(root, "/a", "/c", 0) != 0);
    CHECK(vfs_copy(root, "/a", "/c", 1) == 0);
    CHECK(vfs_get(root, "/c/b") != NULL);
    CHECK(vfs_remove(root, "/a", 0) != 0);
    CHECK(vfs_remove(root, "/a", 1) == 0);
    CHECK(vfs_get(root, "/a") == NULL);
    vfs_destroy(root);
}

TEST(vfs_serialise_round_trip)
{
    Node *root = vfs_new();
    vfs_mkdir_p(root, "/home/x");
    vfs_put(root, "/home/x/multi.txt", "line one\nline two\n", 18, 0);
    vfs_put(root, "/empty", "", 0, 0);
    Buf b;
    buf_init(&b);
    vfs_serialize(&b, root);
    Node *copy = vfs_new();
    CHECK(vfs_deserialize(copy, b.data, b.len) == 0);
    size_t len = 0;
    char *d = vfs_read(copy, "/home/x/multi.txt", &len);
    CHECK(d != NULL && len == 18 && memcmp(d, "line one\nline two\n", 18) == 0);
    free(d);
    CHECK(vfs_get(copy, "/empty") != NULL);
    buf_free(&b);
    vfs_destroy(root);
    vfs_destroy(copy);
}

/* ======================================================================
 * The shell language
 * ====================================================================== */

TEST(shell_echo_and_quoting)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    sh(&r, "echo hello world", &out, &err);
    CHECK_STR(out.data, "hello world\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "echo 'two  spaces' \"and $HOME\"", &out, &err);
    CHECK_STR(out.data, "two  spaces and /home/ghost\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "echo \\$HOME \"a\\\"b\"", &out, &err);
    CHECK_STR(out.data, "$HOME a\"b\n");
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

TEST(shell_variables_and_status)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    sh(&r, "x=5; echo $x ${x} ${nope:-fallback}", &out, &err);
    CHECK_STR(out.data, "5 5 fallback\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "false; echo $?", &out, &err);
    CHECK_STR(out.data, "1\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "y=7 printenv y", &out, &err);
    CHECK_STR(out.data, "7\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "echo ${y:-unset}", &out, &err);
    CHECK_STR(out.data, "unset\n");
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

TEST(shell_lists_and_pipelines)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    sh(&r, "false && echo no; true || echo no; false || echo yes", &out, &err);
    CHECK_STR(out.data, "yes\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "echo abc | tr a-z A-Z | tr -d B", &out, &err);
    CHECK_STR(out.data, "AC\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "printf 'b\\na\\nb\\n' | sort | uniq -c", &out, &err);
    CHECK_STR(out.data, "      1 a\n      2 b\n");
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

TEST(shell_redirections)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    sh(&r, "echo one > /home/ghost/r.txt; echo two >> /home/ghost/r.txt; cat /home/ghost/r.txt", &out, &err);
    CHECK_STR(out.data, "one\ntwo\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "cat < /home/ghost/r.txt | head -n 1", &out, &err);
    CHECK_STR(out.data, "one\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "cat /nope 2>&1", &out, &err);
    CHECK(strstr(out.data, "No such file") != NULL);
    buf_free(&out);
    buf_init(&out);
    sh(&r, "cat /nope 2>/dev/null", &out, &err);
    CHECK(out.len == 0);
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

TEST(shell_control_flow)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    sh(&r, "if test 2 -gt 1; then echo big; else echo small; fi", &out, &err);
    CHECK_STR(out.data, "big\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "if false; then echo a; elif true; then echo b; else echo c; fi", &out, &err);
    CHECK_STR(out.data, "b\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "for w in red green; do echo $w; done", &out, &err);
    CHECK_STR(out.data, "red\ngreen\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "n=0; while test $n -lt 3; do echo $n; n=$((n+1)); done", &out, &err);
    CHECK_STR(out.data, "0\n1\n2\n");
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

TEST(shell_expansions)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    sh(&r, "echo $((2+3*4)) $(((2+3)*4)) $((10/3)) $((10%4))", &out, &err);
    CHECK_STR(out.data, "14 20 3 2\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "echo $(echo inner) `echo back`", &out, &err);
    CHECK_STR(out.data, "inner back\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "ls /bin/[c]at /bin/c[p]", &out, &err);
    CHECK_STR(out.data, "/bin/cat\n/bin/cp\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "echo /bin/ca*", &out, &err);
    CHECK_STR(out.data, "/bin/cat\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "echo \"/bin/ca*\"", &out, &err);
    CHECK_STR(out.data, "/bin/ca*\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "cd /bin; pwd", &out, &err);
    CHECK_STR(out.data, "/bin\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "cd /home/ghost/loot; cd ..; pwd", &out, &err);
    CHECK_STR(out.data, "/home/ghost\n");
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

TEST(shell_errors_and_exit)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    int st = sh(&r, "nosuchcommand", &out, &err);
    CHECK(st == 127);
    CHECK(strstr(err.data, "not found") != NULL);
    buf_free(&err);
    buf_init(&err);
    st = sh(&r, "if true; then", &out, &err);
    CHECK(st == 2);
    CHECK(strstr(err.data, "syntax error") != NULL);
    buf_free(&err);
    buf_init(&err);
    st = sh(&r, "echo ( nope", &out, &err);
    CHECK(st == 2);
    buf_free(&err);
    buf_init(&err);
    sh(&r, "echo before; exit 3; echo after", &out, &err);
    CHECK(strstr(out.data, "after") == NULL);
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

TEST(shell_scripts_with_arguments)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    sh(&r, "printf 'echo script got $1 and $#\\n' > /home/ghost/s.sh; sh /home/ghost/s.sh alpha beta",
       &out, &err);
    CHECK_STR(out.data, "script got alpha and 2\n");
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

/* ======================================================================
 * Coreutils
 * ====================================================================== */

TEST(coreutils_text_tools)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    sh(&r, "seq 1 5 | tail -n 2", &out, &err);
    CHECK_STR(out.data, "4\n5\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "seq 3 | sort -r", &out, &err);
    CHECK_STR(out.data, "3\n2\n1\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "echo a:b:c | cut -d: -f1,3", &out, &err);
    CHECK_STR(out.data, "a:c\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "printf 'x\\ny\\nz\\n' | head -n 2", &out, &err);
    CHECK_STR(out.data, "x\ny\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "basename /a/b/file.txt .txt; dirname /a/b/file.txt", &out, &err);
    CHECK_STR(out.data, "file\n/a/b\n");
    buf_free(&out);
    buf_init(&out);
    sh(&r, "printf '%s-%d\\n' abc 7", &out, &err);
    CHECK_STR(out.data, "abc-7\n");
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

TEST(coreutils_files)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    CHECK(sh(&r, "mkdir -p /home/ghost/t/deep && touch /home/ghost/t/deep/a", &out, &err) == 0);
    CHECK(sh(&r, "cp -r /home/ghost/t /home/ghost/u", &out, &err) == 0);
    CHECK(sh(&r, "mv /home/ghost/u/deep/a /home/ghost/u/b", &out, &err) == 0);
    buf_free(&out);
    buf_init(&out);
    sh(&r, "ls /home/ghost/u", &out, &err);
    CHECK_STR(out.data, "b  deep\n");
    buf_free(&out);
    buf_init(&out);
    CHECK(sh(&r, "rm -r /home/ghost/t", &out, &err) == 0);
    buf_free(&err);
    buf_init(&err);
    CHECK(sh(&r, "rmdir /home/ghost/u", &out, &err) != 0);
    CHECK(strstr(err.data, "not empty") != NULL);
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

TEST(coreutils_protection)
{
    Run r;
    run_init(&r);
    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    /* The rig keeps its mail and loot folders. */
    CHECK(sh(&r, "rm -r /home/ghost/loot", &out, &err) != 0);
    CHECK(strstr(err.data, "Permission denied") != NULL);
    buf_free(&err);
    buf_init(&err);
    CHECK(sh(&r, "rm -r /", &out, &err) != 0);
    buf_free(&out);
    buf_free(&err);
    run_done(&r);
}

/* ======================================================================
 * Hacking mechanics
 * ====================================================================== */

TEST(scan_reveals_linked_hosts)
{
    Run r;
    run_init(&r);
    const char *o = typed(&r, "scan");
    CHECK(strstr(o, "crumbco-pos") != NULL);
    CHECK(strstr(o, "meridian-gw") == NULL);
    run_done(&r);
}

TEST(hosts_must_be_discovered)
{
    Run r;
    run_init(&r);
    const char *o = typed(&r, "ssh clerk@crumbco-pos");
    CHECK(r.g.top->host == r.g.rig);
    CHECK(strstr(o, "password") == NULL);
    run_done(&r);
}

TEST(crack_finds_dictionary_password)
{
    Run r;
    run_init(&r);
    typed(&r, "scan");
    const char *o = typed(&r, "crack crumbco-pos clerk");
    CHECK(strstr(o, "Password found after 26 attempts: dough") != NULL);
    CHECK(game_find_cred(&r.g, "crumbco-pos", "clerk") != NULL);
    run_done(&r);
}

TEST(ssh_asks_for_password_and_checks_it)
{
    Run r;
    run_init(&r);
    typed(&r, "scan");
    typed(&r, "ssh clerk@crumbco-pos");
    CHECK(r.g.password_pending == 1);
    Buf out;
    buf_init(&out);
    CHECK(game_password(&r.g, "wrong", &out) != 0);
    CHECK(strstr(out.data, "Permission denied") != NULL);
    buf_free(&out);
    typed(&r, "ssh clerk@crumbco-pos");
    buf_init(&out);
    CHECK(game_password(&r.g, "dough", &out) == 0);
    CHECK(strstr(out.data, "Welcome to crumbco-pos") != NULL);
    CHECK(r.g.top->host == host_find(r.g.hosts, r.g.nhosts, "crumbco-pos"));
    buf_free(&out);
    run_done(&r);
}

TEST(scp_copies_loot_and_completes_chapter)
{
    Run r;
    run_init(&r);
    typed(&r, "scan");
    typed(&r, "crack crumbco-pos clerk");
    const char *o = typed(&r, "scp crumbco-pos:/home/clerk/cashdrawer.log ~/loot/");
    CHECK(strstr(o, "100%") != NULL);
    CHECK(strstr(o, "CHAPTER 1 COMPLETE") != NULL);
    CHECK(r.g.mission == 2);
    CHECK(game_fs_read(r.g.rig, "/home/ghost/mail/002.txt", NULL) != NULL);
    run_done(&r);
}

TEST(trace_drops_connection)
{
    Run r;
    run_init(&r);
    typed(&r, "scan");
    typed(&r, "crack crumbco-pos clerk");
    typed(&r, "scp crumbco-pos:/home/clerk/cashdrawer.log ~/loot/");
    typed(&r, "ssh clerk@crumbco-pos");
    typed(&r, "scan");
    typed(&r, "crack meridian-gw admin");
    typed(&r, "ssh admin@meridian-gw");
    CHECK(r.g.top->host == host_find(r.g.hosts, r.g.nhosts, "meridian-gw"));
    /* meridian-gw has security 3, so about 34 commands reach the limit. */
    int dropped = 0;
    for (int i = 0; i < 60; i++) {
        const char *o = typed(&r, "ls");
        if (strstr(o, "TRACE COMPLETE") != NULL) {
            dropped = 1;
            break;
        }
    }
    CHECK(dropped);
    CHECK(r.g.top->host == r.g.rig);
    run_done(&r);
}

/* Returns to the rig from any depth of remote sessions. */
static void leave(Run *r)
{
    while (r->g.top->parent != NULL) {
        typed(r, "exit");
    }
}

/* The whole campaign, through the commands a player types. */
TEST(full_playthrough_reaches_the_ending)
{
    Run r;
    run_init(&r);
    typed(&r, "scan");
    typed(&r, "crack crumbco-pos clerk");
    typed(&r, "scp crumbco-pos:/home/clerk/cashdrawer.log ~/loot/");
    CHECK(r.g.mission == 2);

    typed(&r, "ssh clerk@crumbco-pos");
    typed(&r, "scan");
    typed(&r, "crack meridian-gw admin");
    leave(&r);
    typed(&r, "scp meridian-gw:/srv/routes.cfg ~/loot/");
    CHECK(r.g.mission == 3);

    typed(&r, "ssh admin@meridian-gw");
    typed(&r, "scan");
    typed(&r, "exploit fleet-ctl ssh");
    CHECK(r.g.top->is_root == 1);
    leave(&r);
    typed(&r, "scp fleet-ctl:/root/fleet_override.key ~/loot/");
    CHECK(r.g.mission == 4);

    typed(&r, "exploit fleet-ctl ssh");
    typed(&r, "scan");
    typed(&r, "crack audit-vault auditor");
    leave(&r);
    typed(&r, "scp audit-vault:/var/audit/audit_trail.log ~/loot/");
    CHECK(r.g.mission == 5);

    typed(&r, "ssh auditor@audit-vault");
    typed(&r, "scan");
    typed(&r, "crack vell-drop vell");
    leave(&r);
    const char *o = typed(&r, "scp vell-drop:/home/vell/vell_manifest.txt ~/loot/");
    CHECK(strstr(o, "evidence is ready") != NULL);
    CHECK(r.g.won == 1);

    o = typed(&r, "publish");
    CHECK(strstr(o, "newsrooms") != NULL);
    CHECK_STR(r.g.ending, "publish");
    run_done(&r);
}

TEST(scp_only_runs_from_the_rig)
{
    Run r;
    run_init(&r);
    typed(&r, "scan");
    typed(&r, "crack crumbco-pos clerk");
    typed(&r, "ssh clerk@crumbco-pos");
    const char *o = typed(&r, "scp crumbco-pos:/home/clerk/cashdrawer.log ~/loot/");
    CHECK(strstr(o, "run scp from your rig") != NULL);
    o = typed(&r, "exit");
    CHECK(strstr(o, "Connection to crumbco-pos closed") != NULL);
    CHECK(r.g.top->host == r.g.rig);
    run_done(&r);
}

TEST(save_and_load_round_trip)
{
    Run r;
    run_init(&r);
    typed(&r, "scan");
    typed(&r, "crack crumbco-pos clerk");
    typed(&r, "scp crumbco-pos:/home/clerk/cashdrawer.log ~/loot/");
    char path[512];
    snprintf(path, sizeof path, "%s", "/tmp/nullroute-test-save.dat");
    CHECK(game_save(&r.g, path) == 0);
    Run s;
    memset(&s, 0, sizeof s);
    buf_init(&s.out);
    CHECK(game_load(&s.g, world_path, path) == 0);
    CHECK(s.g.mission == 2);
    CHECK(s.g.nhosts == r.g.nhosts);
    CHECK(host_find(s.g.hosts, s.g.nhosts, "crumbco-pos")->discovered == 1);
    CHECK(game_find_cred(&s.g, "crumbco-pos", "clerk") != NULL);
    CHECK(game_fs_read(s.g.rig, "/home/ghost/loot/cashdrawer.log", NULL) != NULL);
    buf_free(&s.out);
    game_destroy(&s.g);
    remove(path);
    run_done(&r);
}

int main(int argc, char **argv)
{
    world_path = argc > 1 ? argv[1] : "assets/world.txt";

    run_path_normalisation();
    run_vfs_basic_operations();
    run_vfs_serialise_round_trip();
    run_shell_echo_and_quoting();
    run_shell_variables_and_status();
    run_shell_lists_and_pipelines();
    run_shell_redirections();
    run_shell_control_flow();
    run_shell_expansions();
    run_shell_errors_and_exit();
    run_shell_scripts_with_arguments();
    run_coreutils_text_tools();
    run_coreutils_files();
    run_coreutils_protection();
    run_scan_reveals_linked_hosts();
    run_hosts_must_be_discovered();
    run_crack_finds_dictionary_password();
    run_ssh_asks_for_password_and_checks_it();
    run_scp_copies_loot_and_completes_chapter();
    run_trace_drops_connection();
    run_full_playthrough_reaches_the_ending();
    run_scp_only_runs_from_the_rig();
    run_save_and_load_round_trip();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
