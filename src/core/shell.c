/*
 * shell.c - a POSIX-style shell that runs inside the game's virtual machines.
 *
 * Supported:
 *   - simple commands, assignments before commands (NAME=value cmd)
 *   - quoting: '...', "...", \x, and backquotes/$(...) for substitution
 *   - parameters: $name ${name} ${name:-word} $? $# $0 $1..$9 $@ $*
 *   - arithmetic: $((expr)) with + - * / % and parentheses
 *   - field splitting on unquoted expansions, and pathname expansion (* ? [...])
 *   - pipelines |, lists ; && ||, and redirections < > >> 2> 2>&1
 *   - compound commands: if/elif/else/fi, while/do/done, until, for/in/do/done
 *   - builtins: cd, exit, export, unset, :, sh
 *
 * Not supported (and reported as syntax errors): subshells ( ), background
 * jobs &, here-documents, functions, case, and job control.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr.h"

#define LOOP_LIMIT 10000

/* ======================================================================
 * Lexer
 * ====================================================================== */

typedef enum { TK_WORD, TK_IONUM, TK_OP, TK_NL, TK_EOF } TkType;

typedef struct Tok {
    TkType type;
    char *text;
    int fd;
} Tok;

typedef struct Lex {
    const char *s;
    size_t i;
    Tok *t;
    int n;
    int cap;
    char err[160];
    int failed;
} Lex;

static void lex_fail(Lex *l, const char *msg)
{
    if (!l->failed) {
        snprintf(l->err, sizeof l->err, "%s", msg);
        l->failed = 1;
    }
}

static void tok_add(Lex *l, TkType type, const char *text, size_t n, int fd)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->t = realloc(l->t, sizeof *l->t * (size_t)l->cap);
        if (l->t == NULL) {
            abort();
        }
    }
    Tok *t = &l->t[l->n++];
    t->type = type;
    t->text = nr_strndup(text ? text : "", n);
    t->fd = fd;
}

/* The skip_* helpers advance *i past a quoted or nested construct. They
 * return 0 on success and -1 if the input ends first. */
static int skip_paren(const char *s, size_t *i);

static int skip_squote(const char *s, size_t *i)
{
    const char *e = strchr(s + *i + 1, '\'');
    if (e == NULL) {
        return -1;
    }
    *i = (size_t)(e - s) + 1;
    return 0;
}

static int skip_dquote(const char *s, size_t *i)
{
    (*i)++;
    while (s[*i] != '\0') {
        if (s[*i] == '\\' && s[*i + 1] != '\0') {
            *i += 2;
        } else if (s[*i] == '"') {
            (*i)++;
            return 0;
        } else if (s[*i] == '$' && s[*i + 1] == '(') {
            (*i)++;
            if (skip_paren(s, i) != 0) {
                return -1;
            }
        } else {
            (*i)++;
        }
    }
    return -1;
}

/* s[*i] must be '(' */
static int skip_paren(const char *s, size_t *i)
{
    int depth = 0;
    while (s[*i] != '\0') {
        char c = s[*i];
        if (c == '\\' && s[*i + 1] != '\0') {
            *i += 2;
            continue;
        }
        if (c == '\'') {
            if (skip_squote(s, i) != 0) {
                return -1;
            }
            continue;
        }
        if (c == '"') {
            if (skip_dquote(s, i) != 0) {
                return -1;
            }
            continue;
        }
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                (*i)++;
                return 0;
            }
        }
        (*i)++;
    }
    return -1;
}

static void lex_word(Lex *l)
{
    const char *s = l->s;
    size_t start = l->i;
    while (s[l->i] != '\0') {
        char c = s[l->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' ||
            c == '|' || c == '<' || c == '>' || c == '(' || c == ')') {
            break;
        }
        if (c == '\\') {
            l->i += s[l->i + 1] != '\0' ? 2 : 1;
        } else if (c == '\'') {
            if (skip_squote(s, &l->i) != 0) {
                lex_fail(l, "unterminated single quote");
                return;
            }
        } else if (c == '"') {
            if (skip_dquote(s, &l->i) != 0) {
                lex_fail(l, "unterminated double quote");
                return;
            }
        } else if (c == '`') {
            l->i++;
            while (s[l->i] != '\0' && s[l->i] != '`') {
                if (s[l->i] == '\\' && s[l->i + 1] != '\0') {
                    l->i++;
                }
                l->i++;
            }
            if (s[l->i] == '\0') {
                lex_fail(l, "unterminated backquote");
                return;
            }
            l->i++;
        } else if (c == '$' && s[l->i + 1] == '(') {
            l->i++;
            if (skip_paren(s, &l->i) != 0) {
                lex_fail(l, "unterminated $(");
                return;
            }
        } else if (c == '$' && s[l->i + 1] == '{') {
            const char *e = strchr(s + l->i + 2, '}');
            if (e == NULL) {
                lex_fail(l, "unterminated ${");
                return;
            }
            l->i = (size_t)(e - s) + 1;
        } else {
            l->i++;
        }
    }
    size_t len = l->i - start;
    int all_digits = len > 0;
    for (size_t k = 0; k < len; k++) {
        if (!isdigit((unsigned char)s[start + k])) {
            all_digits = 0;
            break;
        }
    }
    if (all_digits && (s[l->i] == '<' || s[l->i] == '>')) {
        tok_add(l, TK_IONUM, s + start, len, atoi(s + start));
    } else {
        tok_add(l, TK_WORD, s + start, len, 0);
    }
}

static int lex(const char *src, Lex *l)
{
    l->s = src;
    l->i = 0;
    for (;;) {
        const char *s = l->s;
        while (s[l->i] == ' ' || s[l->i] == '\t' || (s[l->i] == '\\' && s[l->i + 1] == '\n')) {
            l->i += s[l->i] == '\\' ? 2 : 1;
        }
        char c = s[l->i];
        if (c == '\0') {
            tok_add(l, TK_EOF, "", 0, 0);
            break;
        }
        if (c == '#') {
            while (s[l->i] != '\0' && s[l->i] != '\n') {
                l->i++;
            }
            continue;
        }
        if (c == '\n') {
            tok_add(l, TK_NL, "\n", 1, 0);
            l->i++;
            continue;
        }
        if (c == ';') {
            if (s[l->i + 1] == ';') {
                lex_fail(l, "';;' is not supported");
                break;
            }
            tok_add(l, TK_OP, ";", 1, 0);
            l->i++;
            continue;
        }
        if (c == '|') {
            if (s[l->i + 1] == '|') {
                tok_add(l, TK_OP, "||", 2, 0);
                l->i += 2;
            } else {
                tok_add(l, TK_OP, "|", 1, 0);
                l->i++;
            }
            continue;
        }
        if (c == '&') {
            if (s[l->i + 1] == '&') {
                tok_add(l, TK_OP, "&&", 2, 0);
                l->i += 2;
                continue;
            }
            lex_fail(l, "background jobs are not supported");
            break;
        }
        if (c == '<') {
            if (s[l->i + 1] == '<') {
                lex_fail(l, "here-documents are not supported");
                break;
            }
            tok_add(l, TK_OP, "<", 1, 0);
            l->i++;
            continue;
        }
        if (c == '>') {
            if (s[l->i + 1] == '>') {
                tok_add(l, TK_OP, ">>", 2, 0);
                l->i += 2;
            } else if (s[l->i + 1] == '&') {
                tok_add(l, TK_OP, ">&", 2, 0);
                l->i += 2;
            } else {
                tok_add(l, TK_OP, ">", 1, 0);
                l->i++;
            }
            continue;
        }
        if (c == '(' || c == ')') {
            lex_fail(l, "subshells are not supported");
            break;
        }
        lex_word(l);
        if (l->failed) {
            break;
        }
    }
    return l->failed ? -1 : 0;
}

