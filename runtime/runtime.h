/* runtime.h — the native runtime for the compiler's C backend.
 *
 * The compiler translates a program's IR into a small C file that builds
 * the IR tree with the builders here and runs it with runSeq. This runtime
 * implements Arturo's dynamic value model + the builtins the compiler emits,
 * so a linked binary runs the program with no dependency on the Arturo VM.
 *
 * This is compiled ONCE to runtime.a; the per-program step is gcc + link.
 */
#ifndef ARTURO_RUNTIME_H
#define ARTURO_RUNTIME_H

#include <stddef.h>

/* ---- values ---------------------------------------------------------- */
typedef enum {
    V_NULL, V_INT, V_FLOAT, V_STR, V_CHAR, V_BOOL,
    V_BLOCK, V_DICT, V_FUNC, V_BUILTIN, V_RANGE, V_PATH,
    V_WORD, V_LABEL, V_LITERAL, V_SYMBOL, V_TYPE, V_INLINE,
    V_PATHLABEL, V_PATHLITERAL, V_REGEX, V_ATTRIBUTE, V_ATTRIBUTELABEL
} VKind;

typedef struct Value Value;
typedef struct IR IR;
typedef struct Env Env;
typedef struct Dict Dict;   /* shared, heap-allocated dictionary body */
typedef struct Block Block; /* shared, heap-allocated block body (Arturo ref type) */

/* a block is a heap body referenced by every copy of its value, so an
 * `insert`/`append`/`pop` through one copy is visible through all of them —
 * Arturo ref semantics (the compiler's addItem relies on this). */
struct Block {
    Value **items;
    int n;
    int cap;
};

struct Value {
    VKind k;
    union {
        long        i;          /* V_INT */
        double      f;          /* V_FLOAT */
        char       *s;          /* V_STR  */
        char        c;          /* V_CHAR */
        int         b;          /* V_BOOL */
        struct { long lo; long hi; } range;   /* V_RANGE */
        struct { char **segs; int nsegs; Value *segv; } path;  /* V_PATH (segv=segment Values for to :block) */
        struct { Block *b; } block;    /* V_BLOCK — shared body, see Block above */
        Dict       *dict;       /* V_DICT */
        struct { IR *params; IR **body; int nbody; Env *closure; } fn; /* V_FUNC */
    } u;
};

/* a dictionary is a heap body referenced by every copy of its value, so a
 * `set` through one copy is visible through all of them (Arturo ref types) */
struct Dict {
    char **keys;
    Value *vals;
    int n;
};

/* value constructors */
Value v_null(void);
Value v_int(long i);
Value v_float(double f);
Value v_str(const char *s);          /* copies */
Value v_char(char c);
Value v_bool(int b);
Value v_block(Value **items, int n);
Value v_dict(char **keys, Value *vals, int n);
Value v_range(long lo, long hi);
Value v_path(char **segs, int n);
Value v_pathv(Value *segv, int n);   /* path built from segment Values (lexer) */
Value v_token(VKind k, const char *s);  /* word/label/literal/symbol/type/... */
/* lex an Arturo source string into a block of tokens (the `to :block` lexer) */
Value lex_source(const char *s);
Value v_func(IR *params, IR **body, int nbody, Env *closure);

/* value access / printing */
int  v_truthy(Value v);
void v_print(Value v);               /* Arturo's `print`: value + newline */

/* ---- environments (frames with a parent chain) ----------------------- */
struct Env { char **names; Value *vals; int n; Env *parent; };
Env *env_new(Env *parent);
Value env_get(Env *e, const char *name);   /* returns V_NULL if unbound */
int   env_bound(Env *e, const char *name);
void  env_set(Env *e, const char *name, Value v);

/* ---- IR tree ---------------------------------------------------------- */
/* an IR node mirrors the compiler's op-tagged dict: one op, a name, a
 * constant value, an optional fn + args list. All fields are populated by
 * the builders; runNode reads only what its op needs. */
struct IR {
    const char *op;      /* "const","load","define","function","if","do",
                            "block","call","return","while","until",
                            "passthrough","intrinsic" */
    const char *name;    /* load/define/intrinsic */
    Value       v;       /* const / passthrough */
    IR         *fn;      /* call callee */
    IR        **args;    /* construct args */
    int         nargs;
};

IR *ir_const(Value v);
IR *ir_load(const char *name);
IR *ir_intrinsic(const char *name);
IR *ir_word(const char *name);   /* bare word in value position: load-if-bound, else call zero-arity builtin */
IR *ir_define(const char *name, IR *expr);
IR *ir_call(IR *fn, IR **args, int n);
IR *ir_passthrough(Value src);
IR *ir_block(IR **items, int n);
IR *ir_fn(IR *params, IR **body, int n);       /* function */
IR *ir_op(const char *op, IR **args, int n);   /* if/do/return/while/until */
IR *ir_seq(IR **items, int n);                 /* internal __seq of statements */

/* ---- evaluation -------------------------------------------------------- */
void runtime_set_args(int argc, char **argv);
Value runSeq(Env *e, IR **seq, int n);
Value runNode(Env *e, IR *node);

/* ---- builtins (dispatched by name from the runtime) ------------------- */
/* returns 1 if the name is a runtime builtin (result left in *out), else 0 */
int rt_builtin(const char *name, Env *e, Value *args, int n, Value *out);
/* returns 1 if name names a builtin (without evaluating it) */
int rt_builtin_known(const char *name);

/* ---- errors / diagnostics ---------------------------------------------- */
void rt_error(const char *msg);

#endif
