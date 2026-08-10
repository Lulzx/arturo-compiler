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

/* ---- error handling ----------------------------------------------------- */
static void die(const char *msg) {
    fprintf(stderr, "runtime error: %s\n", msg);
    exit(1);
}
void rt_error(const char *msg) { die(msg); }

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { die("out of memory"); }
    return p;
}
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
    Value v; memset(&v,0,sizeof v); v.k=V_PATH; v.u.path.segs=segs; v.u.path.nsegs=n; return v;
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
IR *ir_define(const char *name, IR *expr){ IR*n=ir_new("define"); n->name=name; n->args=(IR**)xmalloc(sizeof(IR*)); n->args[0]=expr; n->nargs=1; return n; }
IR *ir_call(IR *fn, IR **args, int n){ IR*x=ir_new("call"); x->fn=fn; x->args=args; x->nargs=n; return x; }
IR *ir_passthrough(Value src){ IR*n=ir_new("passthrough"); n->v=src; return n; }
IR *ir_block(IR **items, int n){ IR*x=ir_new("block"); x->args=items; x->nargs=n; return x; }
IR *ir_fn(IR *params, IR **body, int n){ IR*x=ir_new("function"); x->args=(IR**)xmalloc((n+1)*sizeof(IR*)); x->args[0]=params; for(int i=0;i<n;i++)x->args[i+1]=body[i]; x->nargs=n+1; return x; }
IR *ir_op(const char *op, IR **args, int n){ IR*x=ir_new(op); x->args=args; x->nargs=n; return x; }
/* a seq: internal __seq node wrapping a list of statements */
IR *ir_seq(IR **items, int n){ IR*x=ir_new("__seq"); x->args=items; x->nargs=n; return x; }

/* ---- return signal ------------------------------------------------------- */
static int   rt_ret_set = 0;
static Value rt_ret_val;

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
    int cnt = coll.k==V_RANGE ? (int)(coll.u.range.hi-coll.u.range.lo+1) : coll.u.block.n;
    for(int i=0;i<cnt;i++){
        Value el = coll.k==V_RANGE ? v_int(coll.u.range.lo+i) : *coll.u.block.items[i];
        applyAction(e,params,body,el);
    }
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
    if (d.k!=V_DICT) die("get: expected dict");
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

static struct { const char *name; Value (*fn)(Env*,Value*,int); } BUILTINS[] = {
    {"print",b_print},{"add",b_add},{"sub",b_sub},{"mul",b_mul},{"div",b_div},
    {"fdiv",b_fdiv},{"mod",b_mod},{"pow",b_pow},{"neg",b_neg},{"inc",b_inc},
    {"dec",b_dec},{"equal?",b_equal},{"notEqual?",b_notEqual},{"greater?",b_greater},{"less?",b_less},
    {"and?",b_and},{"or?",b_or},{"not?",b_not},{"size",b_size},{"first",b_first},
    {"last",b_last},{"append",b_append},{"pop",b_pop},{"makeDict",b_makeDict},{"set",b_set},
    {"get",b_get},{"empty?",b_empty},{"call",b_call},{"type",b_type},
    {"concat",b_concat},{"range",b_range},{"key?",b_key},
    {"map",b_map},{"select",b_select},{"loop",b_loop},
    {NULL,NULL}
};

int rt_builtin(const char *name, Env *e, Value *args, int n, Value *out) {
    for (int i=0; BUILTINS[i].name; i++)
        if (!strcmp(BUILTINS[i].name, name)) { *out = BUILTINS[i].fn(e,args,n); return 1; }
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

/* ---- block-as-code evaluator -------------------------------------------
 * A stored block VALUE (map/select/loop action, or `do` on a block) is run as
 * code. Elements are values; a string element is either an infix operator, a
 * builtin function head applied to the following expression, a bound variable
 * (load), or a string constant. Infix binds tighter than function application,
 * matching the host (`print c ++ c` == print(concat(c,c))). */
static Value evalExpr(Env *e, Value **it, int n, int *ip);
static Value parsePrimary(Env *e, Value **it, int n, int *ip);
static Value runBlockValue(Env *e, Value block);

static Value parsePrimary(Env *e, Value **it, int n, int *ip) {
    Value v = *it[*ip];
    if (v.k == V_BLOCK) { (*ip)++; return runBlockValue(e, v); }  /* inline (sub)expr */
    if (v.k == V_STR) {
        if (rt_builtin_known(v.u.s) && !env_bound(e, v.u.s)) {
            /* function head: apply to the maximal following expression(s) */
            (*ip)++;
            Value *args = (Value*)xmalloc((n+1)*sizeof(Value));
            int m = 0;
            while (*ip < n && !is_binop(*it[*ip])) args[m++] = evalExpr(e, it, n, ip);
            Value out;
            if (rt_builtin(v.u.s, e, args, m, &out)) { free(args); return out; }
            free(args); die("unknown function in action"); return v_null();
        }
        if (binop_name(v.u.s)) { (*ip)++; return v; }   /* lone operator: data */
        if (env_bound(e, v.u.s)) { (*ip)++; return env_get(e, v.u.s); }
        (*ip)++; return v;                              /* string constant */
    }
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
static Value evalSeq(Env *e, Value **it, int n) {
    Value r = v_null();
    int i = 0;
    while (i < n) r = evalExpr(e, it, n, &i);
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
        while (1) {
            Value cond = runNode0(e, node->args[0]); if (rt_ret_set) break;
            if (!v_truthy(cond)) break;
            runNode0(e, node->args[1]); if (rt_ret_set) break;
        }
        if (rt_ret_set) return rt_ret_val;
        return v_null();
    }
    if (!strcmp(node->op,"until")) {
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
