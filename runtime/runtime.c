/* runtime.c — native runtime for the compiler's C backend.
 *
 * Mirrors kernel.art's runNode/runSeq semantics so the compiler's IR, when
 * embedded in C and run here, behaves exactly as the donated VM runs it.
 * Compiled once to runtime.a; the per-program step is gcc + link.
 */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <setjmp.h>

/* ---- error handling ----------------------------------------------------- */
/* `try` uses setjmp/longjmp: die() unwinds to the nearest active try frame
 * instead of exiting when one is set. A linked stack of jmp_bufs lets nested
 * trys nest correctly. */
static jmp_buf *g_try_jmp = NULL;
static void die(const char *msg) {
    fprintf(stderr, "runtime error: %s\n", msg);
    if (g_try_jmp) longjmp(*g_try_jmp, 1);
    exit(1);
}
void rt_error(const char *msg) { die(msg); }

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { die("out of memory"); }
    return p;
}
static char *seg_text(Value v);
static Value v_block_cpy(Value **items, int n);
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { die("out of memory"); }
    return q;
}

/* ---- value constructors ------------------------------------------------- */
Value v_null(void)  { Value v; memset(&v,0,sizeof v); v.k=V_NULL;  return v; }
Value v_int(long i) { Value v; memset(&v,0,sizeof v); v.k=V_INT;   v.u.i=i; return v; }
Value v_float(double f){Value v; memset(&v,0,sizeof v); v.k=V_FLOAT; v.u.f=f; return v; }
Value v_bool(int b) { Value v; memset(&v,0,sizeof v); v.k=V_BOOL;  v.u.b=b; return v; }
Value v_char(char c){ Value v; memset(&v,0,sizeof v); v.k=V_CHAR;  v.u.c=c; return v; }
Value v_str(const char *s) {
    Value v; memset(&v,0,sizeof v); v.k=V_STR;
    v.u.s = (char*)xmalloc(strlen(s)+1); strcpy(v.u.s, s);
    return v;
}
Value v_block(Value **items, int n) {
    Value v; memset(&v,0,sizeof v); v.k=V_BLOCK;
    v.u.block.items = items; v.u.block.n = n; return v;
}
Value v_dict(char **keys, Value *vals, int n) {
    Value v; memset(&v,0,sizeof v); v.k=V_DICT;
    Dict *d=(Dict*)xmalloc(sizeof *d);
    d->keys=keys; d->vals=vals; d->n=n;
    v.u.dict=d; return v;
}
Value v_func(IR *params, IR **body, int nbody, Env *closure) {
    Value v; memset(&v,0,sizeof v); v.k=V_FUNC;
    v.u.fn.params=params; v.u.fn.body=body; v.u.fn.nbody=nbody; v.u.fn.closure=closure;
    return v;
}
Value v_range(long lo, long hi) {
    Value v; memset(&v,0,sizeof v); v.k=V_RANGE; v.u.range.lo=lo; v.u.range.hi=hi; return v;
}
Value v_path(char **segs, int n) {
    Value v; memset(&v,0,sizeof v); v.k=V_PATH; v.u.path.segs=segs; v.u.path.nsegs=n; v.u.path.segv=NULL; return v;
}
Value v_pathv(Value *segv, int n) {
    /* build the string segs (for runtime path ops) from the segment Values
     * (for `to :block` reconstruction). Each seg is a word/literal/int/str. */
    Value v; memset(&v,0,sizeof v); v.k=V_PATH; v.u.path.segv=segv; v.u.path.nsegs=n;
    char **ss=(char**)xmalloc(n*sizeof(char*));
    for(int i=0;i<n;i++) ss[i]=seg_text(segv[i]);
    v.u.path.segs=ss;
    return v;
}
Value v_token(VKind k, const char *s) {
    Value v; memset(&v,0,sizeof v); v.k=k;
    v.u.s=(char*)xmalloc(strlen(s)+1); strcpy(v.u.s,s);
    return v;
}
/* the string form of a path segment value (for runtime path ops). */
static char *seg_text(Value v){
    if(v.k==V_INT){ char b[64]; snprintf(b,sizeof b,"%ld",v.u.i); return strdup(b); }
    if(v.k==V_FLOAT){ char b[64]; snprintf(b,sizeof b,"%g",v.u.f); return strdup(b); }
    if(v.k==V_STR||v.k==V_WORD||v.k==V_LABEL||v.k==V_LITERAL||v.k==V_SYMBOL||v.k==V_TYPE
       ||v.k==V_ATTRIBUTE||v.k==V_ATTRIBUTELABEL) return strdup(v.u.s?v.u.s:"");
    return strdup("");
}

int v_truthy(Value v) {
    if (v.k==V_NULL) return 0;
    if (v.k==V_BOOL) return v.u.b;
    return 1;
}

/* Arturo print: print the value, then a newline. Strings print raw. A block
 * prints its elements space-separated WITHOUT outer brackets; a block nested
 * inside a printed block keeps its brackets (matching the host). */
static void print_scalar(Value v) {
    switch (v.k) {
        case V_NULL:  printf("null"); break;
        case V_INT:   printf("%ld", v.u.i); break;
        case V_FLOAT: printf("%g", v.u.f); break;
        case V_STR:   fwrite(v.u.s,1,strlen(v.u.s),stdout); break;
        case V_CHAR:  putchar(v.u.c); break;
        case V_BOOL:  printf(v.u.b ? "true" : "false"); break;
        case V_DICT:
            printf("#[");
            for (int i=0;i<v.u.dict->n;i++){ if(i)printf(" "); printf("%s:",v.u.dict->keys[i]); print_scalar(v.u.dict->vals[i]); }
            printf("]");
            break;
        case V_FUNC: printf("function"); break;
        case V_BUILTIN: printf("%s", v.u.s); break;
        case V_RANGE: printf("%ld..%ld", v.u.range.lo, v.u.range.hi); break;
        case V_PATH:
            for (int i=0;i<v.u.path.nsegs;i++){ if(i)printf("\\"); printf("%s",v.u.path.segs[i]); }
            break;
        default: break;
    }
}
/* a value as it appears nested inside a printed block: blocks get brackets */
static void print_embedded(Value v) {
    if (v.k == V_BLOCK) {
        printf("[");
        for (int i=0;i<v.u.block.n;i++){ if(i)printf(" "); print_embedded(*v.u.block.items[i]); }
        printf("]");
    } else {
        print_scalar(v);
    }
}
void v_print(Value v) {
    if (v.k == V_BLOCK) {
        /* host prints each element followed by a space (trailing space kept) */
        for (int i=0;i<v.u.block.n;i++){ print_embedded(*v.u.block.items[i]); printf(" "); }
    } else {
        print_scalar(v);
    }
    printf("\n");
}

/* ---- environments -------------------------------------------------------- */
Env *env_new(Env *parent) {
    Env *e = (Env*)xmalloc(sizeof *e);
    e->names=NULL; e->vals=NULL; e->n=0; e->parent=parent; return e;
}
static int env_find(Env *e, const char *name) {
    for (int i=0;i<e->n;i++) if (!strcmp(e->names[i], name)) return i;
    return -1;
}
int env_bound(Env *e, const char *name) {
    for (Env *f=e;f;f=f->parent) if (env_find(f,name)>=0) return 1;
    return 0;
}
Value env_get(Env *e, const char *name) {
    for (Env *f=e;f;f=f->parent) { int i=env_find(f,name); if (i>=0) return f->vals[i]; }
    return v_null();
}
void env_set(Env *e, const char *name, Value v) {
    int i = env_find(e, name);
    if (i>=0) { e->vals[i]=v; return; }
    e->names = (char**)xrealloc(e->names, (e->n+1)*sizeof(char*));
    e->vals  = (Value*)xrealloc(e->vals, (e->n+1)*sizeof(Value));
    e->names[e->n] = (char*)xmalloc(strlen(name)+1); strcpy(e->names[e->n], name);
    e->vals[e->n] = v; e->n++;
}

/* ---- IR builders --------------------------------------------------------- */
static IR *ir_new(const char *op) {
    IR *n=(IR*)xmalloc(sizeof *n); memset(n,0,sizeof *n); n->op=op; return n;
}
IR *ir_const(Value v){ IR*n=ir_new("const"); n->v=v; return n; }
IR *ir_load(const char *name){ IR*n=ir_new("load"); n->name=name; return n; }
IR *ir_intrinsic(const char *name){ IR*n=ir_new("intrinsic"); n->name=name; return n; }
/* `ir_word` — a bare word in VALUE position (the IR's `[op:intrinsic name:X]`
 * node when it is NOT a call callee). On the host a bare word resolves var-first
 * (a bound parameter like `arity`/`env` loads its value), else a zero-arity
 * builtin (`args`, `break`) is called. */
IR *ir_word(const char *name){ IR*n=ir_new("word"); n->name=name; return n; }
IR *ir_define(const char *name, IR *expr){ IR*n=ir_new("define"); n->name=name; n->args=(IR**)xmalloc(sizeof(IR*)); n->args[0]=expr; n->nargs=1; return n; }
IR *ir_call(IR *fn, IR **args, int n){ IR*x=ir_new("call"); x->fn=fn; x->args=args; x->nargs=n; return x; }
IR *ir_passthrough(Value src){ IR*n=ir_new("passthrough"); n->v=src; return n; }
IR *ir_block(IR **items, int n){ IR*x=ir_new("block"); x->args=items; x->nargs=n; return x; }
IR *ir_fn(IR *params, IR **body, int n){ IR*x=ir_new("function"); x->args=(IR**)xmalloc((n+1)*sizeof(IR*)); x->args[0]=params; for(int i=0;i<n;i++)x->args[i+1]=body[i]; x->nargs=n+1; return x; }
IR *ir_op(const char *op, IR **args, int n){ IR*x=ir_new(op); x->args=args; x->nargs=n; return x; }
/* a seq: internal __seq node wrapping a list of statements */
IR *ir_seq(IR **items, int n){ IR*x=ir_new("__seq"); x->args=items; x->nargs=n; return x; }

/* ---- return / break signals ------------------------------------------------------- */
static int   rt_ret_set = 0;
static Value rt_ret_val;
static int   rt_brk_set = 0;   /* `break` inside a loop body */

/* ---- apply a function value to evaluated args ----------------------------- */
static Value applyFunc(Env *caller, Value fn, Value *argv, int n) {
    if (fn.k==V_BUILTIN) {   /* a host builtin reached as a value */
        Value out;
        if (rt_builtin(fn.u.s, caller, argv, n, &out)) return out;
        die("unknown builtin"); return v_null();
    }
    Env *child = env_new(fn.u.fn.closure);
    IR *params = fn.u.fn.params;   /* a block of name words (IR) or NULL */
    if (params && params->op && !strcmp(params->op,"block")) {
        for (int i=0;i<params->nargs;i++){
            IR *p = params->args[i];
            const char *nm = NULL;
            if (p->op && !strcmp(p->op,"load")) nm=p->name;
            if (nm && i<n) env_set(child, nm, argv[i]);
        }
    }
    rt_ret_set=0;
    Value r = runSeq(child, fn.u.fn.body, fn.u.fn.nbody);
    if (rt_ret_set) { rt_ret_set=0; return rt_ret_val; }
    return r;
}

/* ---- builtins ------------------------------------------------------------- */

/* numeric/string/char coercion helpers */
static long as_int(Value v) {
    if (v.k==V_INT) return v.u.i;
    if (v.k==V_FLOAT) return (long)v.u.f;
    die("expected number"); return 0;
}
static double as_float(Value v) {
    if (v.k==V_FLOAT) return v.u.f;
    if (v.k==V_INT) return (double)v.u.i;
    die("expected number"); return 0;
}
static Value num2(long i){ return v_int(i); }
static Value numf(double f){ return v_float(f); }