/* ======================================================================
 * Parser and AST
 * ====================================================================== */

typedef enum {
    AST_SIMPLE,
    AST_PIPE,
    AST_AND,
    AST_OR,
    AST_SEQ,
    AST_IF,
    AST_WHILE,
    AST_UNTIL,
    AST_FOR
} AstKind;

typedef struct Redir {
    int fd;
    char op[4];
    char *target;
} Redir;

typedef struct Ast Ast;
struct Ast {
    AstKind kind;
    char **words;
    int nwords;
    int words_is_all_args; /* for: no "in" list, iterate over $@ */
    Redir *redirs;
    int nredirs;
    Ast **kids;
    int nkids;
    Ast *a;
    Ast *b;
    Ast *c;
    char *var;
};

static Ast *ast_new(AstKind kind)
{
    Ast *a = calloc(1, sizeof *a);
    if (a == NULL) {
        abort();
    }
    a->kind = kind;
    return a;
}

static void ast_kid(Ast *a, Ast *k)
{
    a->kids = realloc(a->kids, sizeof *a->kids * (size_t)(a->nkids + 1));
    if (a->kids == NULL) {
        abort();
    }
    a->kids[a->nkids++] = k;
}

static void ast_word(Ast *a, const char *w)
{
    a->words = realloc(a->words, sizeof *a->words * (size_t)(a->nwords + 1));
    if (a->words == NULL) {
        abort();
    }
    a->words[a->nwords++] = nr_strdup(w);
}

static void ast_free(Ast *a)
{
    if (a == NULL) {
        return;
    }
    for (int i = 0; i < a->nwords; i++) {
        free(a->words[i]);
    }
    free(a->words);
    for (int i = 0; i < a->nredirs; i++) {
        free(a->redirs[i].target);
    }
    free(a->redirs);
    for (int i = 0; i < a->nkids; i++) {
        ast_free(a->kids[i]);
    }
    free(a->kids);
    ast_free(a->a);
    ast_free(a->b);
    ast_free(a->c);
    free(a->var);
    free(a);
}

typedef struct Parser {
    Tok *t;
    int n;
    int i;
    int failed;
    char err[160];
} Parser;

static void p_fail(Parser *p, const char *msg)
{
    if (!p->failed) {
        snprintf(p->err, sizeof p->err, "%s", msg);
        p->failed = 1;
    }
}

static Tok *p_cur(Parser *p)
{
    return &p->t[p->i];
}

static int p_is_op(Parser *p, const char *op)
{
    Tok *t = p_cur(p);
    return t->type == TK_OP && strcmp(t->text, op) == 0;
}

static int p_is_word(Parser *p, const char *w)
{
    Tok *t = p_cur(p);
    return t->type == TK_WORD && strcmp(t->text, w) == 0;
}

