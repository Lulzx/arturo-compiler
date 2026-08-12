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
    V_NULL, V_INT, V_FLOAT, V_RATIONAL, V_COMPLEX, V_QUANTITY, V_UNIT, V_DATE, V_COLOR, V_BINARY, V_STR, V_CHAR, V_BOOL,
    V_BLOCK, V_DICT, V_FUNC, V_BUILTIN, V_RANGE, V_PATH,
    V_WORD, V_LABEL, V_LITERAL, V_SYMBOL, V_SYMBOLLITERAL, V_TYPE, V_VERSION, V_ERRORKIND, V_INLINE,
    V_PATHLABEL, V_PATHLITERAL, V_REGEX, V_ATTRIBUTE, V_ATTRIBUTELABEL,
    V_ERROR
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
        struct { long num; long den; } rational; /* V_RATIONAL */
        struct { double real; double imag; } complex; /* V_COMPLEX */
        struct { double amount; char *unit; int integral; } quantity; /* V_QUANTITY */
        long long   epoch;      /* V_DATE: Unix timestamp */
        unsigned int rgba;      /* V_COLOR: 0xRRGGBBAA */
        struct { unsigned char *data; size_t len; } binary; /* V_BINARY */
        struct { char *message; char *kind; } error; /* V_ERROR */
        char       *s;          /* V_STR  */
        char        c;          /* V_CHAR */
        int         b;          /* V_BOOL */
        struct { long lo; long hi; long step; int character; int infinite; } range;   /* V_RANGE */
        struct { char **segs; int nsegs; Value *segv; } path;  /* V_PATH (segv=segment Values for to :block) */
        struct { Block *b; } block;    /* V_BLOCK — shared body, see Block above */
        Dict       *dict;       /* V_DICT */
        struct { IR *params; IR **body; int nbody; Env *closure; int constructor; char **exports; int nexports; } fn; /* V_FUNC */
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
Value v_float_text(const char *text);
Value v_rational(long numerator, long denominator);
Value v_complex(double real, double imaginary);
Value v_quantity(double amount, const char *unit);
Value v_quantity_int(long amount, const char *unit);
Value v_unit(const char *unit);
Value v_date_iso(const char *iso8601);
Value v_color_hex(const char *hex);
Value v_binary_text(const char *text);
Value v_str(const char *s);          /* copies */
Value v_char(char c);
Value v_bool(int b);
Value v_error(const char *message);
Value v_version(const char *version);
Value v_errorkind(const char *name);
Value v_block(Value **items, int n);
Value v_dict(char **keys, Value *vals, int n);
Value v_range(long lo, long hi);
Value v_path(char **segs, int n);
Value v_pathv(Value *segv, int n);   /* path built from segment Values (lexer) */
Value v_token(VKind k, const char *s);  /* word/label/literal/symbol/type/... */
/* lex an Arturo source string into a block of tokens (the `to :block` lexer) */
Value lex_source(const char *s);
/* first 1-based source line of the flat element with the given index (the
 * block-as-code order that `to :block` produces); 0 when unknown */
int runtime_line_of(const char *src, int index);
Value v_func(IR *params, IR **body, int nbody, Env *closure);

/* value access / printing */
int  v_truthy(Value v);
void v_print(Value v);               /* Arturo's `print`: value + newline */
void rt_write_float(double f);        /* Arturo-compatible float text, no newline */
void rt_print_float(double f);        /* Arturo-compatible float text + newline */

/* ---- environments (frames with a parent chain) ----------------------- */
struct Env { char **names; Value *vals; int n; Env *parent; int rebind_parent; };
Env *env_new(Env *parent);
Value env_get(Env *e, const char *name);   /* returns V_NULL if unbound */
int   env_bound(Env *e, const char *name);
void  env_set(Env *e, const char *name, Value v);

/* ---- IR tree ---------------------------------------------------------- */
/* an IR node mirrors the compiler's op-tagged dict: one op, a name, a
 * constant value, an optional fn + args list. All fields are populated by
 * the builders; runNode reads only what its op needs. */
struct IR {
    const char *op;      /* "const","load","define","function","dictionary","if","do",
                            "block","call","return","while","until","loop","range",
                            "passthrough","intrinsic" */
    const char *name;    /* load/define/intrinsic */
    Value       v;       /* const / passthrough */
    IR         *fn;      /* call callee */
    IR        **args;    /* construct args */
    int         nargs;
    const char **attr_names; /* call attributes, in source order */
    IR         **attr_values;
    int          nattrs;
};

IR *ir_const(Value v);
IR *ir_load(const char *name);
IR *ir_intrinsic(const char *name);
IR *ir_word(const char *name);   /* bare word in value position: load-if-bound, else call zero-arity builtin */
IR *ir_define(const char *name, IR *expr);
IR *ir_let(const char *name, IR *expr);
IR *ir_call(IR *fn, IR **args, int n);
IR *ir_call_attrs(IR *fn, IR **args, int n, const char **names, IR **values, int nattrs);
IR *ir_attrs(IR *node, const char **names, IR **values, int nattrs);
IR *ir_passthrough(Value src);
IR *ir_block(IR **items, int n);
IR *ir_fn(IR *params, IR **body, int n);       /* function */
IR *ir_op(const char *op, IR **args, int n);   /* if/do/return/while/until */
IR *ir_op_attrs(const char *op, IR **args, int n, const char **names, IR **values, int nattrs);
IR *ir_seq(IR **items, int n);                 /* internal __seq of statements */

/* ---- evaluation -------------------------------------------------------- */
void runtime_set_args(int argc, char **argv);
void runtime_set_source(const char *path);
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