static Value b_add(Env*e,Value*a,int n){ if(a[0].k==V_FLOAT||a[1].k==V_FLOAT)return numf(as_float(a[0])+as_float(a[1])); return num2(as_int(a[0])+as_int(a[1])); }
static Value b_sub(Env*e,Value*a,int n){ if(a[0].k==V_FLOAT||a[1].k==V_FLOAT)return numf(as_float(a[0])-as_float(a[1])); return num2(as_int(a[0])-as_int(a[1])); }
static Value b_mul(Env*e,Value*a,int n){ if(a[0].k==V_FLOAT||a[1].k==V_FLOAT)return numf(as_float(a[0])*as_float(a[1])); return num2(as_int(a[0])*as_int(a[1])); }
static Value b_div(Env*e,Value*a,int n){ return num2(as_int(a[0])/as_int(a[1])); }
static Value b_fdiv(Env*e,Value*a,int n){ return numf(as_float(a[0])/as_float(a[1])); }
static Value b_mod(Env*e,Value*a,int n){ return num2(as_int(a[0])%as_int(a[1])); }
static Value b_pow(Env*e,Value*a,int n){ return numf(pow(as_float(a[0]),as_float(a[1]))); }
static Value b_neg(Env*e,Value*a,int n){ if(a[0].k==V_FLOAT)return numf(-a[0].u.f); return num2(-as_int(a[0])); }
static Value b_inc(Env*e,Value*a,int n){ return num2(as_int(a[0])+1); }
static Value b_dec(Env*e,Value*a,int n){ return num2(as_int(a[0])-1); }
static Value b_print(Env*e,Value*a,int n){ v_print(a[0]); return v_null(); }
static Value b_equal(Env*e,Value*a,int n){
    if (a[0].k==V_INT && a[1].k==V_INT) return v_bool(a[0].u.i==a[1].u.i);
    if (a[0].k==V_STR  && a[1].k==V_STR) return v_bool(!strcmp(a[0].u.s,a[1].u.s));
    if (a[0].k==V_BOOL && a[1].k==V_BOOL) return v_bool(a[0].u.b==a[1].u.b);
    if (a[0].k==V_FLOAT && (a[1].k==V_FLOAT||a[1].k==V_INT)) return v_bool(as_float(a[0])==as_float(a[1]));
    if ((a[0].k==V_INT||a[0].k==V_FLOAT) && a[1].k==V_FLOAT) return v_bool(as_float(a[0])==as_float(a[1]));
    return v_bool(0);
}
static Value b_notEqual(Env*e,Value*a,int n){
    if (a[0].k==V_INT && a[1].k==V_INT) return v_bool(a[0].u.i!=a[1].u.i);
    if (a[0].k==V_STR  && a[1].k==V_STR) return v_bool(strcmp(a[0].u.s,a[1].u.s)!=0);
    if (a[0].k==V_BOOL && a[1].k==V_BOOL) return v_bool(a[0].u.b!=a[1].u.b);
    if ((a[0].k==V_INT||a[0].k==V_FLOAT) && (a[1].k==V_INT||a[1].k==V_FLOAT)) return v_bool(as_float(a[0])!=as_float(a[1]));
    if (a[0].k!=a[1].k) return v_bool(1);
    return v_bool(0);
}
static const char *type_name(Value v){
    switch(v.k){
        case V_NULL: return "null"; case V_INT: return "integer"; case V_FLOAT: return "floating";
        case V_STR: return "string"; case V_CHAR: return "char"; case V_BOOL: return "logical";
        case V_BLOCK: return "block"; case V_DICT: return "dictionary"; case V_FUNC: return "function";
        case V_BUILTIN: return "function"; case V_RANGE: return "range"; case V_PATH: return "path";
        case V_WORD: return "word"; case V_LABEL: return "label"; case V_LITERAL: return "literal";
        case V_SYMBOL: return "symbol"; case V_TYPE: return "type"; case V_INLINE: return "inline";
        case V_PATHLABEL: return "pathlabel"; case V_PATHLITERAL: return "pathliteral"; case V_REGEX: return "regex";
        case V_ATTRIBUTE: return "attribute"; case V_ATTRIBUTELABEL: return "attributelabel";
    }
    return "null";
}
static Value b_type(Env*e,Value*a,int n){
    char buf[64]; sprintf(buf,":%s",type_name(a[0]));
    return v_str(buf);
}
static Value b_greater(Env*e,Value*a,int n){ return v_bool(as_float(a[0])>as_float(a[1])); }
static Value b_less(Env*e,Value*a,int n){ return v_bool(as_float(a[0])<as_float(a[1])); }
static Value b_and(Env*e,Value*a,int n){ return v_bool(v_truthy(a[0])&&v_truthy(a[1])); }
static Value b_or(Env*e,Value*a,int n){ return v_bool(v_truthy(a[0])||v_truthy(a[1])); }
static Value b_not(Env*e,Value*a,int n){ return v_bool(!v_truthy(a[0])); }
static Value b_size(Env*e,Value*a,int n){
    Value v=a[0];
    if (v.k==V_BLOCK) return num2(v.u.block.n);
    if (v.k==V_STR) return num2((long)strlen(v.u.s));
    if (v.k==V_DICT) return num2(v.u.dict->n);
    if (v.k==V_RANGE) return num2(v.u.range.hi - v.u.range.lo + 1);
    if (v.k==V_NULL) return num2(0);
    die("size: unsupported"); return v_null();
}
static Value b_first(Env*e,Value*a,int n){
    Value v=a[0];
    if (v.k==V_BLOCK) return v.u.block.n? *v.u.block.items[0] : v_null();
    if (v.k==V_STR) return v_str((char[]){v.u.s[0],0});
    die("first: unsupported"); return v_null();
}
static Value b_last(Env*e,Value*a,int n){
    Value v=a[0];
    if (v.k==V_BLOCK) return v.u.block.n? *v.u.block.items[v.u.block.n-1] : v_null();
    die("last: unsupported"); return v_null();
}
static int dict_find(Value d, const char *k);
static Value applyAction(Env *parent, Value params, Value action, Value el);
static Value evalSeq(Env *e, Value **it, int n);
static Value evalExpr(Env *e, Value **it, int n, int *ip);
static int  path_is_dyn(Value v);
static int  starts_stmt(Value v);
static char *val_str(Value v);
static int  value_eq(Value a, Value b);

/* resolve a pathliteral `'a\b\c` to the container dict holding the final
 * segment plus that segment's index, so a mutating builtin can write back.
 * segs[0] is a variable name in the env; the rest are dict keys. */
static int path_target(Env *e, Value p, Value *cont, int *idx) {
    Value d = env_get(e, p.u.path.segs[0]);
    for (int i=1; i<p.u.path.nsegs-1; i++) {
        int j = dict_find(d, p.u.path.segs[i]);
        if (j < 0) return -1;
        d = d.u.dict->vals[j];
    }
    *idx = dict_find(d, p.u.path.segs[p.u.path.nsegs-1]);
    *cont = d;
    return *idx;
}
static Value block_append(Value v, Value el){
    Value **items=(Value**)xmalloc((v.u.block.n+1)*sizeof(Value*));
    for(int i=0;i<v.u.block.n;i++) items[i]=v.u.block.items[i];
    items[v.u.block.n]=(Value*)xmalloc(sizeof(Value)); items[v.u.block.n][0]=el;
    return v_block(items,v.u.block.n+1);
}
static Value b_append(Env*e,Value*a,int n){
    /* a path reference writes back through the resolved container */
    if (a[0].k==V_PATH){
        Value cont; int idx=path_target(e,a[0],&cont,&idx);
        if (idx<0) die("append: path not found");
        Value v=cont.u.dict->vals[idx];
        Value t[2]={v,a[1]};          /* recurse on the resolved value, not the path */
        Value r = b_append(e, t, 2);
        cont.u.dict->vals[idx]=r;
        return r;
    }
    Value v=a[0];
    if (v.k==V_BLOCK) return block_append(v,a[1]);
    if (v.k==V_STR){
        /* string append: concatenate the second arg (as text) onto the string */
        const char *s = v.u.s;
        char buf[64];
        const char *app;
        Value x = a[1];
        if (x.k==V_STR) app=x.u.s;
        else if (x.k==V_INT){ sprintf(buf,"%ld",x.u.i); app=buf; }
        else if (x.k==V_BOOL){ app=x.u.b?"true":"false"; }
        else if (x.k==V_CHAR){ buf[0]=x.u.c; buf[1]=0; app=buf; }
        else die("append: unsupported");
        char *out=(char*)xmalloc(strlen(s)+strlen(app)+1);
        strcpy(out,s); strcat(out,app);
        Value r=v_str(out); free(out); return r;
    }
    die("append: unsupported"); return v_null();
}
/* `insert dest index value` — insert `value` into block `dest` at `index`
 * (in place). `insert 'c (size c) v` is the compiler's addItem: appending a
 * single element without `append`'s block-flattening. */
static Value b_insert(Env*e,Value*a,int n){
    /* a one-segment path is a bare variable reference `'c`: write back to the
     * env var itself (the compiler's addItem uses `insert 'c (size c) v`). */
    if (a[0].k==V_PATH && a[0].u.path.nsegs==1){
        const char *vn=a[0].u.path.segs[0];
        Value t[3]={env_get(e,vn),a[1],a[2]};
        Value r=b_insert(e,t,3);
        env_set(e,vn,r);
        return r;
    }
    if (a[0].k==V_PATH){
        Value cont; int idx=path_target(e,a[0],&cont,&idx);
        if (idx<0) die("insert: path not found");
        Value v=cont.u.dict->vals[idx];
        Value t[3]={v,a[1],a[2]};
        Value r = b_insert(e, t, 3);
        cont.u.dict->vals[idx]=r;
        return r;
    }
    if (a[0].k!=V_BLOCK) die("insert: expected block");
    long at = as_int(a[1]);
    Value v=a[0];
    if (at<0) at=0; if (at>v.u.block.n) at=v.u.block.n;
    Value **items=(Value**)xmalloc((v.u.block.n+1)*sizeof(Value*));
    for(int i=0;i<at;i++) items[i]=v.u.block.items[i];
    items[at]=(Value*)xmalloc(sizeof(Value)); items[at][0]=a[2];
    for(int i=at;i<v.u.block.n;i++) items[i+1]=v.u.block.items[i];
    return v_block(items,v.u.block.n+1);
}
static Value b_pop(Env*e,Value*a,int n){
    /* pop the last element of a block, returning it and shrinking in place */
    if (a[0].k==V_PATH){
        Value cont; int idx=path_target(e,a[0],&cont,&idx);
        if (idx<0) die("pop: path not found");
        Value v=cont.u.dict->vals[idx];
        if (v.u.block.n==0) die("pop: empty block");
        Value r=*v.u.block.items[v.u.block.n-1];
        Value **items=(Value**)xmalloc((v.u.block.n-1)*sizeof(Value*));
        for(int i=0;i<v.u.block.n-1;i++) items[i]=v.u.block.items[i];
        cont.u.dict->vals[idx]=v_block(items,v.u.block.n-1);
        return r;
    }
    Value v=a[0];
    if (v.u.block.n==0) die("pop: empty block");
    Value r=*v.u.block.items[v.u.block.n-1];
    return r;
}
/* `++` concat: strings concatenate; blocks append */
static Value b_concat(Env*e,Value*a,int n){
    if (a[0].k==V_STR || a[1].k==V_STR) return b_append(e,a,n);
    return b_append(e,a,n);
}
static Value b_range(Env*e,Value*a,int n){
    return v_range(as_int(a[0]), as_int(a[1]));
}
static Value b_key(Env*e,Value*a,int n){ return v_bool(dict_find(a[0], a[1].u.s)>=0); }
static Value b_map(Env*e,Value*a,int n){
    Value coll=a[0], params=a[1], action=a[2];
    int cnt = coll.k==V_RANGE ? (int)(coll.u.range.hi-coll.u.range.lo+1) : coll.u.block.n;
    Value **items=(Value**)xmalloc((cnt+1)*sizeof(Value*));
    for(int i=0;i<cnt;i++){
        Value el = coll.k==V_RANGE ? v_int(coll.u.range.lo+i) : *coll.u.block.items[i];
        items[i]=(Value*)xmalloc(sizeof(Value)); items[i][0]=applyAction(e,params,action,el);
    }
    return v_block(items,cnt);
}
static Value b_select(Env*e,Value*a,int n){
    Value coll=a[0], params=a[1], pred=a[2];
    int cnt = coll.k==V_RANGE ? (int)(coll.u.range.hi-coll.u.range.lo+1) : coll.u.block.n;
    Value **items=(Value**)xmalloc((cnt+1)*sizeof(Value*));
    int m=0;
    for(int i=0;i<cnt;i++){
        Value el = coll.k==V_RANGE ? v_int(coll.u.range.lo+i) : *coll.u.block.items[i];
        if (v_truthy(applyAction(e,params,pred,el))){ items[m]=(Value*)xmalloc(sizeof(Value)); items[m][0]=el; m++; }
    }
    return v_block(items,m);
}
static Value b_loop(Env*e,Value*a,int n){
    Value coll=a[0], params=a[1], body=a[2];
    rt_brk_set=0;
    if (coll.k==V_DICT){
        /* `loop dict [k v]` binds key+value; `[v]` binds value */
        Dict *dd=coll.u.dict;
        for(int i=0;i<dd->n;i++){
            Env *child=env_new(e);
            if(params.k==V_BLOCK && params.u.block.n==1)
                env_set(child, (*params.u.block.items[0]).u.s, dd->vals[i]);
            else if(params.k==V_BLOCK && params.u.block.n>=2){
                env_set(child, (*params.u.block.items[0]).u.s, v_str(dd->keys[i]));
                env_set(child, (*params.u.block.items[1]).u.s, dd->vals[i]);
            }
            evalSeq(child, body.u.block.items, body.u.block.n);
            if (rt_brk_set) break;
        }
        rt_brk_set=0;
        return v_null();
    }
    int cnt = coll.k==V_RANGE ? (int)(coll.u.range.hi-coll.u.range.lo+1) : coll.u.block.n;
    for(int i=0;i<cnt;i++){
        Value el = coll.k==V_RANGE ? v_int(coll.u.range.lo+i) : *coll.u.block.items[i];
        applyAction(e,params,body,el);
        if (rt_brk_set) break;
    }
    rt_brk_set=0;
    return v_null();
}