static int p_at_stop(Parser *p, const char *const *stops)
{
    Tok *t = p_cur(p);
    if (t->type != TK_WORD || stops == NULL) {
        return 0;
    }
    for (int k = 0; stops[k] != NULL; k++) {
        if (strcmp(t->text, stops[k]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void p_expect_word(Parser *p, const char *w)
{
    if (p_is_word(p, w)) {
        p->i++;
    } else {
        char msg[96];
        snprintf(msg, sizeof msg, "expected '%s'", w);
        p_fail(p, msg);
    }
}

static void p_skip_seps(Parser *p)
{
    while (p_is_op(p, ";") || p_cur(p)->type == TK_NL) {
        p->i++;
    }
}

static Ast *p_seq(Parser *p, const char *const *stops);

static int is_redir_op(const char *t)
{
    return strcmp(t, "<") == 0 || strcmp(t, ">") == 0 || strcmp(t, ">>") == 0 ||
           strcmp(t, ">&") == 0;
}

static Ast *p_simple(Parser *p)
{
    Ast *a = ast_new(AST_SIMPLE);
    for (;;) {
        Tok *t = p_cur(p);
        if (t->type == TK_WORD) {
            ast_word(a, t->text);
            p->i++;
            continue;
        }
        if (t->type == TK_IONUM || (t->type == TK_OP && is_redir_op(t->text))) {
            int fd = -1;
            if (t->type == TK_IONUM) {
                fd = t->fd;
                p->i++;
                t = p_cur(p);
                if (!(t->type == TK_OP && is_redir_op(t->text))) {
                    p_fail(p, "syntax error in redirection");
                    break;
                }
            }
            Redir r;
            memset(&r, 0, sizeof r);
            snprintf(r.op, sizeof r.op, "%s", t->text);
            r.fd = fd >= 0 ? fd : (strcmp(t->text, "<") == 0 ? 0 : 1);
            p->i++;
            Tok *w = p_cur(p);
            if (w->type != TK_WORD) {
                p_fail(p, "missing redirection target");
                break;
            }
            r.target = nr_strdup(w->text);
            p->i++;
            a->redirs = realloc(a->redirs, sizeof *a->redirs * (size_t)(a->nredirs + 1));
            if (a->redirs == NULL) {
                abort();
            }
            a->redirs[a->nredirs++] = r;
            continue;
        }
        break;
    }
    if (!p->failed && a->nwords == 0 && a->nredirs == 0) {
        p_fail(p, "unexpected token");
    }
    return a;
}

static Ast *p_pipeline_stage(Parser *p);

static Ast *p_if_after_if(Parser *p)
{
    static const char *const then_stop[] = {"then", NULL};
    static const char *const body_stop[] = {"elif", "else", "fi", NULL};
    Ast *node = ast_new(AST_IF);
    node->a = p_seq(p, then_stop);
    if (p->failed) {
        return node;
    }
    p_expect_word(p, "then");
    node->b = p_seq(p, body_stop);
    if (p->failed) {
        return node;
    }
    if (p_is_word(p, "elif")) {
        p->i++;
        node->c = p_if_after_if(p);
        return node;
    }
    if (p_is_word(p, "else")) {
        static const char *const else_stop[] = {"fi", NULL};
        p->i++;
        node->c = p_seq(p, else_stop);
    }
    p_expect_word(p, "fi");
    return node;
}

static Ast *p_loop(Parser *p, AstKind kind)
{
    static const char *const cond_stop[] = {"do", NULL};
    static const char *const body_stop[] = {"done", NULL};
    p->i++;
    Ast *node = ast_new(kind);
    node->a = p_seq(p, cond_stop);
    p_expect_word(p, "do");
    if (p->failed) {
        return node;
    }
    node->b = p_seq(p, body_stop);
    p_expect_word(p, "done");
    return node;
}

static Ast *p_for(Parser *p)
{
    static const char *const body_stop[] = {"done", NULL};
    p->i++;
    Ast *node = ast_new(AST_FOR);
    if (p_cur(p)->type != TK_WORD) {
        p_fail(p, "expected a variable name after 'for'");
        return node;
    }
    node->var = nr_strdup(p_cur(p)->text);
    p->i++;
    p_skip_seps(p);
    if (p_is_word(p, "in")) {
        p->i++;
        node->words_is_all_args = 0;
        while (p_cur(p)->type == TK_WORD) {
            ast_word(node, p_cur(p)->text);
            p->i++;
        }
        if (!(p_is_op(p, ";") || p_cur(p)->type == TK_NL)) {
            p_fail(p, "expected ';' after the for word list");
            return node;
        }
        p_skip_seps(p);
    } else {
        node->words_is_all_args = 1;
    }
    p_expect_word(p, "do");
    if (p->failed) {
        return node;
    }
    node->b = p_seq(p, body_stop);
    p_expect_word(p, "done");
    return node;
}

static Ast *p_command(Parser *p)
{
    Tok *t = p_cur(p);
    if (t->type == TK_WORD) {
        if (strcmp(t->text, "if") == 0) {
            p->i++;
            return p_if_after_if(p);
        }
        if (strcmp(t->text, "while") == 0) {
            return p_loop(p, AST_WHILE);
        }
        if (strcmp(t->text, "until") == 0) {
            return p_loop(p, AST_UNTIL);
        }
        if (strcmp(t->text, "for") == 0) {
            return p_for(p);
        }
        if (strcmp(t->text, "then") == 0 || strcmp(t->text, "elif") == 0 ||
            strcmp(t->text, "else") == 0 || strcmp(t->text, "fi") == 0 ||
            strcmp(t->text, "do") == 0 || strcmp(t->text, "done") == 0) {
            char msg[96];
            snprintf(msg, sizeof msg, "syntax error near unexpected token '%s'", t->text);
            p_fail(p, msg);
            return ast_new(AST_SIMPLE);
        }
    }
    return p_simple(p);
}

static Ast *p_pipeline_stage(Parser *p)
{
    return p_command(p);
}

static Ast *p_pipeline(Parser *p)
{
    Ast *first = p_pipeline_stage(p);
    if (p->failed || !p_is_op(p, "|")) {
        return first;
    }
    Ast *pipe = ast_new(AST_PIPE);
    ast_kid(pipe, first);
    while (p_is_op(p, "|")) {
        p->i++;
        ast_kid(pipe, p_pipeline_stage(p));
        if (p->failed) {
            break;
        }
    }
    return pipe;
}

static Ast *p_and_or(Parser *p)
{
    Ast *left = p_pipeline(p);
    while (!p->failed && (p_is_op(p, "&&") || p_is_op(p, "||"))) {
        AstKind kind = p_is_op(p, "&&") ? AST_AND : AST_OR;
        p->i++;
        while (p_cur(p)->type == TK_NL) {
            p->i++;
        }
        Ast *node = ast_new(kind);
        node->a = left;
        node->b = p_pipeline(p);
        left = node;
    }
    return left;
}

static Ast *p_seq(Parser *p, const char *const *stops)
{
    Ast *seq = ast_new(AST_SEQ);
    for (;;) {
        p_skip_seps(p);
        if (p->failed || p_cur(p)->type == TK_EOF || p_at_stop(p, stops)) {
            break;
        }
        Ast *cmd = p_and_or(p);
        if (p->failed) {
            break;
        }
        ast_kid(seq, cmd);
        Tok *t = p_cur(p);
        if (!(t->type == TK_EOF || t->type == TK_NL || (t->type == TK_OP && strcmp(t->text, ";") == 0) ||
              p_at_stop(p, stops))) {
            p_fail(p, "syntax error near unexpected token");
            break;
        }
    }
    return seq;
}

/* ======================================================================
 * Expansion
 * ====================================================================== */

/* Each character of an expanded field carries a kind:
 *   '1' quoted (never split, never globbed)
 *   '0' unquoted (globbed if it is * ? or [)
 */
typedef struct Fld {
    Buf s;
    Buf q;
    int quoted_any;
} Fld;

typedef struct Flds {
    Fld *f;
    int n;
    int cap;
} Flds;

typedef struct Strs {
    char **v;
    int n;
    int cap;
} Strs;

typedef enum { K_QUOTED, K_LIT, K_EXP } Kind;

typedef struct Exp {
    Ctx *c;
    Flds *list;
    Fld cur;
    int split;
} Exp;

static void strs_push(Strs *s, char *item)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = realloc(s->v, sizeof *s->v * (size_t)s->cap);
        if (s->v == NULL) {
            abort();
        }
    }
    s->v[s->n++] = item;
}

static void strs_free(Strs *s)
{
    for (int i = 0; i < s->n; i++) {
        free(s->v[i]);
    }
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

static void exp_end(Exp *e)
{
    if (e->cur.s.len == 0 && !e->cur.quoted_any) {
        buf_free(&e->cur.s);
        buf_free(&e->cur.q);
        e->cur.quoted_any = 0;
        return;
    }
    if (e->list->n == e->list->cap) {
        e->list->cap = e->list->cap ? e->list->cap * 2 : 8;
        e->list->f = realloc(e->list->f, sizeof *e->list->f * (size_t)e->list->cap);
        if (e->list->f == NULL) {
            abort();
        }
    }
    e->list->f[e->list->n++] = e->cur;
    buf_init(&e->cur.s);
    buf_init(&e->cur.q);
    e->cur.quoted_any = 0;
}

static void exp_char(Exp *e, char ch, Kind kind)
{
    if (kind == K_EXP && e->split && (ch == ' ' || ch == '\t' || ch == '\n')) {
        exp_end(e);
        return;
    }
    buf_char(&e->cur.s, ch);
    buf_char(&e->cur.q, kind == K_QUOTED ? '1' : '0');
    if (kind == K_QUOTED) {
        e->cur.quoted_any = 1;
    }
}

static void exp_text(Exp *e, const char *text, Kind kind)
{
    for (const char *p = text; *p != '\0'; p++) {
        exp_char(e, *p, kind);
    }
}

static const char *positional(Ctx *c, int n)
{
    if (c->fr == NULL || n < 1 || n > c->fr->argc) {
        return "";
    }
    return c->fr->argv[n - 1];
}

/* Runs a command string and returns its standard output, without trailing
 * newlines. The parent's $? is preserved. */
static void cmd_subst(Ctx *c, const char *src, Buf *res)
{
    Ctx sub = *c;
    int saved = c->s->status;
    sub.out = res;
    sub.exiting = 0;
    sub.exit_status = 0;
    shell_run(&sub, src);
    c->abort = c->abort || sub.abort;
    c->s->status = saved;
    while (res->len > 0 && res->data[res->len - 1] == '\n') {
        res->len--;
    }
    if (res->data != NULL) {
        res->data[res->len] = '\0';
    }
}

/* Arithmetic: a small recursive-descent evaluator. */
typedef struct Arith {
    Ctx *c;
    const char *s;
    size_t i;
    int err;
} Arith;

static long ar_expr(Arith *a);

static void ar_ws(Arith *a)
{
    while (a->s[a->i] == ' ' || a->s[a->i] == '\t') {
        a->i++;
    }
}

static long ar_primary(Arith *a)
{
    ar_ws(a);
    char ch = a->s[a->i];
    if (ch == '(') {
        a->i++;
        long v = ar_expr(a);
        ar_ws(a);
        if (a->s[a->i] == ')') {
            a->i++;
        } else {
            a->err = 1;
        }
        return v;
    }
    if (ch == '-') {
        a->i++;
        return -ar_primary(a);
    }
    if (ch == '+') {
        a->i++;
        return ar_primary(a);
    }
    if (isdigit((unsigned char)ch)) {
        long v = 0;
        while (isdigit((unsigned char)a->s[a->i])) {
            v = v * 10 + (a->s[a->i] - '0');
            a->i++;
        }
        return v;
    }
    if (isalpha((unsigned char)ch) || ch == '_') {
        char name[64];
        size_t n = 0;
        while ((isalnum((unsigned char)a->s[a->i]) || a->s[a->i] == '_') && n < sizeof name - 1) {
            name[n++] = a->s[a->i++];
        }
        name[n] = '\0';
        const char *v = session_getvar(a->c->s, name);
        return v ? strtol(v, NULL, 10) : 0;
    }
    a->err = 1;
    return 0;
}

static long ar_term(Arith *a)
{
    long v = ar_primary(a);
    for (;;) {
        ar_ws(a);
        char op = a->s[a->i];
        if (op == '*') {
            a->i++;
            v *= ar_primary(a);
        } else if (op == '/' || op == '%') {
            a->i++;
            long r = ar_primary(a);
            if (r == 0) {
                a->err = 1;
                r = 1;
            }
            v = op == '/' ? v / r : v % r;
        } else {
            break;
        }
    }
    return v;
}

static long ar_expr(Arith *a)
{
    long v = ar_term(a);
    for (;;) {
        ar_ws(a);
        char op = a->s[a->i];
        if (op == '+') {
            a->i++;
            v += ar_term(a);
        } else if (op == '-') {
            a->i++;
            v -= ar_term(a);
        } else {
            break;
        }
    }
    return v;
}

static void expand_dollar(Exp *e, const char *raw, size_t *i, int inq);

static void expand_dollar_text(Exp *e, const char *text, Kind kind)
{
    exp_text(e, text, kind);
}

/* inq is 1 inside double quotes: the result is then quoted text. */
static void expand_dollar(Exp *e, const char *raw, size_t *i, int inq)
{
    Ctx *c = e->c;
    size_t p = *i + 1;
    char ch = raw[p];
    Kind kind = inq ? K_QUOTED : (e->split ? K_EXP : K_LIT);
    Buf val;
    buf_init(&val);

    if (ch == '(' && raw[p + 1] == '(') {
        size_t end = p;
        if (skip_paren(raw, &end) != 0) {
            buf_free(&val);
            exp_char(e, '$', K_LIT);
            *i += 1;
            return;
        }
        char *expr = nr_strndup(raw + p + 1, end - p - 2);
        Arith a = {c, expr, 0, 0};
        long v = ar_expr(&a);
        if (a.err) {
            buf_str(c->err, "sh: arithmetic error\n");
        }
        free(expr);
        buf_fmt(&val, "%ld", v);
        exp_text(e, val.data, kind);
        buf_free(&val);
        *i = end;
        return;
    }
    if (ch == '(') {
        size_t end = p;
        if (skip_paren(raw, &end) != 0) {
            exp_char(e, '$', K_LIT);
            *i += 1;
            return;
        }
        char *inner = nr_strndup(raw + p + 1, end - p - 2);
        cmd_subst(c, inner, &val);
        free(inner);
        exp_text(e, val.data ? val.data : "", kind);
        buf_free(&val);
        *i = end;
        return;
    }
    if (ch == '{') {
        const char *close = strchr(raw + p + 1, '}');
        if (close == NULL) {
            exp_char(e, '$', K_LIT);
            *i += 1;
            return;
        }
        char *body = nr_strndup(raw + p + 1, (size_t)(close - (raw + p + 1)));
        char *colon = strstr(body, ":-");
        if (colon != NULL) {
            *colon = '\0';
            const char *v = session_getvar(c->s, body);
            if (v == NULL || *v == '\0') {
                expand_dollar_text(e, colon + 2, kind);
            } else {
                expand_dollar_text(e, v, kind);
            }
        } else {
            const char *v = session_getvar(c->s, body);
            expand_dollar_text(e, v ? v : "", kind);
        }
        free(body);
        *i = (size_t)(close - raw) + 1;
        return;
    }
    if (ch == '?') {
        buf_fmt(&val, "%d", c->s->status);
    } else if (ch == '$') {
        buf_str(&val, "1");
    } else if (ch == '#') {
        buf_fmt(&val, "%d", c->fr ? c->fr->argc : 0);
    } else if (ch == '0') {
        buf_str(&val, "sh");
    } else if (ch >= '1' && ch <= '9') {
        buf_str(&val, positional(c, ch - '0'));
    } else if (ch == '@' || ch == '*') {
        if (c->fr != NULL) {
            for (int k = 0; k < c->fr->argc; k++) {
                if (k > 0) {
                    buf_char(&val, ' ');
                }
                buf_str(&val, c->fr->argv[k]);
            }
        }
    } else if (isalpha((unsigned char)ch) || ch == '_') {
        size_t n = p;
        while (isalnum((unsigned char)raw[n]) || raw[n] == '_') {
            n++;
        }
        char *name = nr_strndup(raw + p, n - p);
        const char *v = session_getvar(c->s, name);
        buf_str(&val, v ? v : "");
        free(name);
        *i = n;
        exp_text(e, val.data ? val.data : "", kind);
        buf_free(&val);
        return;
    } else {
        exp_char(e, '$', K_LIT);
        *i += 1;
        buf_free(&val);
        return;
    }
    exp_text(e, val.data ? val.data : "", kind);
    buf_free(&val);
    *i = p + 1;
}

/* Expands one raw word into fields. With split == 0 (assignments, redirection
 * targets) the result is a single field and nothing is globbed. */
static void expand_word(Ctx *c, const char *raw, int split, Flds *out)
{
    Exp e;
    e.c = c;
    e.list = out;
    e.split = split;
    buf_init(&e.cur.s);
    buf_init(&e.cur.q);
    e.cur.quoted_any = 0;

    size_t i = 0;
    if (raw[0] == '~' && (raw[1] == '/' || raw[1] == '\0')) {
        const char *home = session_getvar(c->s, "HOME");
        exp_text(&e, home ? home : "/", K_QUOTED);
        i = 1;
    }
    while (raw[i] != '\0') {
        char ch = raw[i];
        if (ch == '\\') {
            if (raw[i + 1] != '\0') {
                exp_char(&e, raw[i + 1], K_QUOTED);
                i += 2;
            } else {
                i++;
            }
        } else if (ch == '\'') {
            e.cur.quoted_any = 1;
            i++;
            while (raw[i] != '\0' && raw[i] != '\'') {
                exp_char(&e, raw[i], K_QUOTED);
                i++;
            }
            if (raw[i] != '\0') {
                i++;
            }
        } else if (ch == '"') {
            e.cur.quoted_any = 1;
            i++;
            while (raw[i] != '\0' && raw[i] != '"') {
                if (raw[i] == '\\' && raw[i + 1] != '\0' && strchr("$`\"\\\n", raw[i + 1])) {
                    exp_char(&e, raw[i + 1], K_QUOTED);
                    i += 2;
                } else if (raw[i] == '$') {
                    expand_dollar(&e, raw, &i, 1);
                } else {
                    exp_char(&e, raw[i], K_QUOTED);
                    i++;
                }
            }
            if (raw[i] != '\0') {
                i++;
            }
        } else if (ch == '`') {
            size_t close = i + 1;
            while (raw[close] != '\0' && raw[close] != '`') {
                if (raw[close] == '\\' && raw[close + 1] != '\0') {
                    close++;
                }
                close++;
            }
            char *inner = nr_strndup(raw + i + 1, close - i - 1);
            Buf res;
            buf_init(&res);
            cmd_subst(c, inner, &res);
            free(inner);
            exp_text(&e, res.data ? res.data : "", e.split ? K_EXP : K_LIT);
            buf_free(&res);
            i = raw[close] ? close + 1 : close;
        } else if (ch == '$') {
            expand_dollar(&e, raw, &i, 0);
        } else {
            exp_char(&e, ch, K_LIT);
            i++;
        }
    }
    exp_end(&e);
    buf_free(&e.cur.s);
    buf_free(&e.cur.q);
}

static int field_has_magic(const Fld *f)
{
    for (size_t k = 0; k < f->s.len; k++) {
        if (f->q.data[k] == '0' && (f->s.data[k] == '*' || f->s.data[k] == '?' || f->s.data[k] == '[')) {
            return 1;
        }
    }
    return 0;
}

/* Matches a pattern (with per-character quoted flags) against a name. */
static int pmatch(const char *p, const char *pq, const char *s)
{
    if (*p == '\0') {
        return *s == '\0';
    }
    if (*p == '*' && pq[0] == '0') {
        for (;;) {
            if (pmatch(p + 1, pq + 1, s)) {
                return 1;
            }
            if (*s == '\0') {
                return 0;
            }
            s++;
        }
    }
    if (*p == '?' && pq[0] == '0') {
        if (*s == '\0') {
            return 0;
        }
        return pmatch(p + 1, pq + 1, s + 1);
    }
    if (*p == '[' && pq[0] == '0') {
        const char *q = p + 1;
        int neg = 0;
        if (*q == '!' || *q == '^') {
            neg = 1;
            q++;
        }
        const char *scan = q;
        if (*scan == ']') {
            scan++;
        }
        while (*scan != '\0' && *scan != ']') {
            scan++;
        }
        if (*scan == '\0') {
            /* No closing bracket: '[' is an ordinary character. */
            if (*s != '[') {
                return 0;
            }
            return pmatch(p + 1, pq + 1, s + 1);
        }
        if (*s == '\0') {
            return 0;
        }
        int matched = 0;
        for (const char *r = q; r < scan; r++) {
            if (r + 2 < scan && r[1] == '-') {
                if (*s >= r[0] && *s <= r[2]) {
                    matched = 1;
                }
                r += 2;
            } else if (*r == *s) {
                matched = 1;
            }
        }
        if (matched == neg) {
            return 0;
        }
        return pmatch(scan + 1, pq + (size_t)(scan + 1 - p), s + 1);
    }
    if (*s != *p) {
        return 0;
    }
    return pmatch(p + 1, pq + 1, s + 1);
}

static char *join_path(const char *base, const char *name)
{
    size_t bl = strlen(base);
    size_t nl = strlen(name);
    char *out = malloc(bl + nl + 2);
    if (out == NULL) {
        abort();
    }
    if (bl == 0) {
        memcpy(out, name, nl + 1);
    } else if (base[bl - 1] == '/') {
        memcpy(out, base, bl);
        memcpy(out + bl, name, nl + 1);
    } else {
        memcpy(out, base, bl);
        out[bl] = '/';
        memcpy(out + bl + 1, name, nl + 1);
    }
    return out;
}

typedef struct Comp {
    char *text;
    char *q;
} Comp;

/* Pathname expansion for one field. Appends matches to out, or the literal
 * text if nothing matches. */
static void glob_field(Ctx *c, const Fld *f, Strs *out)
{
    size_t len = f->s.len;
    const char *s = f->s.data ? f->s.data : "";
    const char *q = f->q.data ? f->q.data : "";
    int absolute = len > 0 && s[0] == '/';

    Comp *comps = NULL;
    int nc = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && s[i] == '/') {
            i++;
        }
        if (i >= len) {
            break;
        }
        size_t st = i;
        while (i < len && s[i] != '/') {
            i++;
        }
        comps = realloc(comps, sizeof *comps * (size_t)(nc + 1));
        if (comps == NULL) {
            abort();
        }
        comps[nc].text = nr_strndup(s + st, i - st);
        comps[nc].q = nr_strndup(q + st, i - st);
        nc++;
    }

    Strs cands = {NULL, 0, 0};
    strs_push(&cands, nr_strdup(absolute ? "/" : ""));

    for (int k = 0; k < nc; k++) {
        Strs next = {NULL, 0, 0};
        int magic = 0;
        for (size_t m = 0; comps[k].q[m] != '\0'; m++) {
            if (comps[k].q[m] == '0' && strchr("*?[", comps[k].text[m])) {
                magic = 1;
                break;
            }
        }
        for (int j = 0; j < cands.n; j++) {
            if (!magic) {
                strs_push(&next, join_path(cands.v[j], comps[k].text));
                continue;
            }
            char dir[NR_PATH_MAX];
            const char *rel = cands.v[j][0] ? cands.v[j] : ".";
            if (path_join(c->s->cwd, rel, dir, sizeof dir) != 0) {
                continue;
            }
            Node *n = vfs_get(c->s->host->fs, dir);
            if (n == NULL || !n->is_dir) {
                continue;
            }
            for (Node *ch = n->child; ch != NULL; ch = ch->next) {
                /* Names starting with '.' match only patterns that start with '.'. */
                if (ch->name[0] == '.' && comps[k].text[0] != '.') {
                    continue;
                }
                if (pmatch(comps[k].text, comps[k].q, ch->name)) {
                    strs_push(&next, join_path(cands.v[j], ch->name));
                }
            }
        }
        strs_free(&cands);
        cands = next;
    }

    size_t before = (size_t)out->n;
    for (int j = 0; j < cands.n; j++) {
        char abs[NR_PATH_MAX];
        const char *rel = cands.v[j][0] ? cands.v[j] : ".";
        if (path_join(c->s->cwd, rel, abs, sizeof abs) == 0 && vfs_get(c->s->host->fs, abs) != NULL) {
            strs_push(out, nr_strdup(cands.v[j]));
        }
    }
    if ((size_t)out->n == before) {
        strs_push(out, nr_strndup(s, len));
    } else {
        /* Sort the new matches, as a shell would. */
        for (int a = (int)before; a < out->n; a++) {
            for (int b = a + 1; b < out->n; b++) {
                if (strcmp(out->v[b], out->v[a]) < 0) {
                    char *t = out->v[a];
                    out->v[a] = out->v[b];
                    out->v[b] = t;
                }
            }
        }
    }
    strs_free(&cands);
    for (int k = 0; k < nc; k++) {
        free(comps[k].text);
        free(comps[k].q);
    }
    free(comps);
}

/* Expands a list of raw words into argv strings, applying field splitting
 * and pathname expansion. */
static void expand_list(Ctx *c, char **words, int n, Strs *out)
{
    for (int i = 0; i < n; i++) {
        Flds fl = {NULL, 0, 0};
        expand_word(c, words[i], 1, &fl);
        for (int k = 0; k < fl.n; k++) {
            if (field_has_magic(&fl.f[k])) {
                glob_field(c, &fl.f[k], out);
            } else {
                strs_push(out, nr_strndup(fl.f[k].s.data ? fl.f[k].s.data : "", fl.f[k].s.len));
            }
            buf_free(&fl.f[k].s);
            buf_free(&fl.f[k].q);
        }
        free(fl.f);
    }
}

/* Expands one raw word to a single string with no splitting or globbing. */
static char *expand_one(Ctx *c, const char *raw)
{
    Flds fl = {NULL, 0, 0};
    expand_word(c, raw, 0, &fl);
    char *s = nr_strdup("");
    if (fl.n > 0) {
        free(s);
        s = nr_strndup(fl.f[0].s.data ? fl.f[0].s.data : "", fl.f[0].s.len);
    }
    for (int k = 0; k < fl.n; k++) {
        buf_free(&fl.f[k].s);
        buf_free(&fl.f[k].q);
    }
    free(fl.f);
    return s;
}

/* ======================================================================
 * Session variables
 * ====================================================================== */

const char *session_getvar(Session *s, const char *name)
{
    for (Var *v = s->vars; v != NULL; v = v->next) {
        if (strcmp(v->name, name) == 0) {
            return v->value;
        }
    }
    return NULL;
}

void session_setvar(Session *s, const char *name, const char *value)
{
    for (Var *v = s->vars; v != NULL; v = v->next) {
        if (strcmp(v->name, name) == 0) {
            free(v->value);
            v->value = nr_strdup(value);
            return;
        }
    }
    Var *v = calloc(1, sizeof *v);
    if (v == NULL) {
        abort();
    }
    v->name = nr_strdup(name);
    v->value = nr_strdup(value);
    v->next = s->vars;
    s->vars = v;
}

static void session_unsetvar(Session *s, const char *name)
{
    Var **pp = &s->vars;
    while (*pp != NULL) {
        Var *v = *pp;
        if (strcmp(v->name, name) == 0) {
            *pp = v->next;
            free(v->name);
            free(v->value);
            free(v);
            return;
        }
        pp = &v->next;
    }
}

/* ======================================================================
 * Command resolution and execution
 * ====================================================================== */

static const char *BINARY_MARK = "NULLROUTE-BINARY ";

static CmdFn stub_impl(Ctx *c, const char *path)
{
    size_t len = 0;
    char *data = vfs_read(c->s->host->fs, path, &len);
    if (data == NULL) {
        return NULL;
    }
    CmdFn fn = NULL;
    size_t mark = strlen(BINARY_MARK);
    if (len > mark && strncmp(data, BINARY_MARK, mark) == 0) {
        char name[NR_NAME_MAX];
        size_t n = 0;
        for (size_t k = mark; k < len && data[k] != '\n' && n < sizeof name - 1; k++) {
            name[n++] = data[k];
        }
        name[n] = '\0';
        fn = nr_find_impl(name);
    }
    free(data);
    return fn;
}

static CmdFn resolve_command(Ctx *c, const char *name)
{
    char path[NR_PATH_MAX];
    if (strchr(name, '/') != NULL) {
        if (path_join(c->s->cwd, name, path, sizeof path) != 0) {
            return NULL;
        }
        return stub_impl(c, path);
    }
    const char *pathv = session_getvar(c->s, "PATH");
    char *copy = nr_strdup(pathv ? pathv : "/bin");
    char *save = copy;
    for (;;) {
        char *colon = strchr(save, ':');
        if (colon != NULL) {
            *colon = '\0';
        }
        if (save[0] != '\0') {
            char full[NR_PATH_MAX];
            if (snprintf(full, sizeof full, "%s/%s", save, name) < (int)sizeof full) {
                if (path_join("/", full, path, sizeof path) == 0) {
                    CmdFn fn = stub_impl(c, path);
                    if (fn != NULL) {
                        free(copy);
                        return fn;
                    }
                }
            }
        }
        if (colon == NULL) {
            break;
        }
        save = colon + 1;
    }
    free(copy);
    /* Remote shells carry the game's toolkit with them, even when the target
     * host has no copy in its /bin. The rig's own tools live in /usr/bin. */
    if (c->s->parent != NULL) {
        return nr_find_tool(name);
    }
    return NULL;
}

static int run_builtin(Ctx *c, int argc, char **argv, int *handled);

/* Runs one resolved command with the given standard streams. */
static int run_argv(Ctx *c, int argc, char **argv, const Buf *in, Buf *out, Buf *err)
{
    Ctx sub = *c;
    sub.in = in;
    sub.out = out;
    sub.err = err;
    sub.exiting = 0;

    int handled = 0;
    int st = run_builtin(&sub, argc, argv, &handled);
    if (!handled) {
        CmdFn fn = resolve_command(&sub, argv[0]);
        if (fn == NULL) {
            buf_fmt(err, "sh: %s: not found\n", argv[0]);
            st = 127;
        } else {
            if (game_tick(c->g, c->s)) {
                buf_str(err, "connection closed by remote host\n");
                c->abort = 1;
                return 255;
            }
            st = fn(&sub, argc, argv);
        }
    }
    c->abort = c->abort || sub.abort;
    if (sub.exiting) {
        c->exiting = 1;
        c->exit_status = sub.exit_status;
    }
    return st;
}

static int bi_cd(Ctx *c, int argc, char **argv)
{
    Session *s = c->s;
    const char *target = argc > 1 ? argv[1] : session_getvar(s, "HOME");
    if (target == NULL) {
        target = "/";
    }
    char abs[NR_PATH_MAX];
    if (path_join(s->cwd, target, abs, sizeof abs) != 0) {
        buf_fmt(c->err, "sh: cd: %s: path too long\n", target);
        return 1;
    }
    Node *n = vfs_get(s->host->fs, abs);
    if (n == NULL || !n->is_dir) {
        buf_fmt(c->err, "sh: cd: can't cd to %s\n", target);
        return 2;
    }
    snprintf(s->cwd, sizeof s->cwd, "%s", abs);
    session_setvar(s, "PWD", abs);
    return 0;
}

static int bi_export(Ctx *c, int argc, char **argv)
{
    if (argc == 1) {
        for (Var *v = c->s->vars; v != NULL; v = v->next) {
            buf_fmt(c->out, "export %s=%s\n", v->name, v->value);
        }
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq != NULL) {
            char *name = nr_strndup(argv[i], (size_t)(eq - argv[i]));
            session_setvar(c->s, name, eq + 1);
            free(name);
        } else if (session_getvar(c->s, argv[i]) == NULL) {
            session_setvar(c->s, argv[i], "");
        }
    }
    return 0;
}