/* `break` — exit the innermost loop. Sets a signal the enclosing while/until/
 * loop checks after each body evaluation. */
static Value b_break(Env*e,Value*a,int n){ rt_brk_set=1; return v_null(); }
/* `null? x` — true iff x is null */
static Value b_isNull(Env*e,Value*a,int n){ return v_bool(a[0].k==V_NULL); }
/* `contains? coll x` — membership for a block, substring for a string */
static Value b_contains(Env*e,Value*a,int n){
    Value c=a[0];
    if (c.k==V_BLOCK){
        for (int i=0;i<c.u.block.n;i++) if (value_eq(*c.u.block.items[i], a[1])) return v_bool(1);
        return v_bool(0);
    }
    if (c.k==V_STR){
        const char *h=c.u.s, *n=val_str(a[1]);
        return v_bool(strstr(h,n)!=NULL);
    }
    return v_bool(0);
}
/* `greaterOrEqual? a b` — a >= b (numeric or string compare) */
static Value b_ge(Env*e,Value*a,int n){
    if (a[0].k==V_STR) return v_bool(strcmp(a[0].u.s, a[1].u.s)>=0);
    return v_bool(as_float(a[0]) >= as_float(a[1]));
}
/* `join block` — concatenate a block's elements into one string (no separator) */
static Value b_join(Env*e,Value*a,int n){
    Value v=a[0];
    if (v.k!=V_BLOCK) { char *s=val_str(v); return v_str(s); }
    size_t cap=16,len=0; char *buf=xmalloc(cap); buf[0]=0;
    for (int i=0;i<v.u.block.n;i++){
        char *s=val_str(*v.u.block.items[i]);
        size_t t=strlen(s); if (len+t+1>cap){ cap=(len+t)*2; buf=xrealloc(buf,cap); }
        memcpy(buf+len,s,t); len+=t; free(s);
    }
    buf[len]=0; Value r=v_str(buf); free(buf); return r;
}
/* `try [blk]` — evaluate blk; on a runtime error return null instead of dying */
static Value b_try(Env*e,Value*a,int n){
    if (a[0].k!=V_BLOCK) return v_null();
    jmp_buf jb; jmp_buf *prev = g_try_jmp;
    g_try_jmp = &jb;
    if (setjmp(jb)==0){
        Value r = evalSeq(e, a[0].u.block.items, a[0].u.block.n);
        g_try_jmp = prev;
        return r;
    }
    g_try_jmp = prev;
    return v_null();
}

/* dict helpers */
static int dict_find(Value d, const char *k){
    for(int i=0;i<d.u.dict->n;i++) if(!strcmp(d.u.dict->keys[i],k)) return i;
    return -1;
}
static Value b_makeDict(Env*e,Value*a,int n){
    char **keys=(char**)xmalloc((n/2+1)*sizeof(char*));
    Value *vals=(Value*)xmalloc((n/2+1)*sizeof(Value));
    int m=0;
    for(int i=0;i+1<n;i+=2){
        keys[m]=(char*)xmalloc(strlen(a[i].u.s)+1); strcpy(keys[m],a[i].u.s);
        vals[m]=a[i+1]; m++;
    }
    return v_dict(keys,vals,m);
}
static Value b_set(Env*e,Value*a,int n){
    Value d=a[0]; const char *k=a[1].u.s; Value val=a[2];
    if (d.k!=V_DICT) die("set: expected dict");
    Dict *dd=d.u.dict;
    int i=dict_find(d,k);
    if (i>=0){ dd->vals[i]=val; return d; }
    dd->keys=(char**)xrealloc(dd->keys,(dd->n+1)*sizeof(char*));
    dd->vals=(Value*)xrealloc(dd->vals,(dd->n+1)*sizeof(Value));
    dd->keys[dd->n]=(char*)xmalloc(strlen(k)+1); strcpy(dd->keys[dd->n],k);
    dd->vals[dd->n]=val; dd->n++;
    return d;
}
static Value b_get(Env*e,Value*a,int n){
    /* integer index into a block: `c\stack\1` */
    if (a[1].k==V_INT && a[0].k==V_BLOCK){
        int i=(int)a[1].u.i;
        if (i<0 || i>=a[0].u.block.n) return v_null();
        return *a[0].u.block.items[i];
    }
    if (a[1].k==V_INT && a[0].k==V_RANGE){
        long i=a[1].u.i;
        return (i>=a[0].u.range.lo && i<=a[0].u.range.hi) ? v_int(i) : v_null();
    }
    Value d=a[0]; const char *k=a[1].u.s;
    if (d.k!=V_DICT){
        fprintf(stderr, "get: expected dict, got kind %d, key '%s'\n", d.k, k);
        die("get: expected dict");
    }
    int i=dict_find(d,k);
    return i>=0 ? d.u.dict->vals[i] : v_null();
}
static Value b_empty(Env*e,Value*a,int n){
    Value v=a[0];
    if (v.k==V_BLOCK) return v_bool(v.u.block.n==0);
    if (v.k==V_STR) return v_bool(strlen(v.u.s)==0);
    if (v.k==V_DICT) return v_bool(v.u.dict->n==0);
    return v_bool(0);
}
/* call: apply a function value to a block of evaluated args */
static Value b_call(Env*e,Value*a,int n){
    Value fn=a[0]; Value args=a[1];
    Value *argv=(Value*)xmalloc((args.u.block.n+1)*sizeof(Value));
    for(int i=0;i<args.u.block.n;i++) argv[i]=*args.u.block.items[i];
    return applyFunc(e,fn,argv,args.u.block.n);
}

/* ---- builtins the compiler's own source needs ---------------------------- */

/* host renders floats in fixed decimal (never scientific), with the shortest
 * round-trip digits and always at least one fractional digit (`0.0`,`3.5`,
 * `15000000000.0`,`0.00001`). */
static char *fstr(double f, char *out, size_t cap){
    if (f == (double)(long)f) {
        snprintf(out, cap, "%.0f", f);
        size_t l=strlen(out); if(l+2<cap){ out[l]='.'; out[l+1]='0'; out[l+2]=0; }
        return out;
    }
    int p;
    for (p=1; p<=17; p++){
        snprintf(out, cap, "%.*g", p, f);
        if (atof(out)==f) break;
    }
    char *e=strchr(out,'e'); if(!e) e=strchr(out,'E');
    if (e){
        int exp=atoi(e+1); *e=0;
        char mant[64]; strcpy(mant,out);
        char *dot=strchr(mant,'.');
        if(!dot){
            if(exp>=0){
                snprintf(out,cap,"%s",mant); size_t l=strlen(out);
                for(int i=0;i<exp;i++) if(l<cap-1) out[l++]='0';
                out[l]='.'; out[l+1]='0'; out[l+2]=0;
            } else {
                snprintf(out,cap,"0."); size_t l=2;
                for(int i=0;i<(-exp-1);i++) out[l++]='0';
                strcpy(out+l,mant);
            }
        } else {
            char dig[64]; int k=0; for(char*cp=mant;*cp;cp++) if(*cp!='.') dig[k++]=*cp; dig[k]=0;
            int intpart=(int)(dot-mant), pp=intpart+exp;
            if(pp<=0){
                snprintf(out,cap,"0."); size_t l=2;
                for(int i=0;i<(-pp);i++) out[l++]='0';
                strcpy(out+l,dig);
            } else if(pp>=k){
                snprintf(out,cap,"%s",dig); size_t l=k;
                for(int i=0;i<(pp-k);i++) out[l++]='0';
                out[l++]='.'; out[l++]='0'; out[l]=0;
            } else {
                strncpy(out,dig,pp); out[pp]='.'; strcpy(out+pp+1,dig+pp);
            }
        }
    }
    if(!strchr(out,'.')){ size_t l=strlen(out); out[l]='.'; out[l+1]='0'; out[l+2]=0; }
    return out;
}

/* render a value as a C string (Arturo's `to :string`). */
static char *val_str(Value v){
    char b[64];
    switch (v.k) {
        case V_INT:    snprintf(b,64,"%ld",v.u.i); return strdup(b);
        case V_FLOAT:  { char fb[64]; fstr(v.u.f,fb,sizeof fb); return strdup(fb); }
        case V_STR:    return strdup(v.u.s);
        case V_CHAR:   { char c[2]={v.u.c,0}; return strdup(c); }
        case V_BOOL:   return strdup(v.u.b?"true":"false");
        case V_NULL:   return strdup("null");
        case V_FUNC:   return strdup("function");
        case V_BUILTIN:return strdup(v.u.s);
        case V_RANGE:  snprintf(b,64,"%ld..%ld",v.u.range.lo,v.u.range.hi); return strdup(b);
        case V_BLOCK: {
            /* host `to :string` of a block wraps the elements in brackets */
            size_t cap=32, len=0; char *out=xmalloc(cap); out[0]=0;
            out[len++]='[';
            for(int i=0;i<v.u.block.n;i++){
                char *s=val_str(*v.u.block.items[i]); size_t need=len+strlen(s)+2;
                if(need>cap){ cap=need*2; out=xrealloc(out,cap); }
                if(i){ out[len++]=' '; } strcpy(out+len,s); len+=strlen(s); free(s);
            }
            out[len++]=']'; out[len]=0; return out;
        }
        case V_DICT: {
            size_t cap=32, len=0; char *out=xmalloc(cap); out[0]=0;
            strcpy(out,"#["); len=2;
            for(int i=0;i<v.u.dict->n;i++){
                char *s=val_str(v.u.dict->vals[i]);
                size_t need=len+strlen(v.u.dict->keys[i])+1+strlen(s)+1+1;
                if(need>cap){ cap=need*2; out=xrealloc(out,cap); }
                if(i){ out[len++]=' '; } sprintf(out+len,"%s:",v.u.dict->keys[i]);
                len+=strlen(v.u.dict->keys[i])+1; strcpy(out+len,s); len+=strlen(s); free(s);
            }
            out[len++]=']'; out[len]=0; return out;
        }
        case V_PATH:
        case V_PATHLABEL:
        case V_PATHLITERAL: {
            size_t cap=16, len=0; char *out=xmalloc(cap); out[0]=0;
            for(int i=0;i<v.u.path.nsegs;i++){
                size_t add=strlen(v.u.path.segs[i])+1; if(len+add>cap){cap=(len+add)*2;out=xrealloc(out,cap);}
                if(i) out[len++]='\\'; strcpy(out+len,v.u.path.segs[i]); len+=strlen(v.u.path.segs[i]);
            }
            out[len]=0; return out;
        }
        case V_WORD:
        case V_LABEL:
        case V_LITERAL:
        case V_SYMBOL:
        case V_TYPE:
        case V_REGEX:
        case V_ATTRIBUTE:
        case V_ATTRIBUTELABEL:
            return strdup(v.u.s?v.u.s:"");
        case V_INLINE: {
            /* an inline is a group: render its elements like a block body */
            size_t cap=16, len=0; char *out=xmalloc(cap); out[0]=0;
            for(int i=0;i<v.u.block.n;i++){
                char *s=val_str(*v.u.block.items[i]); size_t need=len+strlen(s)+2;
                if(need>cap){ cap=need*2; out=xrealloc(out,cap); }
                if(i){ out[len++]=' '; } strcpy(out+len,s); len+=strlen(s); free(s);
            }
            out[len]=0; return out;
        }
    }
    return strdup("null");
}

/* structural equality: numbers compare by value, everything else by string form
 * (blocks/dicts compare by their flattened rendering — good enough for the
 * compiler's membership checks on scalar items). */
static int value_eq(Value a, Value b){
    if (a.k==V_INT && b.k==V_INT) return a.u.i==b.u.i;
    if (a.k==V_INT || b.k==V_INT) return as_float(a)==as_float(b);
    if (a.k==V_STR && b.k==V_STR) return !strcmp(a.u.s,b.u.s);
    if (a.k==V_BOOL && b.k==V_BOOL) return a.u.b==b.u.b;
    if (a.k==V_NULL && b.k==V_NULL) return 1;
    if (a.k==V_CHAR && b.k==V_CHAR) return a.u.c==b.u.c;
    char *sa=val_str(a), *sb=val_str(b); int r=!strcmp(sa,sb); free(sa); free(sb); return r;
}

static Value one_elt(Value v);

/* `to :type value` — the workhorse conversion (237 call sites in the compiler). */
static Value b_to(Env*e,Value*a,int n){
    const char *ty=a[0].u.s; Value v=a[1];
    if(!strcmp(ty,":string")){ char *s=val_str(v); Value r=v_str(s); free(s); return r; }
    if(!strcmp(ty,":word")||!strcmp(ty,":label")||!strcmp(ty,":literal")||!strcmp(ty,":symbol"))
        { char *s=val_str(v); Value r=v_str(s); free(s); return r; }
    if(!strcmp(ty,":integer")){
        if(v.k==V_STR) return v_int(atol(v.u.s));
        if(v.k==V_FLOAT) return v_int((long)v.u.f);
        if(v.k==V_INT) return v;
        if(v.k==V_BOOL) return v_int(v.u.b?1:0);
        die("to :integer"); return v_null();
    }
    if(!strcmp(ty,":floating")){
        if(v.k==V_STR) return v_float(atof(v.u.s));
        if(v.k==V_INT) return v_float((double)v.u.i);
        if(v.k==V_FLOAT) return v;
        die("to :floating"); return v_null();
    }
    if(!strcmp(ty,":logical")){
        if(v.k==V_STR) return v_bool(!strcmp(v.u.s,"true")||!strcmp(v.u.s,"1"));
        if(v.k==V_INT) return v_bool(v.u.i!=0);
        return v_bool(v_truthy(v));
    }
    if(!strcmp(ty,":char")){
        if(v.k==V_STR) return v_char(v.u.s[0]);
        if(v.k==V_INT) return v_char((char)v.u.i);
        if(v.k==V_CHAR) return v;
        die("to :char"); return v_null();
    }
    if(!strcmp(ty,":block")){
        if(v.k==V_BLOCK) return v;
        if(v.k==V_INLINE) return v_block_cpy(v.u.block.items, v.u.block.n);
        if(v.k==V_STR) return lex_source(v.u.s);
        if(v.k==V_PATH||v.k==V_PATHLABEL||v.k==V_PATHLITERAL){
            /* decompose a path into [base seg1 seg2 ...] with segment VALUES */
            Value **it=(Value**)xmalloc(v.u.path.nsegs*sizeof(Value*));
            for(int i=0;i<v.u.path.nsegs;i++){
                it[i]=(Value*)xmalloc(sizeof(Value));
                if(v.u.path.segv) it[i][0]=v.u.path.segv[i];
                else { it[i][0]=v_token(V_LITERAL, v.u.path.segs[i]); }
            }
            return v_block(it, v.u.path.nsegs);
        }
        return one_elt(v);
    }
    if(!strcmp(ty,":dictionary")){
        if(v.k==V_DICT) return v;
        die("to :dictionary"); return v_null();
    }
    if(!strcmp(ty,":array")){ /* array is block-like */
        if(v.k==V_BLOCK) return v;
        return one_elt(v);
    }
    return v;
}
/* a block holding a single element. */
static Value one_elt(Value v){ Value **it=(Value**)xmalloc(sizeof(Value*)); it[0]=(Value*)xmalloc(sizeof(Value)); it[0][0]=v; return v_block(it,1); }
static Value v_block_cpy(Value **items, int n){
    Value **it=(Value**)xmalloc(n*sizeof(Value*));
    for(int i=0;i<n;i++){ it[i]=(Value*)xmalloc(sizeof(Value)); it[i][0]=items[i][0]; }
    return v_block(it,n);
}

/* `replace s from to` — replace all occurrences of `from` with `to`. */
static Value b_replace(Env*e,Value*a,int n){
    const char *s=a[0].u.s, *from=a[1].u.s, *to=a[2].u.s;
    if(!*from) return v_str(s);
    int cnt=0; for(const char*p=s;(p=strstr(p,from));p+=strlen(from)) cnt++;
    if(!cnt) return v_str(s);
    size_t fl=strlen(from), tl=strlen(to), sl=strlen(s);
    char *out=xmalloc(sl + cnt*(tl-fl) + 1);
    char *o=out;
    const char *p;
    while((p=strstr(s,from))){
        size_t pre=p-s; memcpy(o,s,pre); o+=pre; memcpy(o,to,tl); o+=tl;
        s=p+fl;
    }
    strcpy(o,s);
    Value r=v_str(out); free(out); return r;
}

/* `joinWith coll sep` — render coll's elements joined by sep. */
static Value b_joinWith(Env*e,Value*a,int n){
    Value coll=a[0]; const char *sep=(n>1 && a[1].k==V_STR)?a[1].u.s:"";
    if(coll.k==V_STR) return coll;
    if(coll.k!=V_BLOCK) die("joinWith: expected block");
    size_t total=1;
    for(int i=0;i<coll.u.block.n;i++){ char*s=val_str(*coll.u.block.items[i]); total+=strlen(s); free(s); if(i) total+=strlen(sep); }
    char *out=xmalloc(total+1); size_t len=0; out[0]=0;
    for(int i=0;i<coll.u.block.n;i++){
        char*s=val_str(*coll.u.block.items[i]);
        if(i){ strcpy(out+len,sep); len+=strlen(sep); }
        strcpy(out+len,s); len+=strlen(s); free(s);
    }
    out[len]=0; Value r=v_str(out); free(out); return r;
}

/* `fold coll init [acc x][ body ]` — left fold. */
static Value b_fold(Env*e,Value*a,int n){
    Value coll=a[0], init=a[1], params=a[2], action=a[3];
    int cnt=coll.k==V_RANGE?(int)(coll.u.range.hi-coll.u.range.lo+1):coll.u.block.n;
    Value acc=init;
    for(int i=0;i<cnt;i++){
        Value el=coll.k==V_RANGE?v_int(coll.u.range.lo+i):*coll.u.block.items[i];
        Env *child=env_new(e);
        if(params.k==V_BLOCK && params.u.block.n>=1) env_set(child,(*params.u.block.items[0]).u.s,acc);
        if(params.k==V_BLOCK && params.u.block.n>=2) env_set(child,(*params.u.block.items[1]).u.s,el);
        acc=evalSeq(child,action.u.block.items,action.u.block.n);
    }
    return acc;
}

static Value b_lower(Env*e,Value*a,int n){
    const char*s=a[0].u.s; char*out=strdup(s);
    for(char*p=out;*p;p++) *p=(char)tolower((unsigned char)*p);
    Value r=v_str(out); free(out); return r;
}
static Value b_upper(Env*e,Value*a,int n){
    const char*s=a[0].u.s; char*out=strdup(s);
    for(char*p=out;*p;p++) *p=(char)toupper((unsigned char)*p);
    Value r=v_str(out); free(out); return r;
}
/* `index coll needle` — 0-based index of first occurrence, -1 if absent. */
static Value b_index(Env*e,Value*a,int n){
    if(a[0].k==V_STR){
        const char *s=a[0].u.s, *nd=a[1].u.s;
        const char *p=strstr(s,nd);
        return v_int(p?(long)(p-s):-1);
    }
    if(a[0].k==V_BLOCK){
        Value **it=a[0].u.block.items; int m=a[0].u.block.n;
        for(int i=0;i<m;i++){ /* compare by value-str for simplicity */
            char *sv=val_str(*it[i]), *nv=val_str(a[1]);
            int eq=!strcmp(sv,nv); free(sv); free(nv);
            if(eq) return v_int(i);
        }
        return v_int(-1);
    }
    die("index"); return v_null();
}
/* `slice coll from to` — inclusive slice (Arturo semantics). */
static Value b_slice(Env*e,Value*a,int n){
    long from=as_int(a[1]), to=as_int(a[2]);
    if(a[0].k==V_BLOCK){
        int m=a[0].u.block.n;
        if(from<0) from=0; if(to>=m) to=m-1;
        int cnt=(int)(to-from+1); if(cnt<0) cnt=0;
        Value **items=(Value**)xmalloc((cnt+1)*sizeof(Value*));
        for(int i=0;i<cnt;i++) items[i]=a[0].u.block.items[from+i];
        return v_block(items,cnt);
    }
    if(a[0].k==V_STR){
        const char*s=a[0].u.s; long m=(long)strlen(s);
        if(from<0) from=0; if(to>=m) to=m-1;
        long cnt=to-from+1; if(cnt<0) cnt=0;
        char *out=xmalloc(cnt+1); memcpy(out,s+from,cnt); out[cnt]=0;
        Value r=v_str(out); free(out); return r;
    }
    die("slice"); return v_null();
}
/* `split s sep` — split a string into a block of substrings. */
static Value b_split(Env*e,Value*a,int n){
    const char *s=a[0].u.s, *sep=a[1].u.s;
    if(!*sep){ /* split into chars */
        int m=(int)strlen(s); Value **items=(Value**)xmalloc((m+1)*sizeof(Value*));
        for(int i=0;i<m;i++){ char c[2]={s[i],0}; items[i]=(Value*)xmalloc(sizeof(Value)); items[i][0]=v_str(c); }
        return v_block(items,m);
    }
    int cnt=1; for(const char*p=s; (p=strstr(p,sep)); p+=strlen(sep)) cnt++;
    Value **items=(Value**)xmalloc((cnt+1)*sizeof(Value*));
    int m=0; const char *start=s, *p;
    while((p=strstr(start,sep))){
        long len=p-start; char *tok=xmalloc(len+1); memcpy(tok,start,len); tok[len]=0;
        items[m]=(Value*)xmalloc(sizeof(Value)); items[m][0]=v_str(tok); free(tok); m++;
        start=p+strlen(sep);
    }
    items[m]=(Value*)xmalloc(sizeof(Value)); items[m][0]=v_str(start); m++;
    return v_block(items,m);
}
/* `take coll n` / `drop coll n` */
static Value b_take(Env*e,Value*a,int n){
    long c=as_int(a[1]);
    if(a[0].k==V_BLOCK){
        int m=a[0].u.block.n; if(c>m)c=m; if(c<0)c=0;
        Value **items=(Value**)xmalloc((c+1)*sizeof(Value*));
        for(int i=0;i<c;i++) items[i]=a[0].u.block.items[i];
        return v_block(items,c);
    }
    if(a[0].k==V_STR){
        const char*s=a[0].u.s; long m=(long)strlen(s); if(c>m)c=m; if(c<0)c=0;
        char *out=xmalloc(c+1); memcpy(out,s,c); out[c]=0;
        Value r=v_str(out); free(out); return r;
    }
    die("take"); return v_null();
}
static Value b_drop(Env*e,Value*a,int n){
    long c=as_int(a[1]);
    if(a[0].k==V_BLOCK){
        int m=a[0].u.block.n; if(c>m)c=m; if(c<0)c=0;
        int cnt=m-(int)c; Value **items=(Value**)xmalloc((cnt+1)*sizeof(Value*));
        for(int i=0;i<cnt;i++) items[i]=a[0].u.block.items[c+i];
        return v_block(items,cnt);
    }
    if(a[0].k==V_STR){
        const char*s=a[0].u.s; long m=(long)strlen(s); if(c>m)c=m; if(c<0)c=0;
        long cnt=m-c; char *out=xmalloc(cnt+1); memcpy(out,s+c,cnt); out[cnt]=0;
        Value r=v_str(out); free(out); return r;
    }
    die("drop"); return v_null();
}
static Value b_reverse(Env*e,Value*a,int n){
    if(a[0].k==V_STR){
        const char*s=a[0].u.s; long m=(long)strlen(s);
        char *out=xmalloc(m+1); for(long i=0;i<m;i++) out[i]=s[m-1-i]; out[m]=0;
        Value r=v_str(out); free(out); return r;
    }
    if(a[0].k==V_BLOCK){
        int m=a[0].u.block.n; Value **items=(Value**)xmalloc((m+1)*sizeof(Value*));
        for(int i=0;i<m;i++) items[i]=a[0].u.block.items[m-1-i];
        return v_block(items,m);
    }
    die("reverse"); return v_null();
}
/* `ensure pred msg` — assert; die on failure. */
static Value b_ensure(Env*e,Value*a,int n){
    if(!v_truthy(a[0])){ const char *m=(n>1&&a[1].k==V_STR)?a[1].u.s:"assertion failed"; die(m); }
    return a[0];
}
/* `array coll` — block to an array (block-like here). */
static Value b_array(Env*e,Value*a,int n){
    Value v=a[0];
    if(v.k==V_BLOCK) return v;
    return one_elt(v);
}
/* `case key [match1 -> result1, match2 -> result2, ...]` — the arms form a
 * FLAT block: each arm is `match "->" resultExpr...`, where resultExpr is the
 * tokens from after "->" until the next arm starts. The emitter wraps
 * multi-token results in a nested block, so a top-level token that is a
 * ":type" word or "else" begins a new arm (results never contain those). */