static int bi_sh(Ctx *c, int argc, char **argv)
{
    if (argc < 2) {
        buf_str(c->err, "sh: usage: sh FILE [ARG...]\n");
        return 2;
    }
    char abs[NR_PATH_MAX];
    if (path_join(c->s->cwd, argv[1], abs, sizeof abs) != 0) {
        return 2;
    }
    size_t len = 0;
    char *text = vfs_read(c->s->host->fs, abs, &len);
    if (text == NULL) {
        buf_fmt(c->err, "sh: can't open %s\n", argv[1]);
        return 2;
    }
    Frame fr;
    fr.argc = argc - 2;
    fr.argv = argv + 2;
    Ctx sub = *c;
    sub.fr = &fr;
    sub.exiting = 0;
    int st = shell_run(&sub, text);
    free(text);
    c->abort = c->abort || sub.abort;
    return st;
}

static int run_builtin(Ctx *c, int argc, char **argv, int *handled)
{
    *handled = 1;
    const char *name = argv[0];
    if (strcmp(name, "cd") == 0) {
        return bi_cd(c, argc, argv);
    }
    if (strcmp(name, "export") == 0) {
        return bi_export(c, argc, argv);
    }
    if (strcmp(name, "unset") == 0) {
        for (int i = 1; i < argc; i++) {
            session_unsetvar(c->s, argv[i]);
        }
        return 0;
    }
    if (strcmp(name, "exit") == 0) {
        c->exiting = 1;
        c->exit_status = argc > 1 ? atoi(argv[1]) : c->s->status;
        return c->exit_status;
    }
    if (strcmp(name, ":") == 0) {
        return 0;
    }
    if (strcmp(name, "sh") == 0) {
        return bi_sh(c, argc, argv);
    }
    *handled = 0;
    return 0;
}