static Value b_case(Env*e,Value*a,int n){
    Value key=a[0]; Value pairs=a[1];
    if(pairs.k!=V_BLOCK) die("case: expected arms block");
    Value **it=pairs.u.block.items; int pn=pairs.u.block.n;
    int i=0;
    while(i<pn){
        /* match token */
        Value mv=*it[i];
        int arrow=i+1;
        if(arrow>=pn || it[arrow]->k!=V_STR || strcmp(it[arrow]->u.s,"->")) i++;
        int rs=arrow+1;               /* result start */
        int re=rs;                    /* result end (exclusive) */
        while(re<pn){
            Value t=*it[re];
            if(t.k==V_STR && (t.u.s[0]==':' || !strcmp(t.u.s,"else"))) break;
            re++;
        }
        int hit;
        if(mv.k==V_STR && !strcmp(mv.u.s,"else")) hit=1;
        else hit = v_truthy(b_equal(e,(Value[]){key,mv},2));   /* value equality */
        if(hit){
            if(re<=rs) return v_null();
            return evalSeq(e, it+rs, re-rs);   /* result is a small statement seq */
        }
        i = re;
    }
    return v_null();
}
/* `read path` — read a file as a string. */
static Value b_read(Env*e,Value*a,int n){
    const char *path=a[0].u.s;
    FILE *f=fopen(path,"rb");
    if(!f){ /* return empty on missing (bundle `read` does) */
        return v_str("");
    }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *buf=xmalloc(sz+1); fread(buf,1,sz,f); buf[sz]=0; fclose(f);
    Value r=v_str(buf); free(buf); return r;
}
/* `write content path` — content FIRST (Arturo's actual signature; the compiler
 * emits `write <content> <path>`). */
static Value b_write(Env*e,Value*a,int n){
    char *s=val_str(a[0]);
    const char *path=a[1].u.s;
    FILE *f=fopen(path,"wb");
    if(!f){ free(s); die("write: cannot open"); return v_null(); }
    fwrite(s,1,strlen(s),f); fclose(f); free(s);
    return v_null();
}
/* `execute cmd` — run a shell command and return its captured stdout. */
static Value b_execute(Env*e,Value*a,int n){
    char *s=val_str(a[0]);
    FILE *p=popen(s,"r"); free(s);
    if(!p) return v_str("");
    size_t cap=256, len=0; char *buf=xmalloc(cap); buf[0]=0;
    char tmp[256];
    while(fgets(tmp,sizeof tmp,p)){ size_t t=strlen(tmp); if(len+t+1>cap){cap=(len+t)*2;buf=xrealloc(buf,cap);} memcpy(buf+len,tmp,t); len+=t; }
    buf[len]=0; pclose(p);
    Value r=v_str(buf); free(buf); return r;
}
/* `args` — the command-line arguments as a block of strings. The generated
 * main() calls runtime_set_args() before running the program. */
static int  g_argc = 0;
static char **g_argv = NULL;
void runtime_set_args(int argc, char **argv){ g_argc = argc; g_argv = argv; }

static Value b_args(Env*e,Value*a,int n){
    /* Arturo's `args` is a dict `#[values: [...]]`; the compiler reads
     * `args\values\0`. */
    int c = g_argc>0 ? g_argc-1 : 0;              /* skip program name */
    Value **items = (Value**)xmalloc((c+1)*sizeof(Value*));
    for(int i=0;i<c;i++){ items[i]=(Value*)xmalloc(sizeof(Value)); items[i][0]=v_str(g_argv[i+1]); }
    Value vals = v_block(items, c);
    char **keys=(char**)xmalloc(sizeof(char*)); keys[0]=(char*)xmalloc(7); memcpy(keys[0],"values",7);
    Value *vv=(Value*)xmalloc(sizeof(Value)); vv[0]=vals;
    Value r=v_dict(keys, vv, 1);
    return r;
}

static struct { const char *name; Value (*fn)(Env*,Value*,int); } BUILTINS[] = {
    {"print",b_print},{"add",b_add},{"sub",b_sub},{"mul",b_mul},{"div",b_div},
    {"fdiv",b_fdiv},{"mod",b_mod},{"pow",b_pow},{"neg",b_neg},{"inc",b_inc},
    {"dec",b_dec},{"equal?",b_equal},{"notEqual?",b_notEqual},{"greater?",b_greater},{"less?",b_less},
    {"and?",b_and},{"or?",b_or},{"not?",b_not},{"size",b_size},{"first",b_first},
    {"last",b_last},{"append",b_append},{"pop",b_pop},{"makeDict",b_makeDict},{"set",b_set},
    {"get",b_get},{"empty?",b_empty},{"call",b_call},{"type",b_type},
    {"concat",b_concat},{"range",b_range},{"key?",b_key},
    {"map",b_map},{"select",b_select},{"loop",b_loop},
    {"to",b_to},{"replace",b_replace},{"joinWith",b_joinWith},{"fold",b_fold},
    {"lower",b_lower},{"upper",b_upper},{"index",b_index},{"slice",b_slice},
    {"split",b_split},{"take",b_take},{"drop",b_drop},{"reverse",b_reverse},
    {"ensure",b_ensure},{"array",b_array},{"case",b_case},
    {"read",b_read},{"write",b_write},{"execute",b_execute},{"args",b_args},
    {"insert",b_insert},{"break",b_break},{"null?",b_isNull},{"contains?",b_contains},
    {"greaterOrEqual?",b_ge},{"join",b_join},{"try",b_try},
    {NULL,NULL}
};

int rt_builtin(const char *name, Env *e, Value *args, int n, Value *out) {
    for (int i=0; BUILTINS[i].name; i++)
        if (!strcmp(BUILTINS[i].name, name)) { *out = BUILTINS[i].fn(e,args,n); return 1; }
    return 0;
}
/* zero-arity builtins: when a bare word names one and it is NOT bound as a
 * variable, the host CALLS it (`args`, `break`) rather than yielding a value. */
static int rt_zero_arity(const char *name){
    static const char *z[] = {"args","break",NULL};
    for (int i=0; z[i]; i++) if (!strcmp(z[i], name)) return 1;
    return 0;
}
int rt_builtin_known(const char *name) {
    for (int i=0; BUILTINS[i].name; i++)
        if (!strcmp(BUILTINS[i].name, name)) return 1;
    return 0;
}

/* ---- evaluation --------------------------------------------------------- */
static Value runNode0(Env *e, IR *node);  /* forward */

/* map an infix operator symbol (a block-data word) to a binary builtin name */
static const char *binop_name(const char *s) {
    if (!strcmp(s,"*")) return "mul";
    if (!strcmp(s,"+")) return "add";
    if (!strcmp(s,"-")) return "sub";
    if (!strcmp(s,"/")) return "div";
    if (!strcmp(s,"%")) return "mod";
    if (!strcmp(s,"^")) return "pow";
    if (!strcmp(s,">")) return "greater?";
    if (!strcmp(s,"<")) return "less?";
    if (!strcmp(s,"==")) return "equal?";
    if (!strcmp(s,"++")) return "concat";
    if (!strcmp(s,"&&")) return "and?";
    if (!strcmp(s,"||")) return "or?";
    return NULL;
}
static int is_binop(Value v) {
    return v.k==V_STR && binop_name(v.u.s) != NULL;
}
static Value apply_binop(Value a, const char *sym, Value b) {
    const char *nm = binop_name(sym);
    Value argv[2] = {a, b}, out;
    if (rt_builtin(nm, NULL, argv, 2, &out)) return out;
    return v_null();
}

/* structural skip: advance the pointer over an expression WITHOUT evaluating it,
 * so a conditionally-skipped `if cond -> body` body doesn't trigger builtin
 * side effects (print). Mirrors evalExpr/parsePrimary traversal only. */
static int  starts_stmt(Value v);
static void skipExpr(Env *e, Value **it, int n, int *ip);
static void skipPrimary(Env *e, Value **it, int n, int *ip) {
    Value v = *it[*ip];
    if (v.k == V_BLOCK) { (*ip)++; return; }            /* block value: one token */
    if (v.k == V_STR && rt_builtin_known(v.u.s) && !env_bound(e, v.u.s)) {
        (*ip)++;                                        /* function head */
        while (*ip<n && !is_binop(*it[*ip]) && !starts_stmt(*it[*ip]))
            skipExpr(e, it, n, ip);
        return;
    }
    (*ip)++;
}
static void skipExpr(Env *e, Value **it, int n, int *ip) {
    skipPrimary(e, it, n, ip);
    while (*ip<n && is_binop(*it[*ip])) { (*ip)++; skipPrimary(e, it, n, ip); }
}

/* ---- block-as-code evaluator -------------------------------------------
 * A stored block VALUE (map/select/loop action, or `do` on a block) is run as
 * code. Elements are values; a string element is either an infix operator, a
 * builtin function head applied to the following expression, a bound variable
 * (load), or a string constant. Infix binds tighter than function application,
 * matching the host (`print c ++ c` == print(concat(c,c))). */
static Value evalExpr(Env *e, Value **it, int n, int *ip);
static Value parsePrimary(Env *e, Value **it, int n, int *ip);
static Value runBlockValue(Env *e, Value block);
static int  path_is_dyn(Value v);
static Value path_read(Env *e, Value p);

static Value parsePrimary(Env *e, Value **it, int n, int *ip) {
    Value v = *it[*ip];
    if (v.k == V_BLOCK) { (*ip)++; return runBlockValue(e, v); }  /* inline (sub)expr */
    if (v.k == V_STR) {
        if (rt_builtin_known(v.u.s) && !env_bound(e, v.u.s)) {
            /* function head: apply to the following expression(s), stopping at a
             * statement boundary (a define head, `if`, or a dynamic path write)
             * so a call in statement position doesn't swallow later statements */
            (*ip)++;
            Value *args = (Value*)xmalloc((n+1)*sizeof(Value));
            int m = 0;
            while (*ip < n && !is_binop(*it[*ip]) && !starts_stmt(*it[*ip]))
                args[m++] = evalExpr(e, it, n, ip);
            Value out;
            if (rt_builtin(v.u.s, e, args, m, &out)) { free(args); return out; }
            free(args); die("unknown function in action"); return v_null();
        }
        if (binop_name(v.u.s)) { (*ip)++; return v; }   /* lone operator: data */
        if (env_bound(e, v.u.s)) {
            Value bv = env_get(e, v.u.s);
            if (bv.k==V_FUNC || bv.k==V_BUILTIN) {      /* callable: apply to args */
                (*ip)++;
                Value *args=(Value*)xmalloc((n+1)*sizeof(Value));
                int m=0;
                while (*ip<n && !is_binop(*it[*ip]) && !starts_stmt(*it[*ip]))
                    args[m++]=evalExpr(e,it,n,ip);
                Value out=applyFunc(e,bv,args,m);
                free(args); return out;
            }
            (*ip)++; return bv;                         /* plain variable */
        }
        (*ip)++; return v;                              /* string constant */
    }
    if (v.k == V_PATH && path_is_dyn(v)) { (*ip)++; return path_read(e, v); }  /* path read */
    (*ip)++;
    return v;
}
static Value evalExpr(Env *e, Value **it, int n, int *ip) {
    Value left = parsePrimary(e, it, n, ip);
    while (*ip < n && is_binop(*it[*ip])) {
        const char *op = (*it[*ip]).u.s; (*ip)++;
        Value right = parsePrimary(e, it, n, ip);
        left = apply_binop(left, op, right);
    }
    return left;
}
/* ---- dynamic paths in block-as-code --------------------------------------
 * A dynamic path value has "@DYN" as seg[0], the base variable name as seg[1],
 * then segments: a ":"-prefixed name is a literal key, a bare name is a variable
 * whose value (string/int) is the key. Reads resolve in expression position;
 * a dynamic path heading a statement is an assignment. */
static int path_is_dyn(Value v){ return v.k==V_PATH && v.u.path.nsegs>=1 && !strcmp(v.u.path.segs[0],"@DYN"); }
/* a token that starts a NEW statement in a flattened action body: a define head
 * (`@LBL:name`), the `if` keyword, or a dynamic path write (`acc\[k]: v`). A
 * function call in statement position must stop argument collection here. */
static int starts_stmt(Value v){
    if (v.k == V_PATH) return path_is_dyn(v);
    if (v.k == V_STR){
        if (!strncmp(v.u.s,"@LBL:",5)) return 1;
        if (!strcmp(v.u.s,"if")) return 1;
    }
    return 0;
}
static const char *seg_key(Env *e, const char *s){
    if (s[0]==':') return s+1;
    Value v = env_get(e, s);
    if (v.k==V_STR) return v.u.s;
    static char buf[64];
    snprintf(buf,sizeof buf,"%ld",(long)v.u.i);
    return buf;
}
static Value index_at(Value cur, const char *key, Env *e){
    if (cur.k==V_DICT){ int i=dict_find(cur,key); return i>=0?cur.u.dict->vals[i]:v_null(); }
    if (cur.k==V_BLOCK){ long idx=atol(key); if(idx<0||idx>=cur.u.block.n)return v_null(); return *cur.u.block.items[idx]; }
    if (cur.k==V_RANGE){ long idx=atol(key); return (idx>=cur.u.range.lo&&idx<=cur.u.range.hi)?v_int(idx):v_null(); }
    return v_null();
}
static Value path_read(Env *e, Value p){
    Value cur = env_get(e, p.u.path.segs[1]);
    for (int s=2;s<p.u.path.nsegs;s++) cur = index_at(cur, seg_key(e, p.u.path.segs[s]), e);
    return cur;
}
static void path_write(Env *e, Value p, Value val){
    Value cur = env_get(e, p.u.path.segs[1]);
    for (int s=2; s<p.u.path.nsegs-1; s++) cur = index_at(cur, seg_key(e, p.u.path.segs[s]), e);
    const char *last = seg_key(e, p.u.path.segs[p.u.path.nsegs-1]);
    if (cur.k==V_DICT){
        Dict *dd=cur.u.dict; int i=dict_find(cur,last);
        if (i>=0){ dd->vals[i]=val; return; }
        dd->keys=(char**)xrealloc(dd->keys,(dd->n+1)*sizeof(char*));
        dd->vals=(Value*)xrealloc(dd->vals,(dd->n+1)*sizeof(Value));
        dd->keys[dd->n]=(char*)xmalloc(strlen(last)+1); strcpy(dd->keys[dd->n],last);
        dd->vals[dd->n]=val; dd->n++; return;
    }
    if (cur.k==V_BLOCK){ long idx=atol(last); if(idx>=0&&idx<cur.u.block.n) *cur.u.block.items[idx]=val; }
}

static Value evalSeq(Env *e, Value **it, int n) {
    Value r = v_null();
    int i = 0;
    while (i < n) {
        Value h = *it[i];
        if (h.k == V_BLOCK && h.u.block.n>=3 && (*h.u.block.items[0]).k==V_STR
            && !strcmp((*h.u.block.items[0]).u.s,"set")) {
            /* complex path write `[set, base, keyblock] value` */
            Value **sit=h.u.block.items; int sn=h.u.block.n;
            int si=1;
            Value base = evalExpr(e, sit, sn, &si);   /* the dict (by ref) */
            Value key  = evalExpr(e, sit, sn, &si);   /* computed key */
            i++;                                       /* past the head block */
            Value val = evalExpr(e, it, n, &i);
            b_set(e, (Value[]){base,key,val}, 3);
            continue;
        }
        if (h.k == V_PATH && path_is_dyn(h)) {          /* `path\[k]: val` */
            if (i+1 < n) {                              /* path followed by value: WRITE */
                int i2 = i+1;
                Value val = evalExpr(e, it, n, &i2);
                path_write(e, h, val);
                i = i2; continue;
            }
            r = path_read(e, h);                        /* bare path: READ expr */
            i++; continue;
        }
        if (h.k == V_STR && !strncmp(h.u.s,"@LBL:",5)) { /* define `x: val` */
            const char *nm = h.u.s+5; i++;
            r = evalExpr(e, it, n, &i);
            env_set(e, nm, r);
            continue;
        }
        if (h.k == V_STR && !strcmp(h.u.s,"if")) {       /* `if cond -> x` / `if cond [b]` */
            int ci = i+1;
            Value cond = evalExpr(e, it, n, &ci);
            if (ci<n && it[ci]->k==V_STR && !strcmp(it[ci]->u.s,"->")) {
                ci++;
                if (v_truthy(cond)) { r = evalExpr(e, it, n, &ci); }   /* run */
                else               { skipExpr(e, it, n, &ci); }        /* skip w/o side effects */
                i = ci; continue;
            }
            if (ci<n && it[ci]->k==V_BLOCK) {
                Value blk = *it[ci]; ci++;
                if (v_truthy(cond)) r = runBlockValue(e, blk);
                i = ci; continue;
            }
            r = cond; i = ci; continue;
        }
        r = evalExpr(e, it, n, &i);
    }
    return r;
}
static Value runBlockValue(Env *e, Value block) {
    return evalSeq(e, block.u.block.items, block.u.block.n);
}

/* bind a single parameter to an element and run an action block in a child env */
static Value applyAction(Env *parent, Value params, Value action, Value el) {
    Env *child = env_new(parent);
    /* params is a block of word-names, or a single quoted word (passthrough) */
    if (params.k == V_STR) env_set(child, params.u.s, el);
    else if (params.k == V_BLOCK && params.u.block.n > 0) {
        Value p = *params.u.block.items[0];
        if (p.k == V_STR) env_set(child, p.u.s, el);
    }
    return evalSeq(child, action.u.block.items, action.u.block.n);
}

Value runSeq(Env *e, IR **seq, int n) {
    Value result = v_null();
    for (int i=0;i<n;i++) {
        result = runNode0(e, seq[i]);
        if (rt_ret_set) break;
    }
    return result;
}

/* a call whose callee is intrinsic: dispatch to a builtin */
static Value callIntrinsic(Env *e, IR *node) {
    const char *name = node->fn->name;
    Value *argv = (Value*)xmalloc((node->nargs+1)*sizeof(Value));
    for (int i=0;i<node->nargs;i++) argv[i] = runNode0(e, node->args[i]);
    Value out;
    if (rt_builtin(name, e, argv, node->nargs, &out)) return out;
    fprintf(stderr,"[unknown intrinsic: %s]\n", name);
    die("unknown intrinsic"); return v_null();
}

static Value runNode0(Env *e, IR *node) {
    if (!node) return v_null();
    if (!node->op) return node->v;   /* a raw constant */

    if (!strcmp(node->op,"const")) return node->v;
    if (!strcmp(node->op,"load")) {
        Value v = env_get(e, node->name);
        if (v.k==V_NULL && rt_builtin_known(node->name)) {
            Value b; memset(&b,0,sizeof b); b.k=V_BUILTIN; b.u.s=(char*)node->name; return b;
        }
        return v;
    }
    if (!strcmp(node->op,"intrinsic")) return v_str(node->name); /* hostword marker */
    if (!strcmp(node->op,"word")) {
        /* bare word in value position: load the binding if one exists, else call
         * a zero-arity builtin, else a builtin function value, else null. */
        Value v = env_get(e, node->name);
        if (v.k != V_NULL) return v;
        if (rt_builtin_known(node->name)) {
            if (rt_zero_arity(node->name)) {
                Value out, zargv[1];
                if (rt_builtin(node->name, e, zargv, 0, &out)) return out;
            }
            Value b; memset(&b,0,sizeof b); b.k=V_BUILTIN; b.u.s=(char*)node->name; return b;
        }
        return v_null();
    }
    if (!strcmp(node->op,"passthrough")) return node->v;
    if (!strcmp(node->op,"__seq")) return runSeq(e, node->args, node->nargs);

    if (!strcmp(node->op,"define")) {
        Value v = runNode0(e, node->args[0]);
        env_set(e, node->name, v);
        return v;
    }
    if (!strcmp(node->op,"block")) {
        Value **items=(Value**)xmalloc((node->nargs+1)*sizeof(Value*));
        for (int i=0;i<node->nargs;i++){
            Value rv = runNode0(e, node->args[i]);
            Value *cp=(Value*)xmalloc(sizeof(Value)); *cp=rv; items[i]=cp;
        }
        return v_block(items,node->nargs);
    }
    if (!strcmp(node->op,"function")) {
        IR *params = node->args[0];
        IR **body = (IR**)xmalloc((node->nargs)*sizeof(IR*));
        for (int i=1;i<node->nargs;i++) body[i-1]=node->args[i];
        return v_func(params, body, node->nargs-1, e);
    }
    if (!strcmp(node->op,"if")) {
        Value cond = runNode0(e, node->args[0]);
        if (v_truthy(cond)) return node->nargs>1 ? runNode0(e, node->args[1]) : v_null();
        if (node->nargs>2 && node->args[2]) return runNode0(e, node->args[2]);
        return v_null();
    }
    if (!strcmp(node->op,"do")) {
        IR *body = node->args[0];
        /* a __seq body is already the block's statements; running it yields
         * the last value (a block result there is data, e.g. `do [[1 2][3 4]]`) */
        if (body && body->op && !strcmp(body->op,"__seq")) return runNode0(e, body);
        /* a stored block value (e.g. `do b` where b = [10 * 3]) runs as code */
        Value v = runNode0(e, body);
        if (v.k == V_BLOCK) return runBlockValue(e, v);
        return v;
    }
    if (!strcmp(node->op,"return")) {
        rt_ret_set=1; rt_ret_val = runNode0(e, node->args[0]); return rt_ret_val;
    }
    if (!strcmp(node->op,"while")) {
        rt_brk_set=0;
        while (1) {
            Value cond = runNode0(e, node->args[0]); if (rt_ret_set) break;
            if (!v_truthy(cond)) break;
            runNode0(e, node->args[1]); if (rt_ret_set || rt_brk_set) break;
        }
        if (rt_ret_set) return rt_ret_val;
        return v_null();
    }
    if (!strcmp(node->op,"until")) {
        rt_brk_set=0;
        while (1) {
            runNode0(e, node->args[0]); if (rt_ret_set) break;
            Value cond = runNode0(e, node->args[1]); if (rt_ret_set) break;
            if (v_truthy(cond)) break;
        }
        if (rt_ret_set) return rt_ret_val;
        return v_null();
    }
    if (!strcmp(node->op,"call")) {
        if (node->fn && node->fn->op && !strcmp(node->fn->op,"intrinsic"))
            return callIntrinsic(e, node);
        /* user callee: evaluate to a function value and apply */
        Value fn = runNode0(e, node->fn);
        Value *argv=(Value*)xmalloc((node->nargs+1)*sizeof(Value));
        for (int i=0;i<node->nargs;i++) argv[i]=runNode0(e,node->args[i]);
        if (fn.k==V_FUNC || fn.k==V_BUILTIN) return applyFunc(e, fn, argv, node->nargs);
        die("cannot call non-function");
    }
    die("unknown IR op"); return v_null();
}

Value runNode(Env *e, IR *node) { return runNode0(e, node); }

/* =========================================================================
 * Arturo source lexer — `to :block <string>`.
 *
 * A faithful port of the host's vm/parse.nim parseBlock: produces the same
 * token kinds and values the donated host lexer yields, so the native
 * self-compiler parses input exactly as stage 1 does.
 * ========================================================================= */

typedef struct { const char *s; int pos; int len; } LX;
typedef struct { char *b; int len, cap; } SB;