static int exec_node(Ctx *c, Ast *a);

static void write_redirect_file(Ctx *c, const char *target, int append, const Buf *data)
{
    char abs[NR_PATH_MAX];
    if (path_join(c->s->cwd, target, abs, sizeof abs) != 0) {
        buf_fmt(c->err, "sh: %s: bad path\n", target);
        return;
    }
    Node *dir_check = vfs_get(c->s->host->fs, abs);
    if (dir_check != NULL && dir_check->is_dir) {
        buf_fmt(c->err, "sh: %s: Is a directory\n", target);
        return;
    }
    if (vfs_put(c->s->host->fs, abs, data->data ? data->data : "", data->len, append) != 0) {
        buf_fmt(c->err, "sh: can't create %s: no such directory\n", target);
    }
}

static int exec_simple(Ctx *c, Ast *a)
{
    int i = 0;
    while (i < a->nwords) {
        const char *w = a->words[i];
        const char *eq = strchr(w, '=');
        if (eq == NULL || eq == w || !(isalpha((unsigned char)w[0]) || w[0] == '_')) {
            break;
        }
        int ok = 1;
        for (const char *p = w; p < eq; p++) {
            if (!(isalnum((unsigned char)*p) || *p == '_')) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
        i++;
    }
    int nassign = i;

    /* Expand assignment values now, so the command can see the old values. */
    char **anames = NULL;
    char **avals = NULL;
    for (int k = 0; k < nassign; k++) {
        const char *eq = strchr(a->words[k], '=');
        anames = realloc(anames, sizeof *anames * (size_t)(k + 1));
        avals = realloc(avals, sizeof *avals * (size_t)(k + 1));
        if (anames == NULL || avals == NULL) {
            abort();
        }
        anames[k] = nr_strndup(a->words[k], (size_t)(eq - a->words[k]));
        avals[k] = expand_one(c, eq + 1);
    }

    Strs argv = {NULL, 0, 0};
    expand_list(c, a->words + nassign, a->nwords - nassign, &argv);

    if (argv.n == 0) {
        for (int k = 0; k < nassign; k++) {
            session_setvar(c->s, anames[k], avals[k]);
        }
        for (int k = 0; k < nassign; k++) {
            free(anames[k]);
            free(avals[k]);
        }
        free(anames);
        free(avals);
        strs_free(&argv);
        return 0;
    }

    /* Temporary assignments for this command only. */
    char **old = calloc((size_t)(nassign + 1), sizeof *old);
    for (int k = 0; k < nassign; k++) {
        const char *prev = session_getvar(c->s, anames[k]);
        old[k] = prev ? nr_strdup(prev) : NULL;
        session_setvar(c->s, anames[k], avals[k]);
    }

    /* Redirections. Targets are expanded with no splitting. */
    Buf inbuf;
    buf_init(&inbuf);
    const Buf *in = c->in;
    int have_in_file = 0;
    for (int r = 0; r < a->nredirs; r++) {
        Redir *rd = &a->redirs[r];
        if (strcmp(rd->op, "<") != 0) {
            continue;
        }
        char *target = expand_one(c, rd->target);
        char abs[NR_PATH_MAX];
        size_t len = 0;
        char *data = NULL;
        if (path_join(c->s->cwd, target, abs, sizeof abs) == 0) {
            data = vfs_read(c->s->host->fs, abs, &len);
        }
        if (data == NULL) {
            buf_fmt(c->err, "sh: can't open %s\n", target);
            free(target);
            buf_free(&inbuf);
            c->s->status = 1;
            goto cleanup;
        }
        buf_free(&inbuf);
        buf_put(&inbuf, data, len);
        free(data);
        free(target);
        have_in_file = 1;
    }
    if (have_in_file) {
        in = &inbuf;
    }

    /* Truncate or create output files before the command runs, as shells do. */
    for (int r = 0; r < a->nredirs; r++) {
        Redir *rd = &a->redirs[r];
        if (rd->op[0] == '>' && rd->op[1] != '&') {
            char *target = expand_one(c, rd->target);
            Buf empty;
            buf_init(&empty);
            write_redirect_file(c, target, rd->op[1] == '>', &empty);
            free(target);
            buf_free(&empty);
        }
    }

    Buf out, err;
    buf_init(&out);
    buf_init(&err);
    int st = run_argv(c, (int)argv.n, (char **)argv.v, in, &out, &err);

    /* 2>&1: merge stderr into stdout. */
    int dup_err_to_out = 0;
    for (int r = 0; r < a->nredirs; r++) {
        Redir *rd = &a->redirs[r];
        if (strcmp(rd->op, ">&") == 0 && rd->fd == 2 && strcmp(rd->target, "1") == 0) {
            dup_err_to_out = 1;
        }
    }
    if (dup_err_to_out) {
        buf_put(&out, err.data ? err.data : "", err.len);
        buf_free(&err);
        buf_init(&err);
    }

    /* Route stdout and stderr: to a file if redirected, else to our streams. */
    int out_to_file = 0;
    int err_to_file = 0;
    for (int r = 0; r < a->nredirs; r++) {
        Redir *rd = &a->redirs[r];
        if (rd->op[0] != '>' || rd->op[1] == '&') {
            continue;
        }
        char *target = expand_one(c, rd->target);
        if (rd->fd == 1) {
            write_redirect_file(c, target, rd->op[1] == '>', &out);
            out_to_file = 1;
        } else if (rd->fd == 2) {
            write_redirect_file(c, target, rd->op[1] == '>', &err);
            err_to_file = 1;
        }
        free(target);
    }
    if (!out_to_file && out.len > 0) {
        buf_put(c->out, out.data, out.len);
    }
    if (!err_to_file && err.len > 0) {
        buf_put(c->err, err.data, err.len);
    }
    buf_free(&out);
    buf_free(&err);
    c->s->status = st;

cleanup:
    buf_free(&inbuf);
    for (int k = 0; k < nassign; k++) {
        if (old[k] != NULL) {
            session_setvar(c->s, anames[k], old[k]);
            free(old[k]);
        } else {
            session_unsetvar(c->s, anames[k]);
        }
        free(anames[k]);
        free(avals[k]);
    }
    free(old);
    free(anames);
    free(avals);
    strs_free(&argv);
    return c->s->status;
}

static int exec_seq(Ctx *c, Ast *a)
{
    int st = 0;
    for (int i = 0; i < a->nkids; i++) {
        if (c->abort || c->exiting) {
            break;
        }
        st = exec_node(c, a->kids[i]);
        c->s->status = st;
    }
    return st;
}

static int exec_pipe(Ctx *c, Ast *a)
{
    Buf prev;
    buf_init(&prev);
    int st = 0;
    for (int i = 0; i < a->nkids; i++) {
        int last = i == a->nkids - 1;
        Ctx sub = *c;
        Buf next;
        buf_init(&next);
        sub.in = i == 0 ? c->in : &prev;
        sub.out = last ? c->out : &next;
        st = exec_node(&sub, a->kids[i]);
        c->abort = c->abort || sub.abort;
        if (c->abort) {
            buf_free(&next);
            break;
        }
        buf_free(&prev);
        prev = next;
    }
    buf_free(&prev);
    return st;
}

static int exec_node(Ctx *c, Ast *a)
{
    switch (a->kind) {
    case AST_SIMPLE:
        return exec_simple(c, a);
    case AST_SEQ:
        return exec_seq(c, a);
    case AST_PIPE:
        return exec_pipe(c, a);
    case AST_AND: {
        int st = exec_node(c, a->a);
        c->s->status = st;
        if (!c->abort && !c->exiting && st == 0) {
            st = exec_node(c, a->b);
        }
        return st;
    }
    case AST_OR: {
        int st = exec_node(c, a->a);
        c->s->status = st;
        if (!c->abort && !c->exiting && st != 0) {
            st = exec_node(c, a->b);
        }
        return st;
    }
    case AST_IF: {
        int st = exec_node(c, a->a);
        if (c->abort || c->exiting) {
            return st;
        }
        if (st == 0) {
            return a->b ? exec_node(c, a->b) : 0;
        }
        return a->c ? exec_node(c, a->c) : 0;
    }
    case AST_WHILE:
    case AST_UNTIL: {
        int st = 0;
        for (int iter = 0; iter < LOOP_LIMIT; iter++) {
            int cond = exec_node(c, a->a);
            if (c->abort || c->exiting) {
                break;
            }
            int go = a->kind == AST_WHILE ? cond == 0 : cond != 0;
            if (!go) {
                break;
            }
            st = a->b ? exec_node(c, a->b) : 0;
            if (c->abort || c->exiting) {
                break;
            }
            if (iter == LOOP_LIMIT - 1) {
                buf_str(c->err, "sh: loop limit reached\n");
                st = 1;
            }
        }
        return st;
    }
    case AST_FOR: {
        Strs items = {NULL, 0, 0};
        if (a->words_is_all_args) {
            for (int k = 0; c->fr != NULL && k < c->fr->argc; k++) {
                strs_push(&items, nr_strdup(c->fr->argv[k]));
            }
        } else {
            expand_list(c, a->words, a->nwords, &items);
        }
        int st = 0;
        for (int k = 0; k < items.n; k++) {
            if (c->abort || c->exiting) {
                break;
            }
            session_setvar(c->s, a->var, items.v[k]);
            st = a->b ? exec_node(c, a->b) : 0;
        }
        strs_free(&items);
        return st;
    }
    }
    return 0;
}

int shell_run(Ctx *c, const char *src)
{
    Lex l;
    memset(&l, 0, sizeof l);
    if (lex(src, &l) != 0) {
        buf_fmt(c->err, "sh: syntax error: %s\n", l.err);
        c->s->status = 2;
        for (int k = 0; k < l.n; k++) {
            free(l.t[k].text);
        }
        free(l.t);
        return 2;
    }

    Parser p;
    memset(&p, 0, sizeof p);
    p.t = l.t;
    p.n = l.n;
    Ast *prog = p_seq(&p, NULL);
    if (!p.failed && p_cur(&p)->type != TK_EOF) {
        p_fail(&p, "syntax error near unexpected token");
    }

    int st;
    if (p.failed || prog == NULL) {
        buf_fmt(c->err, "sh: syntax error: %s\n", p.err);
        c->s->status = 2;
        st = 2;
    } else {
        st = exec_node(c, prog);
        c->s->status = st;
    }

    ast_free(prog);
    for (int k = 0; k < l.n; k++) {
        free(l.t[k].text);
    }
    free(l.t);
    return st;
}