static int lx_peek(LX*x,int off){ int p=x->pos+off; return p<x->len ? (unsigned char)x->s[p] : 0; }
static int lx_at(LX*x){ return lx_peek(x,0); }
static void lx_adv(LX*x){ if(x->pos<x->len) x->pos++; }
static int lx_eof(LX*x){ return x->pos>=x->len; }
static int is_id_start(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_id_in(int c){ return is_id_start(c)||(c>='0'&&c<='9'); }
static int is_digit(int c){ return c>='0'&&c<='9'; }
static int in_syms(int c){ return c=='~'||c=='!'||c=='@'||c=='#'||c=='$'||c=='%'||c=='^'||c=='&'||c=='*'||c=='-'||c=='='||c=='+'||c=='<'||c=='>'||c=='/'||c=='|'||c=='?'; }

static void sb_init(SB*s){ s->cap=32; s->len=0; s->b=xmalloc(32); s->b[0]=0; }
static void sb_add(SB*s,int c){ if(s->len+2>s->cap){s->cap*=2;s->b=xrealloc(s->b,s->cap);} s->b[s->len++]=(char)c; s->b[s->len]=0; }
static void sb_adds(SB*s,const char*st){ for(;*st;st++) sb_add(s,*st); }
static char *sb_take(SB*s){ char *r=s->b; s->b=NULL; return r; }

typedef struct { Value **items; int n, cap; } BLD;
static void bld_init(BLD*b){ b->cap=8; b->n=0; b->items=xmalloc(b->cap*sizeof(Value*)); }
static void bld_add(BLD*b,Value v){ if(b->n>=b->cap){ b->cap*=2; b->items=xrealloc(b->items,b->cap*sizeof(Value*)); } b->items[b->n]=(Value*)xmalloc(sizeof(Value)); b->items[b->n][0]=v; b->n++; }
static Value bld_block(BLD*b){ return v_block(b->items,b->n); }

static Value parse_block(LX*x,int level,int isSubBlock,int isSubInline);
static Value mk_pathkind(VKind k, Value p){ p.k=k; return p; }
static int lx_symbol(LX*x, SB*glyph);
static void parse_ident(LX*x,SB*v,int alsoAddCurrent);
static int hexval(int c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }

/* parse_identifier — port of parse.nim's parseIdentifier. A trailing `?` is
 * glued to the identifier only when at least one char followed the first. */
static void parse_ident(LX*x,SB*v,int alsoAddCurrent){
    int pos=x->pos;
    if(alsoAddCurrent) sb_add(v,x->s[pos]);
    pos++;
    int initialPos=pos;
    while(pos<x->len && is_id_in((unsigned char)x->s[pos])){ sb_add(v,x->s[pos]); pos++; }
    if(pos<x->len && x->s[pos]=='?' && pos!=initialPos){ sb_add(v,'?'); pos++; }
    x->pos=pos;
}

/* parse_string — port of parse.nim's parseString (escape handling). */
static void parse_string(LX*x,SB*v,int stopper){
    x->pos++; /* skip opening quote */
    int inCode=0;
    while(1){
        int c=lx_at(x);
        if(x->pos>=x->len){ break; }          /* unterminated: stop */
        if(c==stopper){ x->pos++; break; }
        if(c=='|'){ sb_add(v,'|'); x->pos++; inCode=!inCode; continue; }
        if(c=='\\' && !inCode){
            int n=lx_peek(x,1);
            switch(n){
                case '\\': case '"': case '\'': case '/': sb_add(v,n); x->pos+=2; break;
                case 'a': sb_add(v,'\a'); x->pos+=2; break;
                case 'b': sb_add(v,'\b'); x->pos+=2; break;
                case 'e': sb_add(v,0x1b); x->pos+=2; break;
                case 'f': sb_add(v,'\f'); x->pos+=2; break;
                case 'n': case 'l': sb_add(v,'\n'); x->pos+=2; break;
                case 'r': case 'c': sb_add(v,'\r'); x->pos+=2; break;
                case 't': sb_add(v,'\t'); x->pos+=2; break;
                case 'v': sb_add(v,'\v'); x->pos+=2; break;
                case 'x': {
                    int xi=0,digits=0;
                    for(int k=1;k<=2 && lx_peek(x,1+k)!=0;k++){ int h=hexval(lx_peek(x,1+k)); if(h<0)break; xi=(xi<<4)|h; digits++; }
                    if(digits) sb_add(v,xi); x->pos+=2+digits; break;
                }
                case 'u': case 'U': {
                    int xi=0,digits=0,nd=(n=='U'?8:4);
                    for(int k=1;k<=nd && lx_peek(x,1+k)!=0;k++){ int h=hexval(lx_peek(x,1+k)); if(h<0)break; xi=(xi<<4)|h; digits++; }
                    if(digits && xi<128) sb_add(v,xi);
                    x->pos+=2+digits; break;
                }
                default:
                    sb_add(v,'\\'); sb_add(v,n); x->pos+=2; break;
            }
            continue;
        }
        if(c=='\r'||c=='\n'){ x->pos++; break; }  /* newline in string: stop */
        sb_add(v,c); x->pos++;
    }
}

/* parse_number — port of parse.nim's parseNumber (decimal; bases rare). */
static void parse_number(LX*x,SB*v,int *hasDot){
    *hasDot=0;
    while(x->pos<x->len && is_digit((unsigned char)x->s[x->pos])){ sb_add(v,x->s[x->pos]); x->pos++; }
    if(lx_at(x)=='.' && is_digit((unsigned char)lx_peek(x,1))){
        *hasDot=1; sb_add(v,'.'); x->pos++;
        while(x->pos<x->len && is_digit((unsigned char)x->s[x->pos])){ sb_add(v,x->s[x->pos]); x->pos++; }
        if(lx_at(x)=='.' && is_digit((unsigned char)lx_peek(x,1))){
            sb_add(v,'.'); x->pos++;
            while(x->pos<x->len && is_digit((unsigned char)x->s[x->pos])){ sb_add(v,x->s[x->pos]); x->pos++; }
        }
    }
}

/* port of parseAndAddSymbol: choose the multi-char symbol glyph. Handles
 * ASCII symbols only (the corpus/compiler use none of the unicode ones). */
static int lx_symbol(LX*x, SB*glyph){
    /* `pos` is the LOCAL cursor; LK peeks the char AFTER pos, so the
     * multi-char glyph lookaheads read forward from where the previous
     * lookahead consumed — not from x->pos (which stays fixed here). */
    int pos=x->pos;
    int c=(unsigned char)x->s[pos];
#define LK(p) (((p)+1)<(x)->len ? (unsigned char)(x)->s[(p)+1] : 0)
    switch(c){
        case '~': if(LK(pos)=='>'){pos++;sb_adds(glyph,"~>");}else sb_adds(glyph,"~"); break;
        case '!': if(LK(pos)=='!'){pos++;sb_adds(glyph,"!!");}else sb_adds(glyph,"!"); break;
        case '?': if(LK(pos)=='?'){pos++;sb_adds(glyph,"??");}else sb_adds(glyph,"?"); break;
        case '@': sb_adds(glyph,"@"); break;
        case '#':
            if(is_id_in((unsigned char)LK(pos))){ /* color code: not a symbol */
                int q=pos+1; while(q<x->len && is_id_in((unsigned char)x->s[q])) q++;
                x->pos=q; glyph->len=0; glyph->b[0]=0; return 0;
            }
            if(LK(pos)=='#'){ pos++; if(LK(pos)=='#'){pos++;sb_adds(glyph,"###");} else sb_adds(glyph,"##"); }
            else sb_adds(glyph,"#"); break;
        case '$': sb_adds(glyph,"$"); break;
        case '%': sb_adds(glyph,"%"); break;
        case '^': sb_adds(glyph,"^"); break;
        case '&': sb_adds(glyph,"&"); break;
        case '*': if(LK(pos)=='*'){pos++;sb_adds(glyph,"**");}else sb_adds(glyph,"*"); break;
        case '|': if(LK(pos)=='|'){pos++;sb_adds(glyph,"||");}
                  else if(LK(pos)=='-'){pos++;sb_adds(glyph,"|-");}
                  else if(LK(pos)=='='){pos++;sb_adds(glyph,"|=");}
                  else if(LK(pos)=='>'){pos++;sb_adds(glyph,"|>");}
                  else sb_adds(glyph,"|"); break;
        case '/': if(LK(pos)=='/'){pos++;sb_adds(glyph,"//");}
                  else if(LK(pos)=='%'){pos++;sb_adds(glyph,"/%");}
                  else if(LK(pos)=='\\'){pos++;sb_adds(glyph,"/\\");}
                  else sb_adds(glyph,"/"); break;
        case '+': if(LK(pos)=='+'){pos++;sb_adds(glyph,"++");}else sb_adds(glyph,"+"); break;
        case '-':
            if(LK(pos)=='>'){
                pos++;
                if(LK(pos)=='>'){pos++;sb_adds(glyph,"->>");}else sb_adds(glyph,"->");
            } else if(LK(pos)=='<'){
                pos++;
                if(LK(pos)=='<'){pos++;sb_adds(glyph,"-<<");}else sb_adds(glyph,"-<");
            } else if(LK(pos)==':'){pos++;sb_adds(glyph,"-:");}
            else if(LK(pos)=='-'){
                pos++;
                if(LK(pos)=='>'){pos++;sb_adds(glyph,"-->");}else sb_adds(glyph,"--");
            } else sb_adds(glyph,"-"); break;
        case '=':
            if(LK(pos)=='~'){pos++;sb_adds(glyph,"=~");}
            else if(LK(pos)=='>'){ pos++; if(LK(pos)=='>'){pos++;sb_adds(glyph,"=>>");}else sb_adds(glyph,"=>"); }
            else if(LK(pos)=='<'){pos++;sb_adds(glyph,"=<");}
            else if(LK(pos)=='='){ pos++; if(LK(pos)=='>'){pos++;sb_adds(glyph,"==>");}else sb_adds(glyph,"=="); }
            else sb_adds(glyph,"="); break;
        case '<':
            if(LK(pos)=='='){ pos++; if(LK(pos)=='='){pos++; if(LK(pos)=='>'){pos++;sb_adds(glyph,"<=>");} else sb_adds(glyph,"<==");} else sb_adds(glyph,"<="); }
            else if(LK(pos)=='-'){ pos++; if(LK(pos)=='>'){pos++; if(LK(pos)=='>'){pos++;sb_adds(glyph,"<->>");} else sb_adds(glyph,"<->");} else sb_adds(glyph,"<-"); }
            else if(LK(pos)=='>'){pos++;sb_adds(glyph,"<>");}
            else if(LK(pos)=='<'){ pos++; if(LK(pos)=='<'){pos++;sb_adds(glyph,"<<<");} else sb_adds(glyph,"<<"); }
            else if(LK(pos)=='~'){ pos++; if(LK(pos)=='>'){pos++;sb_adds(glyph,"<~>");} else sb_adds(glyph,"<~"); }
            else if(LK(pos)=='|'){ pos++; if(LK(pos)=='>'){pos++;sb_adds(glyph,"<|>");} else sb_adds(glyph,"<|"); }
            else if(LK(pos)==':'){pos++;sb_adds(glyph,"<:");}
            else sb_adds(glyph,"<"); break;
        case '>':
            if(LK(pos)=='='){pos++;sb_adds(glyph,">=");}
            else if(LK(pos)=='-'){ pos++; if(LK(pos)=='<'){pos++;sb_adds(glyph,">-<");} else sb_adds(glyph,">-"); }
            else if(LK(pos)=='>'){ pos++; if(LK(pos)=='>'){pos++;sb_adds(glyph,">>>");} else sb_adds(glyph,">>"); }
            else if(LK(pos)==':'){pos++;sb_adds(glyph,">:");}
            else sb_adds(glyph,">"); break;
        default: sb_adds(glyph,"?"); break;
    }
    pos++;
    x->pos=pos;
    return 1;
}

/* the type names the host's `newType` resolves to themselves; any other
 * `:name` becomes `:object`. (Empirically even `task`/`event`/`channel` fail.) */
static int lx_known_type(const char *s){
    static const char *known[] = {
        "null","logical","integer","floating","complex","rational","version","type",
        "char","string","word","literal","label","attribute","attributelabel","path",
        "pathlabel","pathliteral","symbol","symbolliteral","unit","quantity","error",
        "errorkind","regex","color","date","binary","dictionary","object","store",
        "function","method","inline","block","module","range","database","socket",
        "bytecode","nothing","any", NULL
    };
    for(int i=0;known[i];i++) if(!strcmp(known[i],s)) return 1;
    return 0;
}

/* port of parse.nim's parseCurlyString: a `{ ... }` curly string. A leading
 * `/` makes it a regex (value = pattern, optional i/m/s flags after the closing
 * `/`); a leading `:` makes it verbatim; a `!` skips flag letters. A trailing
 * `:` makes the whole thing a label. */
static Value parse_curly(LX*x){
    x->pos++; /* consume '{' */
    int verbatim=0, regex=0;
    char flags[8]={0}; int nflags=0;
    if(lx_at(x)=='!'){
        x->pos++;
        while(lx_at(x) && is_id_in((unsigned char)lx_at(x))) x->pos++;
    }
    if(lx_at(x)==':'){ x->pos++; verbatim=1; }
    else if(lx_at(x)=='/'){ x->pos++; regex=1; }
    SB sb; sb_init(&sb);
    int depth=1;
    while(x->pos<x->len && depth>0){
        int c=lx_at(x);
        if(c=='{'){ depth++; sb_add(&sb,'{'); x->pos++; }
        else if(c=='}'){
            depth--; x->pos++;
            if(depth==0) break;
            sb_add(&sb,'}');
        }
        else if(c=='\\'){
            if(regex && lx_peek(x,1)=='/'){ sb_add(&sb,'/'); x->pos+=2; }
            else { sb_add(&sb,'\\'); x->pos++; }
        }
        else if(c=='/' && regex){
            int nx=lx_peek(x,1);
            if(nx=='}'){ x->pos++; }   /* regex terminator: consume '/'; '}' closes next iter */
            else if(nx=='i'||nx=='m'||nx=='s'){
                x->pos++;              /* consume '/' then the flag letters */
                while(lx_at(x)=='i'||lx_at(x)=='m'||lx_at(x)=='s'){ if(nflags<7)flags[nflags++]=lx_at(x); x->pos++; }
                if(lx_at(x)!='}'){ free(sb.b); return v_str(""); } /* unterminated */
            }
            else { sb_add(&sb,'/'); x->pos++; }  /* literal '/' inside pattern */
        }
        else { sb_add(&sb,c); x->pos++; }
    }
    flags[nflags]=0;
    if(lx_at(x)==':'){ x->pos++; return v_token(V_LABEL, sb.b); }
    if(regex) return v_token(V_REGEX, sb.b);
    return v_str(sb.b);
}

/* a path (X\Y\Z) root + segments -> Value. asLiteral=1 -> pathliteral. */
static Value parse_path(LX*x, Value root, int asLiteral){
    int n=1, cap=4;
    Value *segs=(Value*)xmalloc(cap*sizeof(Value));
    segs[0]=root;
    while(lx_at(x)=='\\'){
        x->pos++;
        int c=lx_at(x);
        if(is_id_start((unsigned char)c)){
            SB sb; sb_init(&sb); parse_ident(x,&sb,1);
            if(n>=cap){cap*=2;segs=xrealloc(segs,cap*sizeof(Value));} segs[n++]=v_token(V_LITERAL, sb.b); free(sb.b);
        } else if(is_digit((unsigned char)c)){
            SB sb; sb_init(&sb); int hasDot; parse_number(x,&sb,&hasDot);
            if(n>=cap){cap*=2;segs=xrealloc(segs,cap*sizeof(Value));} segs[n++]= hasDot? v_float(atof(sb.b)) : v_int(atol(sb.b)); free(sb.b);
        } else if(c=='[' && !asLiteral){
            x->pos++; Value sub=parse_block(x,0,1,0);
            if(n>=cap){cap*=2;segs=xrealloc(segs,cap*sizeof(Value));} segs[n++]=sub;
        } else break;
    }
    return v_pathv(segs,n);
}

static Value parse_block(LX*x,int level,int isSubBlock,int isSubInline){
    BLD b; bld_init(&b);
    while(1){
        /* skip whitespace and comments */
        while(1){
            int c=lx_at(x);
            if(x->pos>=x->len) break;
            if(c==' '||c=='\t'||c=='\r'||c=='\n'){ x->pos++; continue; }
            if(c==';'){
                if(lx_peek(x,1)==';'){ x->pos+=2; while(x->pos<x->len && x->s[x->pos]!='\r' && x->s[x->pos]!='\n') x->pos++; }
                else { while(x->pos<x->len && x->s[x->pos]!='\r' && x->s[x->pos]!='\n') x->pos++; }
                continue;
            }
            if(c=='#' && lx_peek(x,1)=='!'){ while(x->pos<x->len && x->s[x->pos]!='\r' && x->s[x->pos]!='\n') x->pos++; continue; }
            break;
        }
        if(x->pos>=x->len){
            if(level!=0 && (isSubBlock||isSubInline)){ /* unterminated: return what we have */ }
            break;
        }
        int c=lx_at(x);
        if(c=='"'){
            SB sb; sb_init(&sb); parse_string(x,&sb,'"');
            if(lx_at(x)==':'){ x->pos++; bld_add(&b, v_token(V_LABEL, sb.b)); }
            else bld_add(&b, v_str(sb.b));
            free(sb.b);
        } else if(c==':'){
            SB sb; sb_init(&sb); parse_ident(x,&sb,0);
            if(sb.len==0){ free(sb.b); sb.b=NULL;
                if(lx_at(x)==':'){ x->pos++; bld_add(&b,v_token(V_SYMBOL,"::")); }
                else if(lx_at(x)=='='){ x->pos++; bld_add(&b,v_token(V_SYMBOL,":=")); }
                else bld_add(&b,v_token(V_SYMBOL,":"));
            } else {
                /* host `newType`: a `:name` whose name is not a known ValueKind
                 * resolves to `:object` (e.g. `:interpret`, `:emit`, `:foo`).
                 * Known names keep their own name. */
                if(lx_known_type(sb.b)) bld_add(&b, v_token(V_TYPE, sb.b));
                else bld_add(&b, v_token(V_TYPE, "object"));
                free(sb.b);
            }
        } else if(is_digit(c)){
            SB sb; sb_init(&sb); int hasDot; parse_number(x,&sb,&hasDot);
            /* exponent */
            if((lx_at(x)=='e'||lx_at(x)=='E') && (is_digit((unsigned char)lx_peek(x,1))||lx_peek(x,1)=='+'||lx_peek(x,1)=='-')){
                int pp=x->pos; sb_add(&sb,x->s[x->pos]); x->pos++;
                if(lx_at(x)=='+'||lx_at(x)=='-'){ sb_add(&sb,lx_at(x)); x->pos++; }
                while(x->pos<x->len && is_digit((unsigned char)x->s[x->pos])){ sb_add(&sb,x->s[x->pos]); x->pos++; }
                bld_add(&b, v_float(atof(sb.b)));
            } else if(hasDot){
                bld_add(&b, v_float(atof(sb.b)));
            } else {
                bld_add(&b, v_int(atol(sb.b)));
            }
            free(sb.b);
        } else if(in_syms(c)){
            SB g; sb_init(&g); int isSym=lx_symbol(x,&g);
            if(isSym && g.len>0) bld_add(&b, v_token(V_SYMBOL, g.b));
            free(g.b);
        } else if(c=='\\'){
            int n=lx_peek(x,1);
            if(is_id_start((unsigned char)n)||n=='['){
                Value root=v_token(V_WORD,"this");
                Value p=parse_path(x,root,0);
                if(lx_at(x)==':'){ x->pos++; bld_add(&b, mk_pathkind(V_PATHLABEL,p)); }
                else bld_add(&b, p);
            } else if(n=='\\'){ x->pos+=2; bld_add(&b,v_token(V_SYMBOL,"\\\\")); }
            else if(n=='/'){ x->pos+=2; bld_add(&b,v_token(V_SYMBOL,"//")); }
            else { x->pos++; bld_add(&b,v_token(V_SYMBOL,"\\")); }
        } else if(is_id_start(c)){
            SB sb; sb_init(&sb); parse_ident(x,&sb,1);
            if(sb.len==1 && sb.b[0]=='_'){ bld_add(&b,v_token(V_SYMBOL,"_")); free(sb.b); }
            else if(lx_at(x)==':'){ x->pos++; bld_add(&b, v_token(V_LABEL, sb.b)); free(sb.b); }
            else if(lx_at(x)=='\\' && (is_id_start((unsigned char)lx_peek(x,1))||is_digit((unsigned char)lx_peek(x,1))||lx_peek(x,1)=='[')){
                Value root=v_token(V_WORD, sb.b); free(sb.b);
                Value p=parse_path(x,root,0);
                if(lx_at(x)==':'){ x->pos++; bld_add(&b, mk_pathkind(V_PATHLABEL,p)); }
                else bld_add(&b, p);
            } else if(lx_at(x)=='\\'){
                x->pos++; bld_add(&b, v_token(V_SYMBOL, "\\")); free(sb.b);
            } else {
                bld_add(&b, v_token(V_WORD, sb.b)); free(sb.b);
            }
        } else if(c=='\''){
            int initialP=x->pos;
            SB sb; sb_init(&sb); parse_ident(x,&sb,0);
            if(sb.len==0){ free(sb.b);
                /* empty after tick: symbol-literal or char */
                if(in_syms(lx_at(x))){
                    SB g; sb_init(&g); lx_symbol(x,&g);
                    /* backslash-escape char like '\n' */
                    if(lx_at(x)=='\''){ x->pos++; bld_add(&b, v_char(g.b[1])); }
                    else if(!strcmp(g.b,"\\")) { /* \n style */ if(lx_at(x)=='n'){x->pos++;bld_add(&b,v_char('\n'));} else if(lx_at(x)=='t'){x->pos++;bld_add(&b,v_char('\t'));} else bld_add(&b,v_token(V_SYMBOL,g.b)); }
                    else bld_add(&b, v_token(V_SYMBOL, g.b));
                    free(g.b);
                } else {
                    x->pos=initialP; SB s2; sb_init(&s2); parse_string(x,&s2,'\''); bld_add(&b, v_char(s2.b[0])); free(s2.b);
                }
            } else {
                if(lx_at(x)=='\\' && (is_id_start((unsigned char)lx_peek(x,1))||is_digit((unsigned char)lx_peek(x,1)))){
                    Value root=v_token(V_WORD, sb.b); free(sb.b);
                    Value p=parse_path(x,root,1);
                    bld_add(&b, mk_pathkind(V_PATHLITERAL,p));
                } else if(lx_at(x)=='\''){
                    x->pos++; bld_add(&b, v_char(sb.b[0])); free(sb.b);
                } else {
                    bld_add(&b, v_token(V_LITERAL, sb.b)); free(sb.b);
                }
            }
        } else if(c=='`'){
            x->pos++; SB sb; sb_init(&sb);
            while(x->pos<x->len && (is_id_in((unsigned char)x->s[x->pos])||x->s[x->pos]=='.'||x->s[x->pos]=='/')){ sb_add(&sb,x->s[x->pos]); x->pos++; }
            bld_add(&b, v_str(sb.b)); free(sb.b);
        } else if(c=='.'){
            if(lx_peek(x,1)=='.'){ x->pos+=2; if(lx_at(x)=='.'){ x->pos++; bld_add(&b,v_token(V_SYMBOL,"...")); } else bld_add(&b,v_token(V_SYMBOL,"..")); }
            else if(lx_peek(x,1)=='/'){ x->pos+=2; bld_add(&b,v_token(V_SYMBOL,"./")); }
            else { SB sb; sb_init(&sb); parse_ident(x,&sb,0); if(lx_at(x)==':'){ x->pos++; bld_add(&b,v_token(V_ATTRIBUTELABEL,sb.b)); } else bld_add(&b,v_token(V_ATTRIBUTE,sb.b)); free(sb.b); }
        } else if(c=='['){
            x->pos++; Value sub=parse_block(x,level+1,1,0); bld_add(&b, sub);
        } else if(c==']'){
            if(isSubBlock){ x->pos++; break; }
            else break; /* stray */
        } else if(c=='('){
            x->pos++; Value sub=parse_block(x,level+1,0,1); bld_add(&b, sub);
        } else if(c==')'){
            if(isSubInline){ x->pos++; break; }
            else break;
        } else if(c=='{'){
            bld_add(&b, parse_curly(x));
        } else {
            x->pos++; /* skip unknown byte */
        }
    }
    Value r=bld_block(&b);
    if(isSubInline) r.k=V_INLINE;
    return r;
}

Value lex_source(const char *s){
    LX x; x.s=s; x.pos=0; x.len=(int)strlen(s);
    return parse_block(&x,0,0,0);
}
