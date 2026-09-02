/* runtime.c — native runtime for the compiler's C backend.
 *
 * Mirrors kernel.art's runNode/runSeq semantics so the compiler's IR, when
 * embedded in C and run here, behaves exactly as the donated VM runs it.
 * Compiled once to runtime.a; the per-program step is gcc + link.
 */
/* strptime and friends are POSIX/XSI extensions; ask for them explicitly so
 * glibc declares them (an implicit declaration truncated the returned
 * pointer to int, and newer compilers reject it outright). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <stdio.h>
#include <execinfo.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <regex.h>
#include <limits.h>
#include <stdint.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/md5.h>
#include <openssl/sha.h>
#endif
extern char **environ;

/* ---- process arena ----------------------------------------------------- */
#ifndef __has_feature
#define __has_feature(feature) 0
#endif

/* Sanitizer builds retain real malloc/free so use-after-free remains visible.
 * ARTURO_TRACKED_HEAP provides the same diagnostic allocator in an ordinary
 * build. Normal binaries use chunked size classes: Arturo values are heavily
 * aliased and process-owned, while short-lived parser and call buffers benefit
 * from O(1) freelist reuse instead of a hash-table operation per allocation. */
#if defined(ARTURO_TRACKED_HEAP) || defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define ARENA_TOMB ((void*)1)
static void **g_arena_slots=NULL;
static size_t g_arena_cap=0,g_arena_used=0,g_arena_tomb=0;
static int g_arena_cleanup_registered=0;
static size_t arena_hash(const void *pointer){
    uintptr_t x=(uintptr_t)pointer;x^=x>>17;x*=(uintptr_t)0x9E3779B97F4A7C15ull;x^=x>>29;return (size_t)x;
}
static void arena_insert_slot(void *pointer){
    size_t mask=g_arena_cap-1,i=arena_hash(pointer)&mask;
    while(g_arena_slots[i]&&g_arena_slots[i]!=ARENA_TOMB)i=(i+1)&mask;
    if(g_arena_slots[i]==ARENA_TOMB)g_arena_tomb--;
    g_arena_slots[i]=pointer;g_arena_used++;
}
static void arena_rehash(size_t capacity){
    void **old=g_arena_slots;size_t old_capacity=g_arena_cap;
    g_arena_slots=(void**)calloc(capacity,sizeof *g_arena_slots);
    if(!g_arena_slots){fprintf(stderr,"runtime error: out of memory\n");exit(1);}
    g_arena_cap=capacity;g_arena_used=0;g_arena_tomb=0;
    for(size_t i=0;i<old_capacity;i++)if(old[i]&&old[i]!=ARENA_TOMB)arena_insert_slot(old[i]);
    free(old);
}
static void arena_track(void *pointer){
    if((g_arena_used+g_arena_tomb+1)*2>g_arena_cap){
        size_t capacity=g_arena_cap?g_arena_cap:1024;
        if(g_arena_used*4>=g_arena_cap)capacity*=2; /* otherwise just purge tombstones */
        arena_rehash(capacity);
    }
    arena_insert_slot(pointer);
}
static void **arena_find(const void *pointer){
    if(!g_arena_cap)return NULL;
    size_t mask=g_arena_cap-1,i=arena_hash(pointer)&mask;
    while(g_arena_slots[i]){if(g_arena_slots[i]==pointer)return &g_arena_slots[i];i=(i+1)&mask;}
    return NULL;
}
static int arena_untrack(const void *pointer){
    void **slot=arena_find(pointer);
    if(!slot)return 0;
    *slot=ARENA_TOMB;g_arena_used--;g_arena_tomb++;return 1;
}
/* Sequential-access cache for rune addressing (see rune_offset). Forget a
 * buffer whenever it is freed, resized, or written in place. */
static struct { const char *s; int index; const char *at; int count; } g_rune_cache;
static void rune_cache_forget(const void *s){ if(s&&s==(const void*)g_rune_cache.s)g_rune_cache.s=NULL; }
void *runtime_alloc(size_t size){
    if(!g_arena_cleanup_registered){if(atexit(runtime_cleanup)!=0){fprintf(stderr,"runtime error: cannot register cleanup\n");exit(1);}g_arena_cleanup_registered=1;}
    void *pointer=malloc(size?size:1);
    if(!pointer){fprintf(stderr,"runtime error: out of memory\n");exit(1);}
    arena_track(pointer);return pointer;
}
static void *arena_realloc(void *pointer,size_t size){
    if(!pointer)return runtime_alloc(size);
    rune_cache_forget(pointer);
    int tracked=arena_untrack(pointer);
    void *resized=realloc(pointer,size?size:1);
    if(!resized){fprintf(stderr,"runtime error: out of memory\n");exit(1);}
    (void)tracked;arena_track(resized);
    return resized;
}
static char *arena_strdup(const char *source){
    size_t size=strlen(source)+1;char *copy=(char*)runtime_alloc(size);memcpy(copy,source,size);return copy;
}
static void arena_free(void *pointer){
    if(!pointer)return;
    rune_cache_forget(pointer);
    arena_untrack(pointer);
    free(pointer);
}
void runtime_cleanup(void){
    for(size_t i=0;i<g_arena_cap;i++)if(g_arena_slots[i]&&g_arena_slots[i]!=ARENA_TOMB)free(g_arena_slots[i]);
    free(g_arena_slots);g_arena_slots=NULL;g_arena_cap=g_arena_used=g_arena_tomb=0;
}
#else
#define ARENA_CHUNK_BYTES ((size_t)1024*1024)
#define ARENA_CLASS_COUNT 10
#define ARENA_MAGIC 0x41525452u
#define ARENA_LARGE_CLASS UINT32_MAX
typedef struct ArenaHeader { size_t requested; uint32_t class_id; uint32_t magic; } ArenaHeader;
typedef struct ArenaChunk ArenaChunk;
struct ArenaChunk {
    ArenaChunk *next;
    size_t used;
    size_t capacity;
    union { max_align_t align; unsigned char bytes[1]; } data;
};
typedef struct ArenaLarge ArenaLarge;
struct ArenaLarge {
    ArenaLarge *prev,*next;
    ArenaHeader header;
};
static const size_t ARENA_CLASS_SIZE[ARENA_CLASS_COUNT]={16,32,64,128,256,512,1024,2048,4096,8192};
static void *g_arena_free[ARENA_CLASS_COUNT];
static ArenaChunk *g_arena_chunks=NULL;
static ArenaLarge *g_arena_large=NULL;
static int g_arena_cleanup_registered=0;
static struct { const char *s; int index; const char *at; int count; } g_rune_cache;
static void rune_cache_forget(const void *s){if(s&&s==(const void*)g_rune_cache.s)g_rune_cache.s=NULL;}
static void arena_register_cleanup(void){
    if(!g_arena_cleanup_registered){if(atexit(runtime_cleanup)!=0){fprintf(stderr,"runtime error: cannot register cleanup\n");exit(1);}g_arena_cleanup_registered=1;}
}
static int arena_class_for(size_t size){
    for(int i=0;i<ARENA_CLASS_COUNT;i++)if(size<=ARENA_CLASS_SIZE[i])return i;
    return -1;
}
static void *arena_small_alloc(size_t size,int class_id){
    void *pointer=g_arena_free[class_id];
    if(pointer){g_arena_free[class_id]=*(void**)pointer;ArenaHeader *h=(ArenaHeader*)pointer-1;h->requested=size;return pointer;}
    size_t total=sizeof(ArenaHeader)+ARENA_CLASS_SIZE[class_id];
    ArenaChunk *chunk=g_arena_chunks;
    if(!chunk||chunk->capacity-chunk->used<total){
        size_t offset=offsetof(ArenaChunk,data.bytes);
        chunk=(ArenaChunk*)malloc(offset+ARENA_CHUNK_BYTES);
        if(!chunk){fprintf(stderr,"runtime error: out of memory\n");exit(1);}
        chunk->next=g_arena_chunks;chunk->used=0;chunk->capacity=ARENA_CHUNK_BYTES;g_arena_chunks=chunk;
    }
    ArenaHeader *h=(ArenaHeader*)(chunk->data.bytes+chunk->used);chunk->used+=total;
    h->requested=size;h->class_id=(uint32_t)class_id;h->magic=ARENA_MAGIC;return h+1;
}
void *runtime_alloc(size_t size){
    arena_register_cleanup();if(!size)size=1;int class_id=arena_class_for(size);
    if(class_id>=0)return arena_small_alloc(size,class_id);
    ArenaLarge *large=(ArenaLarge*)malloc(sizeof(ArenaLarge)+size);
    if(!large){fprintf(stderr,"runtime error: out of memory\n");exit(1);}
    large->prev=NULL;large->next=g_arena_large;if(g_arena_large)g_arena_large->prev=large;g_arena_large=large;
    large->header.requested=size;large->header.class_id=ARENA_LARGE_CLASS;large->header.magic=ARENA_MAGIC;
    return &large->header+1;
}
static void arena_free(void *pointer){
    if(!pointer)return;rune_cache_forget(pointer);ArenaHeader *h=(ArenaHeader*)pointer-1;
    if(h->magic!=ARENA_MAGIC){fprintf(stderr,"runtime error: invalid arena pointer\n");abort();}
    if(h->class_id==ARENA_LARGE_CLASS){
        ArenaLarge *large=(ArenaLarge*)((unsigned char*)h-(size_t)offsetof(ArenaLarge,header));
        if(large->prev)large->prev->next=large->next;else g_arena_large=large->next;
        if(large->next)large->next->prev=large->prev;h->magic=0;free(large);return;
    }
    int class_id=(int)h->class_id;*(void**)pointer=g_arena_free[class_id];g_arena_free[class_id]=pointer;
}
static void *arena_realloc(void *pointer,size_t size){
    if(!pointer)return runtime_alloc(size);if(!size)size=1;rune_cache_forget(pointer);
    ArenaHeader *h=(ArenaHeader*)pointer-1;
    if(h->magic!=ARENA_MAGIC){fprintf(stderr,"runtime error: invalid arena pointer\n");abort();}
    size_t capacity=h->class_id==ARENA_LARGE_CLASS?h->requested:ARENA_CLASS_SIZE[h->class_id];
    if(size<=capacity){h->requested=size;return pointer;}
    size_t old_size=h->requested;void *resized=runtime_alloc(size);memcpy(resized,pointer,old_size);arena_free(pointer);return resized;
}
static char *arena_strdup(const char *source){size_t size=strlen(source)+1;char *copy=(char*)runtime_alloc(size);memcpy(copy,source,size);return copy;}
void runtime_cleanup(void){
    while(g_arena_large){ArenaLarge *next=g_arena_large->next;free(g_arena_large);g_arena_large=next;}
    while(g_arena_chunks){ArenaChunk *next=g_arena_chunks->next;free(g_arena_chunks);g_arena_chunks=next;}
    memset(g_arena_free,0,sizeof g_arena_free);
}
#endif
static void libc_free(void *pointer){free(pointer);}
#define malloc runtime_alloc
#define realloc arena_realloc
#define strdup arena_strdup
#define free arena_free

/* forward declaration so line_map_add (below, above the allocator
 * definitions) can grow the line map */
static void *xrealloc(void *p, size_t n);

/* ---- error handling ----------------------------------------------------- */
/* `try` uses setjmp/longjmp: die() unwinds to the nearest active try frame
 * instead of exiting when one is set. A linked stack of jmp_bufs lets nested
 * trys nest correctly. */
static jmp_buf *g_try_jmp = NULL;
static char g_last_error[512];
static char g_last_error_kind[256]="Runtime Error";
static int g_custom_error_pending=0;
static const char *g_source_path = NULL;
/* source line mapping (diagnostics).  Built by the lexer pass below: one entry
 * per flat top-level element in the order `to :block` yields. */
typedef struct { int *lines; int n; } LineMap;
static LineMap g_line_map = {NULL, 0};
static void line_map_reset(void){ free(g_line_map.lines); g_line_map.lines=NULL; g_line_map.n=0; }
typedef struct { const char **names; Value *values; int n; } AttrContext;
static AttrContext g_attrs = {NULL,NULL,0};

static void line_map_add(int line){
    if(g_line_map.n%64==0){
        int cap=g_line_map.n?g_line_map.n+64:64;
        g_line_map.lines=(int*)xrealloc(g_line_map.lines,(size_t)cap*sizeof(int));
    }
    g_line_map.lines[g_line_map.n++]=line;
}
typedef struct { int kind; const char *var; Value container; int index; } MutTarget;
static Value mut_load(Env*e,Value receiver,MutTarget*t);
static void mut_store(Env*e,MutTarget*t,Value result);
static int rt_attr_index(const char *name){for(int i=0;i<g_attrs.n;i++)if(!strcmp(g_attrs.names[i],name))return i;return -1;}
static int rt_has_attr(const char *name){return rt_attr_index(name)>=0;}
static Value rt_attr_value(const char *name,Value fallback){int i=rt_attr_index(name);return i>=0?g_attrs.values[i]:fallback;}
static void die(const char *msg) {
    snprintf(g_last_error,sizeof g_last_error,"%s",msg?msg:"runtime error");
    if(!g_custom_error_pending)snprintf(g_last_error_kind,sizeof g_last_error_kind,"Runtime Error");
    g_custom_error_pending=0;
    if (g_try_jmp) longjmp(*g_try_jmp, 1);
    if (strcmp(g_last_error_kind,"Runtime Error")==0)
        fprintf(stderr, "runtime error: %s\n", g_last_error);
    else
        fprintf(stderr, "runtime error (%s): %s\n", g_last_error_kind, g_last_error);
    if(g_source_path&&*g_source_path)fprintf(stderr,"  at %s\n",g_source_path);
    if(getenv("ARTURO_NATIVE_TRACE")){void *frames[64];int count=backtrace(frames,64);backtrace_symbols_fd(frames,count,2);}
    exit(1);
}
void rt_error(const char *msg) { die(msg); }

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { die("out of memory"); }
    return p;
}
static size_t checked_add_size(size_t left,size_t right,const char *operation){
    if(right>SIZE_MAX-left)die(operation);
    return left+right;
}
static size_t checked_mul_size(size_t left,size_t right,const char *operation){
    if(left&&right>SIZE_MAX/left)die(operation);
    return left*right;
}
static char *seg_text(Value v);
static Value runNode0(Env *e, IR *node);
static Value apply_tail_two(Env *e, Value second, Value last);
static Value applyFunc(Env *caller, Value fn, Value *argv, int n);
static int node_is_stmt(IR *node);
static int is_shadowable(const char *name);
static int fn_arity(const char *name);
static int utf8_codepoint(const char *s);
static int utf8_rune_len(const char *s);
static int rune_count(const char *s);
static const char *rune_offset(const char *s, int i);
static void rune_at(const char *s, char out[5]);
static const char *last_rune(const char *s);
static Value v_block_cpy(Value **items, int n);
static char *fstr(double f, char *out, size_t cap);
static char *val_str(Value v);
static Value runBlockValue(Env *e, Value block);
static void env_define_local(Env *e, const char *name, Value v);
static void env_capture(Env *e);
static void env_release(Env *e);
static Value b_replace(Env *e,Value *a,int n);
static const char *type_name(Value v);
static Value clone_value(Value v);
static Value normalize_value(Value source);
static int value_eq(Value a,Value b);
static const char *custom_unit_property(const char *unit);
static const char *custom_unit_symbol(const char *unit);
static const char *custom_unit_dimension(const char *unit);
static const char *canonical_unit(const char *unit);
static int metric_length_factor(const char *unit,double *factor);
static int metric_liter_factor(const char *unit,double *factor);
static int metric_second_factor(const char *unit,double *factor);
static int metric_gram_factor(const char *unit,double *factor);
static int metric_newton_factor(const char *unit,double *factor);
static int metric_named_factor(const char *unit,const char *suffix,double base,double *factor);
static int metric_information_factor(const char *unit,double *factor);
typedef struct UnitDef {const char *unit,*dimension;double factor;} UnitDef;
static const UnitDef *unit_def(const char *unit);
static Value b_specify(Env *e,Value *a,int n);
static double as_float(Value v);
static long as_int(Value v);
static void floating_fraction(double value,long *numerator,long *denominator);
static int actionParamCount(Value params);
static void bindActionChunk(Env *child,Value params,Value collection,int start,int count);
enum { ACTION_NORMAL=0,ACTION_CONTINUE=1,ACTION_STOP=2 };
static int isIRAction(Value action);
static int irActionSignal(Value action);
static Value eval_data_item(Env *e,Value v);
static int path_is_dyn(Value v);
static Value path_read(Env *e,Value p);
static Value index_at(Value cur,const char *key,Env *e);
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { die("out of memory"); }
    return q;
}
/* grow a shared block body to hold at least `need` elements */
static void block_grow(Block *b, int need){
    if (b->cap >= need) return;
    int ncap = b->cap ? b->cap : 4;
    while (ncap < need) ncap *= 2;
    b->items = (Value**)xrealloc(b->items, (size_t)ncap*sizeof(Value*));
    b->cap = ncap;
}

/* ---- value constructors ------------------------------------------------- */
Value v_null(void)  { Value v;v.k=V_NULL;return v; }
Value v_int(long i) { Value v;v.k=V_INT;v.u.i=i;return v; }
Value v_float(double f){Value v;v.k=V_FLOAT;v.u.f=f;return v; }
Value v_rational(long numerator,long denominator){
    if(denominator==0)die("division by zero");
    if(denominator<0){numerator=-numerator;denominator=-denominator;}
    long a=labs(numerator),b=labs(denominator);
    while(b){long r=a%b;a=b;b=r;}
    long g=a?a:1;
    Value v;v.k=V_RATIONAL;
    v.u.rational.num=numerator/g;v.u.rational.den=denominator/g;return v;
}
Value v_complex(double real,double imaginary){Value v;v.k=V_COMPLEX;v.u.complex.real=real;v.u.complex.imag=imaginary;return v;}
Value v_float_text(const char *text){if(!strcmp(text,"∞"))return v_float(INFINITY);if(!strcmp(text,"-∞"))return v_float(-INFINITY);return v_float(strtod(text,NULL));}
Value v_quantity(double amount,const char *unit){Value v;v.k=V_QUANTITY;v.u.quantity.amount=amount;v.u.quantity.unit=strdup(canonical_unit(unit?unit:""));v.u.quantity.integral=0;return v;}
Value v_quantity_int(long amount,const char *unit){Value v=v_quantity((double)amount,unit);v.u.quantity.integral=1;return v;}
Value v_unit(const char *unit){Value v=v_token(V_UNIT,canonical_unit(unit?unit:""));return v;}
Value v_date_iso(const char *iso){
    int y=0,mo=0,d=0,h=0,mi=0,s=0,oh=0,om=0;char sign='+';
    int got=sscanf(iso,"%d-%d-%dT%d:%d:%d%c%d:%d",&y,&mo,&d,&h,&mi,&s,&sign,&oh,&om);
    if(got<6)die("to :date");
    struct tm tmv;memset(&tmv,0,sizeof tmv);tmv.tm_year=y-1900;tmv.tm_mon=mo-1;tmv.tm_mday=d;tmv.tm_hour=h;tmv.tm_min=mi;tmv.tm_sec=s;
    time_t stamp=timegm(&tmv);
    if(got>=9){long off=(long)(oh*3600+om*60);stamp+=(sign=='-'?off:-off);}
    Value v;v.k=V_DATE;v.u.epoch=(long long)stamp;return v;
}
Value v_color_hex(const char *hex){
    const char *s=hex&&hex[0]=='#'?hex+1:hex;
    struct {const char *name;unsigned rgb;} named[]={
        {"black",0x000000},{"red",0xFF0000},{"green",0x00FF00},{"yellow",0xFFFF00},
        {"blue",0x0000FF},{"magenta",0xFF00FF},{"orange",0xFFA500},{"cyan",0x00FFFF},
        {"white",0xFFFFFF},{"gray",0x808080},{"grey",0x808080},{"lightblue",0xADD8E6},{NULL,0}
    };
    for(int i=0;named[i].name;i++)if(!strcasecmp(s,named[i].name)){Value v;v.k=V_COLOR;v.u.rgba=(named[i].rgb<<8)|0xffu;return v;}
    unsigned long raw=strtoul(s,NULL,16);size_t n=strlen(s);
    Value v;v.k=V_COLOR;v.u.rgba=(unsigned int)(n<=6?((raw<<8)|0xffu):raw);return v;
}
Value v_binary_text(const char *text){size_t n=strlen(text);Value v;v.k=V_BINARY;v.u.binary.data=(unsigned char*)xmalloc(n?n:1);memcpy(v.u.binary.data,text,n);v.u.binary.len=n;return v;}
static char *color_text(Value v){char *s=(char*)xmalloc(10);unsigned x=v.u.rgba;if((x&0xffu)==0xffu)snprintf(s,10,"#%02X%02X%02X",(x>>24)&255,(x>>16)&255,(x>>8)&255);else snprintf(s,10,"#%08X",x);return s;}
static Value color_rgba(int r,int g,int b,int a){Value v;v.k=V_COLOR;v.u.rgba=((unsigned)(r&255)<<24)|((unsigned)(g&255)<<16)|((unsigned)(b&255)<<8)|(unsigned)(a&255);return v;}
static int color_chan(Value v,int shift){return (int)((v.u.rgba>>shift)&255u);}
static Value b_blend(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_COLOR||a[1].k!=V_COLOR)die("blend: expected colors");return color_rgba((int)round((color_chan(a[0],24)+color_chan(a[1],24))/2.0),(int)round((color_chan(a[0],16)+color_chan(a[1],16))/2.0),(int)round((color_chan(a[0],8)+color_chan(a[1],8))/2.0),(int)round((color_chan(a[0],0)+color_chan(a[1],0))/2.0));}
static Value color_value(Value v,double percent){int r=color_chan(v,24),g=color_chan(v,16),b=color_chan(v,8),a=color_chan(v,0),sign=percent<0?-1:1;double p=fabs(percent);r+=sign*(int)round(r*p);g+=sign*(int)round(g*p);b+=sign*(int)round(b*p);if(r>255)r=255;if(g>255)g=255;if(b>255)b=255;if(r<0)r=0;if(g<0)g=0;if(b<0)b=0;return color_rgba(r,g,b,a);}
static Value b_darken(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_COLOR)die("darken: expected color");return color_value(a[0],-as_float(a[1]));}
static Value b_lighten(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_COLOR)die("lighten: expected color");return color_value(a[0],as_float(a[1]));}
static void rgb_hsl(Value v,double *h,double *s,double *l){double r=color_chan(v,24)/255.0,g=color_chan(v,16)/255.0,b=color_chan(v,8)/255.0,mx=fmax(r,fmax(g,b)),mn=fmin(r,fmin(g,b)),d=mx-mn;*l=(mx+mn)/2.0;if(d==0){*h=0;*s=0;return;}*s=d/(1.0-fabs(2.0*(*l)-1.0));if(mx==r)*h=60.0*fmod((g-b)/d,6.0);else if(mx==g)*h=60.0*((b-r)/d+2.0);else *h=60.0*((r-g)/d+4.0);if(*h<0)*h+=360.0;}
static double hue_rgb(double p,double q,double t){if(t<0)t+=1.0;if(t>1)t-=1.0;if(t<1.0/6.0)return p+(q-p)*6*t;if(t<0.5)return q;if(t<2.0/3.0)return p+(q-p)*(2.0/3.0-t)*6;return p;}
static Value hsl_rgb(double h,double s,double l,int alpha){double hn=h/360.0,r,g,b;if(s==0)r=g=b=l;else{double q=l<0.5?l*(1+s):l+s-l*s,p=2*l-q;r=hue_rgb(p,q,hn+1.0/3.0);g=hue_rgb(p,q,hn);b=hue_rgb(p,q,hn-1.0/3.0);}return color_rgba((int)round(r*255),(int)round(g*255),(int)round(b*255),alpha);}
static Value color_saturation(Value v,double diff){double h,s,l;rgb_hsl(v,&h,&s,&l);s+=s*diff;if(s<0)s=0;if(s>1)s=1;return hsl_rgb(h,s,l,color_chan(v,0));}
static Value b_saturate(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_COLOR)die("saturate: expected color");return color_saturation(a[0],as_float(a[1]));}
static Value b_desaturate(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_COLOR)die("desaturate: expected color");return color_saturation(a[0],-as_float(a[1]));}
static Value b_grayscale(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_COLOR)die("grayscale: expected color");return color_saturation(a[0],-1.0);}
static Value color_spin(Value v,double degrees){double h,s,l;rgb_hsl(v,&h,&s,&l);h=fmod(round(h)+degrees,360.0);if(h<0)h+=360.0;return hsl_rgb(h,s,l,color_chan(v,0));}
static Value b_invert(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_COLOR)die("invert: expected color");return color_spin(a[0],180.0);}
static Value color_block(Value *colors,int count){Value **items=(Value**)xmalloc((size_t)(count+1)*sizeof(Value*));for(int i=0;i<count;i++){items[i]=(Value*)xmalloc(sizeof(Value));*items[i]=colors[i];}return v_block(items,count);}
static Value b_palette(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_COLOR)die("palette: expected color");
    int count=rt_has_attr("size")?(int)as_int(rt_attr_value("size",v_int(6))):6;if(count<0)count=0;
    if(rt_has_attr("triad")){Value out[3]={a[0],color_spin(a[0],120),color_spin(a[0],240)};return color_block(out,3);}
    if(rt_has_attr("tetrad")){Value out[4]={a[0],color_spin(a[0],90),color_spin(a[0],180),color_spin(a[0],270)};return color_block(out,4);}
    if(rt_has_attr("split")){Value out[3]={a[0],color_spin(a[0],72),color_spin(a[0],216)};return color_block(out,3);}
    if(!rt_has_attr("analogous")&&!rt_has_attr("monochrome")&&!rt_has_attr("random")){Value out[1]={a[0]};return color_block(out,1);}
    Value *out=(Value*)xmalloc((size_t)(count+1)*sizeof(Value));
    if(rt_has_attr("analogous")){double h,s,l;rgb_hsl(a[0],&h,&s,&l);h=fmod(h-12.0*(count/2)+720.0,360.0);for(int i=0;i<count;i++)out[i]=hsl_rgb(fmod(h+12.0*i,360.0),s,l,color_chan(a[0],0));}
    else if(rt_has_attr("monochrome")){for(int i=0;i<count;i++){double factor=1.0-(double)i/(double)(count?count:1);out[i]=color_rgba((int)(color_chan(a[0],24)*factor),(int)(color_chan(a[0],16)*factor),(int)(color_chan(a[0],8)*factor),color_chan(a[0],0));}}
    else {for(int i=0;i<count;i++){Value base=color_spin(a[0],120.0*(i%3));double delta=((double)rand()/RAND_MAX)*0.4-0.2;out[i]=i==0?a[0]:color_value(base,delta);}}
    Value result=color_block(out,count);free(out);return result;
}

static Value b_crc(Env*e,Value*a,int n){
    (void)n;MutTarget target;Value source=mut_load(e,a[0],&target);if(source.k!=V_STR)die("crc: expected string");unsigned crc=0xffffffffu;
    for(const unsigned char *p=(const unsigned char*)source.u.s;*p;p++){crc^=*p;for(int bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u&-(int)(crc&1));}
    char out[9];snprintf(out,sizeof out,"%08X",crc^0xffffffffu);Value result=v_str(out);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
}
static Value base64_encode(const unsigned char *src,size_t len){
    static const char tab[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";size_t olen=4*((len+2)/3);char *out=(char*)xmalloc(olen+1);size_t i=0,j=0;
    while(i<len){unsigned a=src[i++],b=i<len?src[i++]:0,c=i<len?src[i++]:0;unsigned triple=(a<<16)|(b<<8)|c;out[j++]=tab[(triple>>18)&63];out[j++]=tab[(triple>>12)&63];out[j++]=tab[(triple>>6)&63];out[j++]=tab[triple&63];}
    if(len%3)out[olen-1]='=';if(len%3==1)out[olen-2]='=';out[olen]=0;Value r=v_str(out);free(out);return r;
}
static int base64_digit(int c){if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;}
static Value base64_decode(const char *src){
    size_t len=strlen(src),cap=len*3/4+4,used=0;char *out=(char*)xmalloc(cap+1);unsigned acc=0;int bits=0;
    for(size_t i=0;i<len;i++){if(src[i]=='=')break;int d=base64_digit((unsigned char)src[i]);if(d<0){if(isspace((unsigned char)src[i]))continue;die("decode: invalid base64");}acc=(acc<<6)|(unsigned)d;bits+=6;if(bits>=8){bits-=8;out[used++]=(char)((acc>>bits)&255);}}
    out[used]=0;Value r=v_str(out);free(out);return r;
}
static int url_safe(int c){return isalnum((unsigned char)c)||c=='-'||c=='_'||c=='.'||c=='~';}
static Value url_encode(Value v){
    const unsigned char *s=(const unsigned char*)v.u.s;size_t len=strlen(v.u.s),cap=len*3+1,j=0;char *out=(char*)xmalloc(cap);static const char hex[]="0123456789ABCDEF";
    int encodeSpaces=rt_has_attr("spaces"),keepSlashes=!rt_has_attr("slashes");
    for(size_t i=0;i<len;i++){int c=s[i];if(url_safe(c)||(c=='/'&&keepSlashes)){out[j++]=(char)c;}else if(c==' '&&!encodeSpaces){out[j++]='+';}else{out[j++]='%';out[j++]=hex[(c>>4)&15];out[j++]=hex[c&15];}}
    out[j]=0;Value r=v_str(out);free(out);return r;
}
static Value url_decode(Value v){
    const char *s=v.u.s;size_t len=strlen(s),j=0;char *out=(char*)xmalloc(len+1);
    for(size_t i=0;i<len;i++){if(s[i]=='+')out[j++]=' ';else if(s[i]=='%'&&i+2<len&&isxdigit((unsigned char)s[i+1])&&isxdigit((unsigned char)s[i+2])){char h[3]={s[i+1],s[i+2],0};out[j++]=(char)strtol(h,NULL,16);i+=2;}else out[j++]=s[i];}
    out[j]=0;Value r=v_str(out);free(out);return r;
}
static Value b_encode(Env*e,Value*a,int n){(void)n;MutTarget target;Value source=mut_load(e,a[0],&target);if(source.k!=V_STR)die("encode: expected string");Value result=rt_has_attr("url")?url_encode(source):base64_encode((unsigned char*)source.u.s,strlen(source.u.s));if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
static Value b_decode(Env*e,Value*a,int n){(void)n;MutTarget target;Value source=mut_load(e,a[0],&target);if(source.k!=V_STR)die("decode: expected string");Value result=rt_has_attr("url")?url_decode(source):base64_decode(source.u.s);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
static Value b_color(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_COLOR||a[1].k!=V_STR)die("color: expected color and string");
    if(getenv("ARTURO_NO_COLOR")||getenv("NO_COLOR"))return v_str(a[1].u.s);
    int r=color_chan(a[0],24),g=color_chan(a[0],16),b=color_chan(a[0],8);char prefix[64];
    snprintf(prefix,sizeof prefix,"\033[%s%s38;2;%d;%d;%dm",rt_has_attr("bold")?"1;":"",rt_has_attr("underline")?"4;":"",r,g,b);
    size_t z=strlen(prefix)+strlen(a[1].u.s)+(rt_has_attr("keep")?1:5);char *out=(char*)xmalloc(z);snprintf(out,z,"%s%s%s",prefix,a[1].u.s,rt_has_attr("keep")?"":"\033[0m");Value result=v_str(out);free(out);return result;
}
static void text_add(char **out,size_t *used,size_t *cap,const char *s){size_t z=strlen(s);if(*used+z+1>*cap){*cap=(*used+z+1)*2;*out=xrealloc(*out,*cap);}memcpy(*out+*used,s,z);*used+=z;(*out)[*used]=0;}
static Value escape_value(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_STR)die("escape: expected string");const unsigned char *s=(unsigned char*)a[0].u.s;size_t cap=strlen(a[0].u.s)*8+8,used=0;char *out=xmalloc(cap);out[0]=0;
    int json=rt_has_attr("json"),regexMode=rt_has_attr("regex"),shell=rt_has_attr("shell"),xml=rt_has_attr("xml"),html=rt_has_attr("html");
    if(shell){text_add(&out,&used,&cap,"'");for(;*s;s++){if(*s=='\'')text_add(&out,&used,&cap,"'\"'\"'");else{char one[2]={(char)*s,0};text_add(&out,&used,&cap,one);}}text_add(&out,&used,&cap,"'");Value r=v_str(out);free(out);return r;}
    if(!json&&!regexMode&&!xml&&!html)text_add(&out,&used,&cap,"\"");
    for(;*s;s++){
        char buf[16];
        if(regexMode){if(isalnum(*s)||*s=='_'){buf[0]=(char)*s;buf[1]=0;}else snprintf(buf,sizeof buf,"\\x%02X",*s);text_add(&out,&used,&cap,buf);continue;}
        if(xml||html){const char *entity=NULL;if(*s=='&')entity="&amp;";else if(*s=='<')entity="&lt;";else if(*s=='>')entity="&gt;";else if(*s=='\"')entity="&quot;";else if(*s=='\'')entity=html?"&#39;":"&apos;";if(entity)text_add(&out,&used,&cap,entity);else{buf[0]=(char)*s;buf[1]=0;text_add(&out,&used,&cap,buf);}continue;}
        const char *esc=NULL;if(*s=='\\')esc="\\\\";else if(*s=='\"')esc="\\\"";else if(*s=='\n')esc="\\n";else if(*s=='\r')esc="\\r";else if(*s=='\t')esc="\\t";else if(*s=='\b')esc="\\b";else if(*s=='\f')esc="\\f";
        if(esc)text_add(&out,&used,&cap,esc);else if(*s<32){snprintf(buf,sizeof buf,"\\u%04X",*s);text_add(&out,&used,&cap,buf);}else{buf[0]=(char)*s;buf[1]=0;text_add(&out,&used,&cap,buf);}
    }
    if(!json&&!regexMode&&!xml&&!html)text_add(&out,&used,&cap,"\"");Value r=v_str(out);free(out);return r;
}
static int hex_digit_value(int c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static Value unescape_value(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_STR)die("unescape: expected string");const char *s=a[0].u.s;size_t len=strlen(s),used=0;char *out=xmalloc(len+1);
    for(size_t i=0;i<len;){
        if((rt_has_attr("xml")||rt_has_attr("html"))&&s[i]=='&'){const char *semi=strchr(s+i,';');if(semi){size_t z=(size_t)(semi-(s+i))+1;int c=-1;if(z==5&&!strncmp(s+i,"&amp;",5))c='&';else if(z==4&&!strncmp(s+i,"&lt;",4))c='<';else if(z==4&&!strncmp(s+i,"&gt;",4))c='>';else if(z==6&&!strncmp(s+i,"&quot;",6))c='\"';else if(z==6&&!strncmp(s+i,"&apos;",6))c='\'';else if(s[i+1]=='#')c=(int)strtol(s+i+2,NULL,s[i+2]=='x'||s[i+2]=='X'?16:10);if(c>=0&&c<128){out[used++]=(char)c;i+=z;continue;}}}
        if(s[i]=='\\'&&i+1<len){int c=s[++i];if(c=='n')out[used++]='\n';else if(c=='r')out[used++]='\r';else if(c=='t')out[used++]='\t';else if(c=='b')out[used++]='\b';else if(c=='f')out[used++]='\f';else if(c=='u'&&i+4<len){int v=0,ok=1;for(int k=1;k<=4;k++){int h=hex_digit_value(s[i+k]);if(h<0){ok=0;break;}v=v*16+h;}if(ok&&v<128){out[used++]=(char)v;i+=4;}else out[used++]=(char)c;}else out[used++]=(char)c;i++;continue;}
        out[used++]=s[i++];
    }
    out[used]=0;Value r=v_str(out);free(out);return r;
}
static Value wordwrap_value(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_STR)die("wordwrap: expected string");long width=rt_has_attr("at")?as_int(rt_attr_value("at",v_int(80))):80;if(width<1)width=1;char *copy=strdup(a[0].u.s),*save=NULL;size_t cap=strlen(copy)*2+2,used=0,line=0;char *out=xmalloc(cap);out[0]=0;
    for(char *word=strtok_r(copy," \t\r\n",&save);word;word=strtok_r(NULL," \t\r\n",&save)){size_t z=strlen(word);if(line&&line+1+z>(size_t)width){text_add(&out,&used,&cap,"\n");line=0;}else if(line){text_add(&out,&used,&cap," ");line++;}text_add(&out,&used,&cap,word);line+=z;}
    Value r=v_str(out);free(out);free(copy);return r;
}
static struct {const char *name;int arity;} DECLARED_ARITIES[]={
    {"fold",3},
#include "intrinsic_arity.inc"
    {NULL,0}
};
/* ---- name indexes ------------------------------------------------------- */
/* Static name tables (builtins, declared arities) are consulted on every
 * intrinsic call. A lazily built open-addressed index turns the linear
 * strcmp scan into a hash probe. */
typedef struct { const char *name; int index; } NameIndexSlot;
typedef struct { NameIndexSlot *slots; size_t cap; int built; } NameIndex;
static size_t name_hash(const char *s){
    size_t h=(size_t)1469598103934665603ull;
    while(*s){h^=(unsigned char)*s++;h*=(size_t)1099511628211ull;}
    return h;
}
typedef struct { char **slots; size_t cap,used; } InternTable;
static InternTable g_intern;
static void intern_grow(void){
    size_t cap=g_intern.cap?g_intern.cap*2:1024;char **slots=(char**)xmalloc(cap*sizeof(char*));memset(slots,0,cap*sizeof(char*));
    for(size_t i=0;i<g_intern.cap;i++)if(g_intern.slots[i]){size_t at=name_hash(g_intern.slots[i])&(cap-1);while(slots[at])at=(at+1)&(cap-1);slots[at]=g_intern.slots[i];}
    free(g_intern.slots);g_intern.slots=slots;g_intern.cap=cap;
}
static const char *intern(const char *name){
    if(!name)return NULL;if((g_intern.used+1)*2>g_intern.cap)intern_grow();size_t at=name_hash(name)&(g_intern.cap-1);
    while(g_intern.slots[at]){if(!strcmp(g_intern.slots[at],name))return g_intern.slots[at];at=(at+1)&(g_intern.cap-1);}
    char *copy=strdup(name);g_intern.slots[at]=copy;g_intern.used++;return copy;
}
static void name_index_build(NameIndex *ix,int count,const char *(*name_at)(int)){
    size_t cap=16;while(cap<(size_t)count*2)cap*=2;
    ix->slots=(NameIndexSlot*)calloc(cap,sizeof *ix->slots);
    if(!ix->slots){fprintf(stderr,"runtime error: out of memory\n");exit(1);}
    ix->cap=cap;
    /* first definition wins, matching the linear scan's semantics */
    for(int i=count-1;i>=0;i--){
        const char *name=name_at(i);size_t j=name_hash(name)&(cap-1);
        while(ix->slots[j].name&&strcmp(ix->slots[j].name,name))j=(j+1)&(cap-1);
        ix->slots[j].name=name;ix->slots[j].index=i;
    }
    ix->built=1;
}
static int name_index_find(const NameIndex *ix,const char *name){
    size_t j=name_hash(name)&(ix->cap-1);
    while(ix->slots[j].name){if(!strcmp(ix->slots[j].name,name))return ix->slots[j].index;j=(j+1)&(ix->cap-1);}
    return -1;
}

/* ---- IR opcodes --------------------------------------------------------- */
/* Generated code names IR operations by string.  The interpreter used to
 * re-compare that string against every operation it knew on each visit; intern
 * it once at construction and dispatch on the integer instead. */
typedef enum { OP_OTHER=0, OP___SEQ, OP_BLOCK, OP_BREAK, OP_CALL, OP_CASE, OP_CONST, OP_CONSTRUCTOR, OP_CONTINUE, OP_DEFINE, OP_DICTIONARY, OP_DO, OP_ENSURE, OP_FUNCTION, OP_HIGHER, OP_IF, OP_INTRINSIC, OP_IS, OP_LET, OP_LOAD, OP_LOOP, OP_METHOD, OP_PASSTHROUGH, OP_RANGE, OP_RETURN, OP_SWITCH, OP_THROWS_Q, OP_TRY, OP_UNLESS, OP_UNTIL, OP_USING, OP_WHEN, OP_WHILE, OP_WORD } IROpcode;
static const char *const OP_NAMES[]={"__seq", "block", "break", "call", "case", "const", "constructor", "continue", "define", "dictionary", "do", "ensure", "function", "higher", "if", "intrinsic", "is", "let", "load", "loop", "method", "passthrough", "range", "return", "switch", "throws?", "try", "unless", "until", "using", "when", "while", "word"};
static NameIndex g_op_index;
static const char *op_name_at(int i){return OP_NAMES[i];}
static int opcode_of(const char *op){
    if(!op)return OP_OTHER;
    if(!g_op_index.built)name_index_build(&g_op_index,(int)(sizeof OP_NAMES/sizeof *OP_NAMES),op_name_at);
    int i=name_index_find(&g_op_index,op);
    return i<0?OP_OTHER:i+1;
}
enum { IRF_ARITH_REF=1, IRF_SHADOWABLE=2 };
/* index of `name` in a NULL-terminated static list, hashed on first use */
typedef struct { const char *const *list; NameIndex ix; } NameList;
static int name_list_find(NameList *nl,const char *name){
    if(!nl->ix.built){
        int count=0;while(nl->list[count])count++;
        size_t cap=16;while(cap<(size_t)count*2)cap*=2;
        nl->ix.slots=(NameIndexSlot*)calloc(cap,sizeof *nl->ix.slots);
        if(!nl->ix.slots){fprintf(stderr,"runtime error: out of memory\n");exit(1);}
        nl->ix.cap=cap;
        for(int i=count-1;i>=0;i--){size_t j=name_hash(nl->list[i])&(cap-1);while(nl->ix.slots[j].name&&strcmp(nl->ix.slots[j].name,nl->list[i]))j=(j+1)&(cap-1);nl->ix.slots[j].name=nl->list[i];nl->ix.slots[j].index=i;}
        nl->ix.built=1;
    }
    return name_index_find(&nl->ix,name);
}
static int builtin_index_of(const char *name);
static NameIndex g_arity_index;
static const char *declared_arity_name_at(int i){return DECLARED_ARITIES[i].name;}
static int declared_arity_of(const char *name){
    if(!g_arity_index.built){int count=0;while(DECLARED_ARITIES[count].name)count++;name_index_build(&g_arity_index,count,declared_arity_name_at);}
    int i=name_index_find(&g_arity_index,name);
    return i<0?-1:DECLARED_ARITIES[i].arity;
}
static int function_param_count(Value fn){if(fn.k!=V_FUNC||!fn.u.fn.params)return 0;IR *p=fn.u.fn.params;if(p->op&&(p->opcode==OP_BLOCK))return p->nargs;return 1;}
static Value b_arity(Env*e,Value*a,int n){
    (void)a;(void)n;int count=0;while(DECLARED_ARITIES[count].name)count++;for(Env *frame=e;frame;frame=frame->parent)for(int i=0;i<frame->n;i++)if(frame->vals[i].k==V_FUNC)count++;
    char **keys=xmalloc((size_t)(count+1)*sizeof(char*));Value *vals=xmalloc((size_t)(count+1)*sizeof(Value));int used=0;
    for(int i=0;DECLARED_ARITIES[i].name;i++){keys[used]=strdup(DECLARED_ARITIES[i].name);vals[used++]=v_int(DECLARED_ARITIES[i].arity);}
    for(Env *frame=e;frame;frame=frame->parent)for(int i=0;i<frame->n;i++)if(frame->vals[i].k==V_FUNC){keys[used]=strdup(frame->names[i]);vals[used++]=v_int(function_param_count(frame->vals[i]));}
    return v_dict(keys,vals,used);
}
static Value b_symbols(Env*e,Value*a,int n){
    (void)a;(void)n;int count=0;while(DECLARED_ARITIES[count].name)count++;for(Env *frame=e;frame;frame=frame->parent)for(int i=0;i<frame->n;i++)if(frame->names[i][0]&&islower((unsigned char)frame->names[i][0]))count++;
    char **keys=xmalloc((size_t)(count+1)*sizeof(char*));Value *vals=xmalloc((size_t)(count+1)*sizeof(Value));int used=0;
    for(int i=0;DECLARED_ARITIES[i].name;i++)if(DECLARED_ARITIES[i].name[0]&&islower((unsigned char)DECLARED_ARITIES[i].name[0])){keys[used]=strdup(DECLARED_ARITIES[i].name);vals[used++]=v_token(V_BUILTIN,DECLARED_ARITIES[i].name);}
    for(Env *frame=e;frame;frame=frame->parent)for(int i=0;i<frame->n;i++)if(frame->names[i][0]&&islower((unsigned char)frame->names[i][0])){int seen=0;for(int j=0;j<used;j++)if(!strcmp(keys[j],frame->names[i]))seen=1;if(!seen){keys[used]=strdup(frame->names[i]);vals[used++]=frame->vals[i];}}
    return v_dict(keys,vals,used);
}
static Value b_var(Env*e,Value*a,int n){(void)n;char *name=val_str(a[0]);if(!env_bound(e,name)){if(rt_builtin_known(name)||declared_arity_of(name)>=0){Value result=v_token(V_BUILTIN,name);free(name);return result;}free(name);die("var: identifier not found");}Value result=env_get(e,name);free(name);return result;}
static Value b_unset(Env*e,Value*a,int n){
    (void)n;char *name=val_str(a[0]);const char *key=intern(name);for(Env *frame=e;frame;frame=frame->parent)for(int i=0;i<frame->n;i++)if(frame->names[i]==key){for(int j=i+1;j<frame->n;j++){frame->names[j-1]=frame->names[j];frame->vals[j-1]=frame->vals[j];}frame->n--;free(name);return v_null();}free(name);return v_null();
}
static int is_currency_unit(const char *unit){
    static const char *currencies[]={
        "AED","ALL","ARS","AUD","BGN","BHD","BNB","BND","BOB","BRL","BTC","BWP",
        "CAD","CHF","CLP","CNY","COP","CRC","CZK","DKK","DOP","DZD","EGP","ETB","ETH",
        "EUR","FJD","GBP","HKD","HNL","HRK","HUF","IDR","ILS","INR","IRR","ISK","JMD",
        "JOD","JPY","KES","KRW","KWD","KYD","KZT","LBP","LKR","MAD","MDL","MKD","MXN",
        "MUR","MYR","NAD","NGN","NIO","NOK","NPR","NZD","OMR","PAB","PEN","PGK","PHP",
        "PKR","PLN","PYG","QAR","RON","RSD","RUB","SAR","SCR","SEK","SGD","SLL","SOS",
        "SVC","THB","TND","TRY","TTD","TWD","TZS","UAH","UGX","USD","UYU","UZS","VES",
        "VND","XAF","XAG","XAU","XOF","YER","ZAR","ZMW",NULL
    };
    for(int i=0;currencies[i];i++)if(!strcmp(unit,currencies[i]))return 1;
    return 0;
}
static const char *canonical_unit(const char *unit){
    if(!strcmp(unit,"dollar")||!strcmp(unit,"dollars"))return "USD";
    static const struct {const char *alias,*canonical;} aliases[]={
        {"meter","m"},{"meters","m"},{"metre","m"},{"metres","m"},{"inch","in"},{"inches","in"},{"foot","ft"},{"feet","ft"},{"yard","yd"},{"yards","yd"},{"fathom","ftm"},{"fathoms","ftm"},{"rods","rod"},{"mile","mi"},{"miles","mi"},{"furlong","fur"},{"furlongs","fur"},{"nauticalMile","nmi"},{"nauticalMiles","nmi"},{"angstrom","ang"},{"angstroms","ang"},{"astronomicalUnit","au"},{"astronomicalUnits","au"},{"lightYear","ly"},{"lightYears","ly"},{"pixel","px"},{"pixels","px"},{"point","pt"},{"points","pt"},{"pica","pc"},{"picas","pc"},{"squareInch","sqin"},{"squareInches","sqin"},{"squareFoot","sqft"},{"squareFeet","sqft"},{"acre","ac"},{"acres","ac"},{"ares","are"},{"hectare","ha"},{"hectares","ha"},{"barns","barn"},{"l","L"},{"liter","L"},{"liters","L"},{"gals","gal"},{"gallon","gal"},{"gallons","gal"},{"barrel","bbl"},{"barrels","bbl"},{"quart","qt"},{"quarts","qt"},{"pint","p"},{"pints","p"},{"cups","cup"},{"fluidOunce","floz"},{"fluidOunces","floz"},{"tablespoon","tbsp"},{"tablespoons","tbsp"},{"teaspoon","tsp"},{"teaspoons","tsp"},{"bushel","bu"},{"bushels","bu"},{"cords","cord"},{"second","s"},{"seconds","s"},{"minute","min"},{"minutes","min"},{"hour","h"},{"hours","h"},{"hr","h"},{"hrs","h"},{"days","day"},{"week","wk"},{"weeks","wk"},{"month","mo"},{"months","mo"},{"year","yr"},{"years","yr"},{"pound","lb"},{"pounds","lb"},{"slugs","slug"},{"ounce","oz"},{"ounces","oz"},{"carat","ct"},{"carats","ct"},{"tonne","t"},{"tonnes","t"},{"metricTon","t"},{"metricTons","t"},{"tons","ton"},{"shortTon","ton"},{"shortTons","ton"},{"longTon","lt"},{"longTons","lt"},{"stone","st"},{"stones","st"},{"dalton","Da"},{"daltons","Da"},{"AMU","Da"},{"grain","gr"},{"grains","gr"},{"pennyweight","dwt"},{"pennyweights","dwt"},{"troyOunce","ozt"},{"troyOunces","ozt"},{"troyPound","lbt"},{"troyPounds","lbt"},{"meterPerSecond","mps"},{"metersPerSecond","mps"},{"kilometerPerHour","kph"},{"kilometersPerHour","kph"},{"milePerHour","mph"},{"milesPerHour","mph"},{"knot","kn"},{"knots","kn"},{"footPerSecond","fps"},{"feetPerSecond","fps"},{"machs","mach"},{"galileo","Gal"},{"galileos","Gal"},{NULL,NULL}
    };
    for(int i=0;aliases[i].alias;i++)if(!strcmp(unit,aliases[i].alias))return aliases[i].canonical;
    static const struct {const char *alias,*canonical;} forceAliases[]={{"newton","N"},{"newtons","N"},{"dyne","dyn"},{"dynes","dyn"},{"poundal","pdl"},{"poundals","pdl"},{"poundsForce","lbf"},{"kilogramsForce","kgf"},{NULL,NULL}};
    for(int i=0;forceAliases[i].alias;i++)if(!strcmp(unit,forceAliases[i].alias))return forceAliases[i].canonical;
    static const struct {const char *alias,*canonical;} pressureAliases[]={{"millimeterOfMercury","mmHg"},{"millimetersOfMercury","mmHg"},{"poundPerSquareInch","psi"},{"poundsPerSquareInch","psi"},{"atmosphere","atm"},{"atmospheres","atm"},{"pascal","Pa"},{"pascals","Pa"},{"barye","Ba"},{"baryes","Ba"},{"torr","Torr"},{"torrs","Torr"},{"bars","bar"},{"pieze","pz"},{NULL,NULL}};
    for(int i=0;pressureAliases[i].alias;i++)if(!strcmp(unit,pressureAliases[i].alias))return pressureAliases[i].canonical;
    static const struct {const char *alias,*canonical;} energyAliases[]={{"britishThermalUnit","BTU"},{"britishThermalUnits","BTU"},{"electronVolt","eV"},{"electronVolts","eV"},{"wattHour","Wh"},{"wattHours","Wh"},{"calorie","cal"},{"calories","cal"},{"thermie","th"},{"thermies","th"},{"joule","J"},{"joules","J"},{"therm","thm"},{"therms","thm"},{"ergs","erg"},{NULL,NULL}};
    for(int i=0;energyAliases[i].alias;i++)if(!strcmp(unit,energyAliases[i].alias))return energyAliases[i].canonical;
    if(!strcmp(unit,"watt")||!strcmp(unit,"watts"))return "W";
    if(!strcmp(unit,"horsepower"))return "hp";
    static const struct {const char *alias,*canonical;} currentAliases[]={{"amp","A"},{"amps","A"},{"ampere","A"},{"amperes","A"},{"abampere","abA"},{"abamperes","abA"},{"biot","Bi"},{"biots","Bi"},{NULL,NULL}};
    for(int i=0;currentAliases[i].alias;i++)if(!strcmp(unit,currentAliases[i].alias))return currentAliases[i].canonical;
    static const struct {const char *alias,*canonical;} electricalAliases[]={{"volt","V"},{"volts","V"},{"statvolt","statV"},{"statvolts","statV"},{"abvolt","abV"},{"abvolts","abV"},{"ohm","Ohm"},{"ohms","Ohm"},{"statohm","statOhm"},{"statohms","statOhm"},{"abohm","abOhm"},{"abohms","abOhm"},{"siemens","S"},{"coulomb","C"},{"coulombs","C"},{"statcoulomb","statC"},{"statcoulombs","statC"},{"abcoulomb","abC"},{"abcoulombs","abC"},{"franklin","Fr"},{"franklins","Fr"},{"farad","F"},{"farads","F"},{"henry","H"},{"henrys","H"},{"abhenry","abH"},{"abhenrys","abH"},{"weber","Wb"},{"webers","Wb"},{"maxwell","Mx"},{"maxwells","Mx"},{"tesla","T"},{"teslas","T"},{"gauss","G"},{NULL,NULL}};
    for(int i=0;electricalAliases[i].alias;i++)if(!strcmp(unit,electricalAliases[i].alias))return electricalAliases[i].canonical;
    if(!strcmp(unit,"oC")||!strcmp(unit,"celsius"))return "degC";
    if(!strcmp(unit,"oF")||!strcmp(unit,"fahrenheit"))return "degF";
    if(!strcmp(unit,"oR")||!strcmp(unit,"rankine"))return "degR";
    static const struct {const char *alias,*canonical;} infoAliases[]={{"byte","B"},{"bytes","B"},{"bit","b"},{"bits","b"},{"kibibyte","KiB"},{"kibibytes","KiB"},{"mebibyte","MiB"},{"mebibytes","MiB"},{"gibibyte","GiB"},{"gibibytes","GiB"},{"tebibyte","TiB"},{"tebibytes","TiB"},{"pebibyte","PiB"},{"pebibytes","PiB"},{"exbibyte","EiB"},{"exbibytes","EiB"},{NULL,NULL}};
    for(int i=0;infoAliases[i].alias;i++)if(!strcmp(unit,infoAliases[i].alias))return infoAliases[i].canonical;
    static const struct {const char *alias,*canonical;} angleAliases[]={{"radian","rad"},{"radians","rad"},{"degree","deg"},{"degrees","deg"},{"gradian","grad"},{"gradians","grad"},{"arcminute","arcmin"},{"arcminutes","arcmin"},{"arcsecond","arcsec"},{"arcseconds","arcsec"},{NULL,NULL}};
    for(int i=0;angleAliases[i].alias;i++)if(!strcmp(unit,angleAliases[i].alias))return angleAliases[i].canonical;
    if(!strcmp(unit,"katal")||!strcmp(unit,"katals"))return "kat";
    if(!strcmp(unit,"hertz"))return "Hz";
    static const struct {const char *alias,*canonical;} radiationAliases[]={{"becquerel","Bq"},{"becquerels","Bq"},{"curie","Ci"},{"curies","Ci"},{"gray","Gy"},{"grays","Gy"},{"sievert","Sv"},{"sieverts","Sv"},{"roentgen","R"},{"roentgens","R"},{NULL,NULL}};
    for(int i=0;radiationAliases[i].alias;i++)if(!strcmp(unit,radiationAliases[i].alias))return radiationAliases[i].canonical;
    static const struct {const char *alias,*canonical;} finalAliases[]={{"poise","P"},{"poises","P"},{"stokes","St"},{"rpms","rpm"},{"clos","clo"},{"candela","lx"},{"candelas","lx"},{"lux","lx"},{"luxes","lx"},{"lambert","Lb"},{"lamberts","Lb"},{"lumen","lm"},{"lumens","lm"},{NULL,NULL}};
    for(int i=0;finalAliases[i].alias;i++)if(!strcmp(unit,finalAliases[i].alias))return finalAliases[i].canonical;
    return unit;
}
static const char *unit_property(const char *unit){
    if(!unit)return "unknown";
    const char *custom=custom_unit_property(unit);if(custom)return custom;
    if(is_currency_unit(unit))return "currency";
    const UnitDef *knownDefinition=unit_def(unit);if(knownDefinition)return knownDefinition->dimension;
    if(!strcmp(unit,"W")||!strcmp(unit,"J/s")||!strcmp(unit,"N.m/s")||!strcmp(unit,"hp"))return "power";
    double powerFactor;if(metric_named_factor(unit,"W",1.0,&powerFactor))return "power";
    if(!strcmp(unit,"V"))return "potential";
    if(!strcmp(unit,"J"))return "energy";
    if(!strcmp(unit,"N"))return "force";
    if(strstr(unit,"/km2."))return "pressure";
    if(strchr(unit,'/')){const UnitDef *definition=unit_def(unit);if(definition)return definition->dimension;const char *slash=strrchr(unit,'/');char numerator[128];size_t nl=(size_t)(slash-unit);if(nl>=sizeof numerator)nl=sizeof numerator-1;memcpy(numerator,unit,nl);numerator[nl]=0;size_t dz=strlen(slash+1);int squared=dz>1&&(slash[dz]=='2');if(squared&&strchr(numerator,'.'))return "force";static const char *speedNumerators[]={"mps","kph","mph","kn","fps","mach",NULL};for(int i=0;speedNumerators[i];i++)if(!strcmp(numerator,speedNumerators[i]))return "acceleration";return squared?"acceleration":"speed";}
    static const char *speedUnits[]={"mps","kph","mph","kn","fps","mach",NULL};for(int i=0;speedUnits[i];i++)if(!strcmp(unit,speedUnits[i]))return "speed";
    if(!strcmp(unit,"Gal"))return "acceleration";
    static const char *forceUnits[]={"N","dyn","lbf","kgf","pdl",NULL};for(int i=0;forceUnits[i];i++)if(!strcmp(unit,forceUnits[i]))return "force";
    static const char *pressureUnits[]={"Pa","atm","bar","pz","Ba","mmHg","psi","Torr",NULL};for(int i=0;pressureUnits[i];i++)if(!strcmp(unit,pressureUnits[i]))return "pressure";
    double pressureFactor;if(metric_named_factor(unit,"Pa",1.0,&pressureFactor)||metric_named_factor(unit,"bar",100000.0,&pressureFactor))return "pressure";
    if(strstr(unit,"/m2")||strstr(unit,"/cm2")||strstr(unit,"/km2"))return "pressure";
    static const char *energyUnits[]={"J","Wh","cal","BTU","eV","erg","th","thm",NULL};for(int i=0;energyUnits[i];i++)if(!strcmp(unit,energyUnits[i]))return "energy";
    double energyFactor;if(metric_named_factor(unit,"J",1.0,&energyFactor)||metric_named_factor(unit,"Wh",3600.0,&energyFactor)||metric_named_factor(unit,"cal",4.184,&energyFactor)||metric_named_factor(unit,"eV",1.602176565e-19,&energyFactor))return "energy";
    if(!strcmp(unit,"N.m")||!strcmp(unit,"kg.m2/s2"))return "energy";
    static const char *currentUnits[]={"A","statA","abA","Bi",NULL};for(int i=0;currentUnits[i];i++)if(!strcmp(unit,currentUnits[i]))return "current";
    double currentFactor;if(metric_named_factor(unit,"A",1.0,&currentFactor))return "current";
    size_t z=strlen(unit);if((z&&unit[z-1]=='2')||strstr(unit,"²"))return "area";if((z&&unit[z-1]=='3')||strstr(unit,"³"))return "volume";
    static const char *areaUnits[]={"sqin","sqft","ac","are","ha","barn",NULL};for(int i=0;areaUnits[i];i++)if(!strcmp(unit,areaUnits[i]))return "area";
    static const char *volumeUnits[]={"gal","bbl","qt","p","cup","floz","tbsp","tsp","bu","cord",NULL};for(int i=0;volumeUnits[i];i++)if(!strcmp(unit,volumeUnits[i]))return "volume";
    double literFactor;if(metric_liter_factor(unit,&literFactor))return "volume";
    static const char *length[]={"m","cm","mm","km","yd","mi","ft","in","ftm","rod","fur","nmi","ang","au","ly","meter","meters","metre","metres","inch","inches","foot","feet","yard","yards","fathom","fathoms","rods","mile","miles","furlong","furlongs","angstrom","angstroms","lightYear","lightYears","nauticalMile","nauticalMiles","astronomicalUnit","astronomicalUnits","point","points","pica","picas","pixel","pixels",NULL};for(int i=0;length[i];i++)if(!strcmp(unit,length[i]))return "length";
    double metricFactor;if(metric_length_factor(unit,&metricFactor))return "length";
    static const char *timeUnits[]={"s","ms","min","minute","minutes","h","hour","hours","day","days","week","weeks","wk","month","months","mo","year","years","yr",NULL};for(int i=0;timeUnits[i];i++)if(!strcmp(unit,timeUnits[i]))return "time";
    double secondFactor;if(metric_second_factor(unit,&secondFactor))return "time";
    double gramFactor;if(metric_gram_factor(unit,&gramFactor))return "mass";
    static const char *massUnits[]={"lb","slug","oz","ct","t","ton","lt","st","Da","gr","dwt","ozt","lbt",NULL};for(int i=0;massUnits[i];i++)if(!strcmp(unit,massUnits[i]))return "mass";
    return unit;
}
static const char *quantity_unit(Value v){if(v.k==V_QUANTITY)return v.u.quantity.unit;if(v.k==V_UNIT||v.k==V_STR||v.k==V_WORD||v.k==V_LITERAL||v.k==V_SYMBOL||v.k==V_SYMBOLLITERAL)return canonical_unit(v.u.s);die("expected quantity or unit");return "";}
static Value b_property(Env*e,Value*a,int n){(void)e;(void)n;const char *p=unit_property(quantity_unit(a[0]));if(rt_has_attr("hash")){unsigned h=2166136261u;for(const unsigned char *s=(const unsigned char*)p;*s;s++)h=(h^*s)*16777619u;return v_int((long)h);}return v_str(p);}
static Value b_conformsp(Env*e,Value*a,int n){(void)e;(void)n;return v_bool(!strcmp(unit_property(quantity_unit(a[0])),unit_property(quantity_unit(a[1]))));}
static Value b_parse(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k==V_BLOCK)return a[0];if(a[0].k!=V_STR)die("parse: expected string or block");Value all=lex_source(a[0].u.s);if(rt_has_attr("data"))return all;if(all.u.block.b->n<1)return v_null();return *all.u.block.b->items[0];}
static Value translate_value(Env*e,Value*a,int n){(void)n;if(a[0].k!=V_STR||a[1].k!=V_DICT)die("translate: expected string and dictionary");Value current=v_str(a[0].u.s);for(int i=0;i<a[1].u.dict->n;i++){char *replacement=val_str(a[1].u.dict->vals[i]);Value av[3]={current,v_str(a[1].u.dict->keys[i]),v_str(replacement)};current=b_replace(e,av,3);free(replacement);}return current;}
static Value b_extract(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_STR)die("extract: expected path string");const char *path=a[0].u.s,*slash=strrchr(path,'/');const char *base=slash?slash+1:path;const char *dot=strrchr(base,'.');
    size_t dirLen=slash?(size_t)(slash-path):0,fileLen=dot?(size_t)(dot-base):strlen(base);char *dir=xmalloc(dirLen+1),*file=xmalloc(fileLen+1);memcpy(dir,path,dirLen);dir[dirLen]=0;memcpy(file,base,fileLen);file[fileLen]=0;const char *ext=dot?dot:"";
    if(rt_has_attr("directory")){Value r=v_str(dir);free(dir);free(file);return r;}if(rt_has_attr("basename")){Value r=v_str(base);free(dir);free(file);return r;}if(rt_has_attr("filename")){Value r=v_str(file);free(dir);free(file);return r;}if(rt_has_attr("extension")){Value r=v_str(ext);free(dir);free(file);return r;}
    char **keys=xmalloc(4*sizeof(char*));Value *vals=xmalloc(4*sizeof(Value));const char *names[]={"directory","basename","filename","extension"};const char *texts[]={dir,base,file,ext};for(int i=0;i<4;i++){keys[i]=strdup(names[i]);vals[i]=v_str(texts[i]);}free(dir);free(file);return v_dict(keys,vals,4);
}
static void template_literal_source(char **source,size_t *used,size_t *cap,const char *text,size_t len){
    if(!len)return;
    text_add(source,used,cap,"append '__template_output \"");
    for(size_t i=0;i<len;i++){
        char escaped[3]={0,0,0};
        if(text[i]=='\\'||text[i]=='\"'){escaped[0]='\\';escaped[1]=text[i];text_add(source,used,cap,escaped);}
        else if(text[i]=='\n')text_add(source,used,cap,"\\n");
        else if(text[i]=='\r')text_add(source,used,cap,"\\r");
        else if(text[i]=='\t')text_add(source,used,cap,"\\t");
        else {escaped[0]=text[i];text_add(source,used,cap,escaped);}
    }
    text_add(source,used,cap,"\" ");
}
static Value render_template_once(Env *e,const char *input){
    size_t cap=strlen(input)*3+128,used=0;char *source=xmalloc(cap);source[0]=0;const char *cursor=input;
    while(1){
        const char *open=strstr(cursor,"<||");
        if(!open){template_literal_source(&source,&used,&cap,cursor,strlen(cursor));break;}
        template_literal_source(&source,&used,&cap,cursor,(size_t)(open-cursor));
        const char *body=open+3,*close=strstr(body,"||>");
        if(!close){template_literal_source(&source,&used,&cap,open,strlen(open));break;}
        if(body<close&&*body=='='){
            text_add(&source,&used,&cap,"append '__template_output to :string ");body++;
        }
        size_t codeLen=(size_t)(close-body);char *code=xmalloc(codeLen+1);memcpy(code,body,codeLen);code[codeLen]=0;text_add(&source,&used,&cap,code);text_add(&source,&used,&cap," ");free(code);
        cursor=close+3;
    }
    Env *frame=env_new(e);Value output=v_block(xmalloc(sizeof(Value*)),0);env_define_local(frame,"__template_output",output);
    Value code=lex_source(source);(void)runBlockValue(frame,code);free(source);output=env_get(frame,"__template_output");
    size_t outCap=64,outUsed=0;char *joined=xmalloc(outCap);joined[0]=0;
    if(output.k==V_BLOCK)for(int i=0;i<output.u.block.b->n;i++){char *part=val_str(*output.u.block.b->items[i]);text_add(&joined,&outUsed,&outCap,part);free(part);}
    Value result=v_str(joined);free(joined);env_release(frame);return result;
}
static Value render_value(Env*e,Value*a,int n){
    (void)n;if(a[0].k!=V_STR)die("render: expected string");char *current=strdup(a[0].u.s);int rounds=rt_has_attr("once")?1:100;
    if(rt_has_attr("template")){
        for(int round=0;round<rounds&&strstr(current,"<||");round++){Value rendered=render_template_once(e,current);free(current);current=strdup(rendered.u.s);}
        Value result=v_str(current);free(current);return result;
    }
    for(int round=0;round<rounds;round++){const char *open=strchr(current,'|');if(!open)break;const char *close=strchr(open+1,'|');if(!close)break;size_t exprLen=(size_t)(close-open-1);char *expr=xmalloc(exprLen+1);memcpy(expr,open+1,exprLen);expr[exprLen]=0;Value evaluated;if(strchr(expr,'\\')){char *save=NULL,*base=strtok_r(expr,"\\",&save);evaluated=env_get(e,base);for(char *segment=strtok_r(NULL,"\\",&save);segment;segment=strtok_r(NULL,"\\",&save))evaluated=index_at(evaluated,segment,e);}else{Value code=lex_source(expr);evaluated=runBlockValue(e,code);}char *replacement=val_str(evaluated);size_t prefix=(size_t)(open-current),suffix=strlen(close+1),newLen=prefix+strlen(replacement)+suffix;char *next=xmalloc(newLen+1);memcpy(next,current,prefix);strcpy(next+prefix,replacement);strcpy(next+prefix+strlen(replacement),close+1);free(expr);free(replacement);free(current);current=next;}
    Value result=v_str(current);free(current);return result;
}
static char *express_value(Value v){
    if(v.k==V_QUANTITY){
        const char *symbol=custom_unit_symbol(v.u.quantity.unit);
        if(symbol){long numerator,denominator;floating_fraction(v.u.quantity.amount,&numerator,&denominator);char amount[96];if(denominator==1)snprintf(amount,sizeof amount,"%ld",numerator);else snprintf(amount,sizeof amount,"%ld:%ld",numerator,denominator);size_t z=strlen(amount)+strlen(symbol)+2;char *out=xmalloc(z);snprintf(out,z,"%s`%s",amount,symbol);return out;}
        {long numerator,denominator;floating_fraction(v.u.quantity.amount,&numerator,&denominator);char amount[96];if(denominator==1)snprintf(amount,sizeof amount,"%ld",numerator);else snprintf(amount,sizeof amount,"%ld:%ld",numerator,denominator);size_t z=strlen(amount)+strlen(v.u.quantity.unit)+2;char*out=xmalloc(z);snprintf(out,z,"%s`%s",amount,v.u.quantity.unit);return out;}
    }
    if(v.k==V_STR){size_t cap=strlen(v.u.s)*2+3,used=0;char *out=xmalloc(cap);out[0]=0;text_add(&out,&used,&cap,"\"");for(const char *p=v.u.s;*p;p++){if(*p=='\\')text_add(&out,&used,&cap,"\\\\");else if(*p=='\"')text_add(&out,&used,&cap,"\\\"");else if(*p=='\n')text_add(&out,&used,&cap,"\\n");else{char one[2]={*p,0};text_add(&out,&used,&cap,one);}}text_add(&out,&used,&cap,"\"");return out;}
    if(v.k==V_BLOCK){size_t cap=32,used=0;char *out=xmalloc(cap);out[0]=0;text_add(&out,&used,&cap,"[");for(int i=0;i<v.u.block.b->n;i++){if(i)text_add(&out,&used,&cap," ");char *part=express_value(*v.u.block.b->items[i]);text_add(&out,&used,&cap,part);free(part);}text_add(&out,&used,&cap,"]");return out;}
    if(v.k==V_DICT){size_t cap=32,used=0;char *out=xmalloc(cap);out[0]=0;text_add(&out,&used,&cap,"#[");for(int i=0;i<v.u.dict->n;i++){if(i)text_add(&out,&used,&cap," ");text_add(&out,&used,&cap,v.u.dict->keys[i]);text_add(&out,&used,&cap,": ");char *part=express_value(v.u.dict->vals[i]);text_add(&out,&used,&cap,part);free(part);}if(v.u.dict->n)text_add(&out,&used,&cap," ");text_add(&out,&used,&cap,"]");return out;}
    char *plain=val_str(v);if(v.k==V_CHAR){size_t z=strlen(plain)+3;char *out=xmalloc(z);snprintf(out,z,"'%s'",plain);free(plain);return out;}if(v.k==V_LITERAL){size_t z=strlen(plain)+2;char *out=xmalloc(z);snprintf(out,z,"'%s",plain);free(plain);return out;}if(v.k==V_TYPE){size_t z=strlen(plain)+2;char *out=xmalloc(z);snprintf(out,z,":%s",plain);free(plain);return out;}return plain;
}
static Value b_express(Env*e,Value*a,int n){(void)e;(void)n;char *s=express_value(a[0]);Value r=v_str(s);free(s);return r;}
static void inspect_value(Value v,int indent){
    if(v.k==V_BLOCK){
        printf("[ :block\n");
        for(int i=0;i<v.u.block.b->n;i++){
            Value item=*v.u.block.b->items[i];
            for(int j=0;j<indent+8;j++)putchar(' ');
            if(item.k==V_BLOCK)inspect_value(item,indent+8);
            else {
                char*s=val_str(item);const char*kind=type_name(item);const char*shown=s;
                if(item.k==V_STR&&strncmp(s,"@LBL:",5)==0){kind="label";shown=s+5;}
                else if(item.k==V_STR&&(!strcmp(s,"?")||!strcmp(s,"??")||!strcmp(s,"_")))kind="symbol";
                else if(item.k==V_STR)kind="literal";
                printf("%s :%s\n",shown,kind);free(s);
            }
        }
        for(int j=0;j<indent;j++)putchar(' ');printf("]\n");
        return;
    }
    char*s=val_str(v);printf("%s :%s\n",s,type_name(v));free(s);
}
static Value b_inspect(Env*e,Value*a,int n){(void)e;(void)n;inspect_value(a[0],0);return v_null();}
static Value b_benchmark(Env*e,Value*a,int n){
    (void)n;struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);if(a[0].k==V_BLOCK)(void)runBlockValue(e,a[0]);else die("benchmark: expected block");clock_gettime(CLOCK_MONOTONIC,&end);double ms=(end.tv_sec-start.tv_sec)*1000.0+(end.tv_nsec-start.tv_nsec)/1000000.0;
    if(rt_has_attr("get"))return v_quantity(ms,"ms");printf("[benchmark] time: %.3fs\n",ms/1000.0);return v_null();
}
static Value b_export(Env*e,Value*a,int n){(void)n;if(a[0].k!=V_DICT)die("export: expected module, object, or dictionary");for(int i=0;i<a[0].u.dict->n;i++)if(strncmp(a[0].u.dict->keys[i],"__",2))env_set(e,a[0].u.dict->keys[i],a[0].u.dict->vals[i]);return v_null();}
static Value b_alphabet(Env*e,Value*a,int n){
    (void)e;(void)n;char *locale=val_str(a[0]);int upper=rt_has_attr("upper"),lower=rt_has_attr("lower")||!upper,count=(lower?26:0)+(upper?26:0);Value **items=xmalloc((size_t)(count+1)*sizeof(Value*));int at=0;
    if(lower)for(char c='a';c<='z';c++){items[at]=xmalloc(sizeof(Value));*items[at++]=v_char(c);}if(upper)for(char c='A';c<='Z';c++){items[at]=xmalloc(sizeof(Value));*items[at++]=v_char(c);}free(locale);return v_block(items,at);
}
static Value b_digest(Env*e,Value*a,int n){
    (void)n;MutTarget target;Value source=mut_load(e,a[0],&target);if(source.k!=V_STR)die("digest: expected string");unsigned char bytes[20];unsigned length;
#ifdef __APPLE__
    if(rt_has_attr("sha")){CC_SHA1(source.u.s,(CC_LONG)strlen(source.u.s),bytes);length=CC_SHA1_DIGEST_LENGTH;}else{CC_MD5(source.u.s,(CC_LONG)strlen(source.u.s),bytes);length=CC_MD5_DIGEST_LENGTH;}
#else
    if(rt_has_attr("sha")){SHA1((unsigned char*)source.u.s,strlen(source.u.s),bytes);length=SHA_DIGEST_LENGTH;}else{MD5((unsigned char*)source.u.s,strlen(source.u.s),bytes);length=MD5_DIGEST_LENGTH;}
#endif
    char out[41];for(unsigned i=0;i<length;i++)snprintf(out+i*2,3,"%02x",bytes[i]);out[length*2]=0;Value result=v_str(out);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
}

/* Nim 2's 64-bit hashes, used by the pinned Arturo VM.  Arturo hashes the
 * ValueKind first, mixes the payload with `!&`, then applies `!$`.  Keeping
 * this algorithm here makes hashes stable across the donated VM and native
 * executables (rather than using libc's deliberately unspecified hashes). */
static uint64_t hash_hixorlo(uint64_t a,uint64_t b){
#if defined(__SIZEOF_INT128__)
    __uint128_t p=(__uint128_t)a*b;return (uint64_t)p^(uint64_t)(p>>64);
#else
    uint64_t ah=a>>32,al=(uint32_t)a,bh=b>>32,bl=(uint32_t)b;
    uint64_t rhh=ah*bh,rhl=ah*bl,rlh=al*bh,rll=al*bl;
    uint64_t t=rll+(rhl<<32),carry=t<rll,lo=t+(rlh<<32);carry+=lo<t;
    return (rhh+(rhl>>32)+(rlh>>32)+carry)^lo;
#endif
}
static uint64_t hash_wang(uint64_t x){
    const uint64_t p0=UINT64_C(0xa0761d6478bd642f),p1=UINT64_C(0xe7037ed1a0b428db),p58=UINT64_C(0xeb44accab455d16d);
    return hash_hixorlo(hash_hixorlo(p0,x^p1),p58);
}
static uint64_t hash_mix(uint64_t h,uint64_t v){uint64_t r=h+v;r+=r<<10;r^=r>>6;return r;}
static uint64_t hash_finish(uint64_t h){uint64_t r=h+(h<<3);r^=r>>11;r+=r<<15;return r;}
static uint64_t hash_load32(const unsigned char*s){uint32_t v;memcpy(&v,s,4);return v;}
static uint64_t hash_load64(const unsigned char*s){uint64_t v;memcpy(&v,s,8);return v;}
static uint64_t hash_shiftmix(uint64_t v){return v^(v>>47);}
static uint64_t hash_rotr(uint64_t v,unsigned n){return (v>>n)|(v<<(64-n));}
static uint64_t hash_len16(uint64_t u,uint64_t v,uint64_t mul){uint64_t a=(u^v)*mul;a^=a>>47;uint64_t b=(v^a)*mul;b^=b>>47;return b*mul;}
static uint64_t hash_farm(const unsigned char*s,size_t n){
    const uint64_t k0=UINT64_C(0xc3a5c85c97cb3127),k1=UINT64_C(0xb492b66fbe98f273),k2=UINT64_C(0x9ae16a3b2f90404f);
    if(n<=16){
        if(n>=8){uint64_t m=k2+2*n,a=hash_load64(s)+k2,b=hash_load64(s+n-8);return hash_len16(hash_rotr(b,37)*m+a,(hash_rotr(a,25)+b)*m,m);}
        if(n>=4){uint64_t m=k2+2*n,a=hash_load32(s);return hash_len16(n+(a<<3),hash_load32(s+n-4),m);}
        if(n){uint32_t a=s[0],b=s[n>>1],c=s[n-1];uint64_t y=a+(b<<8),z=n+(c<<2);return hash_shiftmix((y*k2)^(z*k0))*k2;}
        return k2;
    }
    if(n<=32){uint64_t m=k2+2*n,a=hash_load64(s)*k1,b=hash_load64(s+8),c=hash_load64(s+n-8)*m,d=hash_load64(s+n-16)*k2;return hash_len16(hash_rotr(a+b,43)+hash_rotr(c,30)+d,a+hash_rotr(b+k2,18)+c,m);}
    if(n<=64){uint64_t m=k2+2*n,a=hash_load64(s)*k2,b=hash_load64(s+8),c=hash_load64(s+n-8)*m,d=hash_load64(s+n-16)*k2,y=hash_rotr(a+b,43)+hash_rotr(c,30)+d,z=hash_len16(y,a+hash_rotr(b+k2,18)+c,m),e=hash_load64(s+16)*m,f=hash_load64(s+24),g=(y+hash_load64(s+n-32))*m,h=(z+hash_load64(s+n-24))*m;return hash_len16(hash_rotr(e+f,43)+hash_rotr(g,30)+h,e+hash_rotr(f+a,18)+g,m);}
    /* FarmHash's long-string loop, expressed without platform intrinsics. */
    uint64_t x=81,y=81*k1+113,z=hash_shiftmix((81*k1+113)*k2+113)*k2;
    uint64_t v0=0,v1=0,w0=0,w1=0;size_t off=0,eos=((n-1)/64)*64,last=eos+((n-1)&63)-63;
#define WEAK32(P,A,B,O0,O1) do{uint64_t _a=(A)+hash_load64(P),_b=hash_rotr((B)+_a+hash_load64((P)+24),21),_c=_a;_a+=hash_load64((P)+8)+hash_load64((P)+16);_b+=hash_rotr(_a,44);(O0)=_a+hash_load64((P)+24);(O1)=_b+_c;}while(0)
    x=x*k2+hash_load64(s);
    for(;;){x=hash_rotr(x+y+v0+hash_load64(s+off+8),37)*k1;y=hash_rotr(y+v1+hash_load64(s+off+48),42)*k1;x^=w1;y+=v0+hash_load64(s+off+40);z=hash_rotr(z+w0,33)*k1;WEAK32(s+off,v1*k1,x+w0,v0,v1);WEAK32(s+off+32,z+w1,y+hash_load64(s+off+16),w0,w1);uint64_t t=z;z=x;x=t;off+=64;if(off==eos)break;}
    uint64_t m=k1+((z&255)<<1);off=last;w0+=(n-1)&63;v0+=w0;w0+=v0;x=hash_rotr(x+y+v0+hash_load64(s+off+8),37)*m;y=hash_rotr(y+v1+hash_load64(s+off+48),42)*m;x^=w1*9;y+=v0*9+hash_load64(s+off+40);z=hash_rotr(z+w0,33)*m;WEAK32(s+off,v1*m,x+w0,v0,v1);WEAK32(s+off+32,z+w1,y+hash_load64(s+off+16),w0,w1);uint64_t t=z;z=x;x=t;
#undef WEAK32
    return hash_len16(hash_len16(v0,w0,m)+hash_shiftmix(y)*k0+z,hash_len16(v1,w1,m)+x,m);
}
static int art_kind(Value v){switch(v.k){
    case V_NULL:return 0;case V_BOOL:return 1;case V_INT:return 2;case V_BIGINT:return 45;case V_FLOAT:return 3;case V_COMPLEX:return 4;case V_RATIONAL:return 5;case V_VERSION:return 6;case V_TYPE:return 7;case V_CHAR:return 8;case V_STR:return 9;case V_WORD:return 10;case V_LITERAL:return 11;case V_LABEL:return 12;case V_ATTRIBUTE:return 13;case V_ATTRIBUTELABEL:return 14;case V_PATH:return 15;case V_PATHLABEL:return 16;case V_PATHLITERAL:return 17;case V_SYMBOL:return 18;case V_SYMBOLLITERAL:return 19;case V_UNIT:return 20;case V_QUANTITY:return 21;case V_ERROR:return 22;case V_ERRORKIND:return 23;case V_REGEX:return 24;case V_COLOR:return 25;case V_DATE:return 26;case V_BINARY:return 27;case V_DICT:return 28;case V_FUNC:case V_BUILTIN:return 31;case V_INLINE:return 33;case V_BLOCK:return 34;case V_RANGE:return 36;default:return 44;}}
static uint64_t art_hash(Value v){
    uint64_t h=hash_wang((uint64_t)art_kind(v));
    switch(v.k){
        case V_NULL:break;case V_BOOL:h=hash_mix(h,v.u.b?UINT64_C(4):UINT64_C(8));break;case V_INT:h=hash_mix(h,(uint64_t)v.u.i);break;
        case V_BIGINT:h=hash_mix(h,hash_farm((const unsigned char*)v.u.s,strlen(v.u.s)));break;
        case V_FLOAT:{uint64_t bits;memcpy(&bits,&v.u.f,8);h=hash_mix(h,bits);break;}
        case V_CHAR:h=hash_mix(h,(uint64_t)utf8_codepoint(v.u.c));break;
        case V_STR:case V_WORD:case V_LITERAL:case V_LABEL:case V_ATTRIBUTE:case V_ATTRIBUTELABEL:case V_TYPE:case V_VERSION:case V_ERRORKIND:case V_REGEX:h=hash_mix(h,hash_farm((const unsigned char*)v.u.s,strlen(v.u.s)));break;
        case V_ERROR:h=hash_mix(h,hash_farm((const unsigned char*)v.u.error.message,strlen(v.u.error.message)));break;
        case V_RATIONAL:h=hash_mix(h,(uint64_t)v.u.rational.num);h=hash_mix(h,(uint64_t)v.u.rational.den);break;
        case V_COMPLEX:{uint64_t a,b;memcpy(&a,&v.u.complex.real,8);memcpy(&b,&v.u.complex.imag,8);h=hash_mix(h,a);h=hash_mix(h,b);break;}
        case V_QUANTITY:{uint64_t bits;memcpy(&bits,&v.u.quantity.amount,8);h=hash_mix(h,bits);h=hash_mix(h,hash_farm((const unsigned char*)v.u.quantity.unit,strlen(v.u.quantity.unit)));break;}
        case V_UNIT:h=hash_mix(h,hash_farm((const unsigned char*)v.u.s,strlen(v.u.s)));break;
        case V_COLOR:h=hash_mix(h,v.u.rgba);break;case V_DATE:case V_BINARY:break;
        case V_BLOCK:case V_INLINE:h=1;for(int i=0;i<v.u.block.b->n;i++)h=hash_mix(h,art_hash(*v.u.block.b->items[i]));break;
        case V_DICT:for(int i=0;i<v.u.dict->n;i++){h=hash_mix(h,hash_farm((const unsigned char*)v.u.dict->keys[i],strlen(v.u.dict->keys[i])));h=hash_mix(h,art_hash(v.u.dict->vals[i]));}break;
        case V_RANGE:h=hash_mix(h,(uint64_t)v.u.range.lo);h=hash_mix(h,(uint64_t)v.u.range.hi);h=hash_mix(h,(uint64_t)v.u.range.step);h=hash_mix(h,(uint64_t)v.u.range.character);h=hash_mix(h,(uint64_t)v.u.range.infinite);break;
        case V_PATH:for(int i=0;i<v.u.path.nsegs;i++){Value seg=v.u.path.segv?v.u.path.segv[i]:v_str(v.u.path.segs[i]);h=hash_mix(h,art_hash(seg));}break;
        default:break;
    }
    return hash_finish(h);
}
static Value b_hash(Env*e,Value*a,int n){(void)e;(void)n;int64_t h=(int64_t)art_hash(a[0]);if(rt_has_attr("string")){char out[32];snprintf(out,sizeof out,"%lld",(long long)h);return v_str(out);}return v_int((long)h);}
static Value with_substitute(Env *e,Value v){
    if(v.k==V_BLOCK){Value **items=xmalloc((size_t)(v.u.block.b->n+1)*sizeof(Value*));for(int i=0;i<v.u.block.b->n;i++){items[i]=xmalloc(sizeof(Value));*items[i]=with_substitute(e,*v.u.block.b->items[i]);}return v_block(items,v.u.block.b->n);}
    if((v.k==V_WORD||v.k==V_LITERAL||v.k==V_STR)&&v.u.s&&env_bound(e,v.u.s))return env_get(e,v.u.s);return clone_value(v);
}
static Value b_with(Env*e,Value*a,int n){
    (void)n;if(a[1].k!=V_BLOCK)die("with: expected body block");Env *capture=env_new(e);
    if(a[0].k==V_DICT)for(int i=0;i<a[0].u.dict->n;i++)env_set(capture,a[0].u.dict->keys[i],a[0].u.dict->vals[i]);
    else if(a[0].k==V_BLOCK)for(int i=0;i<a[0].u.block.b->n;i++){char *name=val_str(*a[0].u.block.b->items[i]);if(env_bound(e,name))env_set(capture,name,env_get(e,name));free(name);}
    else {char *name=val_str(a[0]);if(env_bound(e,name))env_set(capture,name,env_get(e,name));free(name);}Value result=with_substitute(capture,a[1]);env_release(capture);return result;
}
static Value b_alias(Env*e,Value*a,int n){(void)e;(void)a;(void)n;return v_null();}
static char *date_text(Value v){
    time_t stamp=(time_t)v.u.epoch;struct tm tmv;localtime_r(&stamp,&tmv);
    char base[32],zone[16];strftime(base,sizeof base,"%Y-%m-%dT%H:%M:%S",&tmv);strftime(zone,sizeof zone,"%z",&tmv);
    char *out=(char*)xmalloc(40);if(strlen(zone)==5)snprintf(out,40,"%s%c%c%c:%c%c",base,zone[0],zone[1],zone[2],zone[3],zone[4]);else snprintf(out,40,"%s%s",base,zone);return out;
}
static void dict_invalidate_object(Dict *d){if(d){d->object_name=NULL;d->object_checked=0;}}
static const char *object_type_name(Value v){
    if(v.k!=V_DICT||!v.u.dict)return NULL;Dict *d=v.u.dict;if(d->object_checked)return d->object_name;int marked=0;const char *name=NULL;
    for(int i=0;i<v.u.dict->n;i++){
        if(!strcmp(v.u.dict->keys[i],"__object")&&v.u.dict->vals[i].k==V_BOOL&&v.u.dict->vals[i].u.b)marked=1;
        if(!strcmp(v.u.dict->keys[i],"__type")&&v.u.dict->vals[i].k==V_STR)name=v.u.dict->vals[i].u.s;
    }
    d->object_name=marked?name:NULL;d->object_checked=1;return d->object_name;
}
Value v_bool(int b) { Value v;v.k=V_BOOL;v.u.b=b;return v; }
Value v_char(char c){ Value v;v.k=V_CHAR;v.u.c[0]=c;v.u.c[1]=0;return v; }
Value v_char_text(const char *s){ Value v;v.k=V_CHAR;size_t n=strlen(s);if(n>4)n=4;memcpy(v.u.c,s,n);v.u.c[n]=0;return v; }
Value v_str(const char *s) {
    Value v;v.k=V_STR;
    v.u.s = (char*)xmalloc(strlen(s)+1); strcpy(v.u.s, s);
    return v;
}
static Value v_str_owned(char *s){Value v;v.k=V_STR;v.u.s=s;return v;}
static Value v_error_kind(const char *message,const char *kind){Value v;v.k=V_ERROR;v.u.error.message=strdup(message?message:"");v.u.error.kind=strdup(kind?kind:"Runtime Error");return v;}
Value v_error(const char *s) { return v_error_kind(s,g_last_error_kind); }
Value v_version(const char *s) { return v_token(V_VERSION,s?s:""); }
Value v_errorkind(const char *s) { return v_token(V_ERRORKIND,s?s:""); }
Value v_block(Value **items, int n) {
    Value v;v.k=V_BLOCK;
    Block *b=(Block*)xmalloc(sizeof *b);
    b->items=items; b->n=n; b->cap=n;   /* takes ownership of the caller's array */
    v.u.block.b=b; return v;
}
Value v_dict(char **keys, Value *vals, int n) {
    Value v;v.k=V_DICT;
    Dict *d=(Dict*)xmalloc(sizeof *d);
    d->keys=keys; d->vals=vals; d->n=n; d->cap=n; d->index=NULL; d->index_cap=0; d->index_n=0; d->object_name=NULL; d->object_checked=0;
    v.u.dict=d; return v;
}
Value v_func(IR *params, IR **body, int nbody, Env *closure) {
    Value v;v.k=V_FUNC;
    v.u.fn.params=params; v.u.fn.body=body; v.u.fn.nbody=nbody; v.u.fn.closure=closure;v.u.fn.constructor=0;v.u.fn.is_action=0;v.u.fn.exports=NULL;v.u.fn.nexports=0;env_capture(closure);
    return v;
}
Value v_range(long lo, long hi) {
    Value v;v.k=V_RANGE;v.u.range.lo=lo;v.u.range.hi=hi;v.u.range.step=hi>=lo?1:-1;v.u.range.character=0;v.u.range.infinite=0;return v;
}
Value v_path(char **segs, int n) {
    /* Generated code passes a compound literal; own the segment array so the
     * value survives the builder function that created it. */
    char **copy=(char**)xmalloc((size_t)(n>0?n:1)*sizeof(char*));
    if(n>0&&segs)memcpy(copy,segs,(size_t)n*sizeof(char*));
    Value v;v.k=V_PATH;v.u.path.segs=copy;v.u.path.nsegs=n;v.u.path.segv=NULL;return v;
}
Value v_pathv(Value *segv, int n) {
    /* build the string segs (for runtime path ops) from the segment Values
     * (for `to :block` reconstruction). Each seg is a word/literal/int/str. */
    Value v;v.k=V_PATH;v.u.path.segv=segv;v.u.path.nsegs=n;
    char **ss=(char**)xmalloc(n*sizeof(char*));
    for(int i=0;i<n;i++) ss[i]=seg_text(segv[i]);
    v.u.path.segs=ss;
    return v;
}
Value v_token(VKind k, const char *s) {
    Value v;v.k=k;
    v.u.s=(char*)xmalloc(strlen(s)+1); strcpy(v.u.s,s);
    return v;
}
static Value clone_value(Value v){
    switch(v.k){
        case V_STR: case V_WORD: case V_LABEL: case V_LITERAL: case V_SYMBOL: case V_SYMBOLLITERAL:
        case V_TYPE: case V_INLINE: case V_PATHLABEL: case V_PATHLITERAL:
        case V_REGEX: case V_ATTRIBUTE: case V_ATTRIBUTELABEL: case V_VERSION: case V_ERRORKIND:
            return v_token(v.k,v.u.s?v.u.s:"");
        case V_ERROR:return v_error_kind(v.u.error.message,v.u.error.kind);
        case V_BLOCK: {
            int n=v.u.block.b->n;
            Value **items=(Value**)xmalloc((size_t)(n+1)*sizeof(Value*));
            for(int i=0;i<n;i++){
                items[i]=(Value*)xmalloc(sizeof(Value));
                *items[i]=clone_value(*v.u.block.b->items[i]);
            }
            return v_block(items,n);
        }
        case V_DICT: {
            int n=v.u.dict->n;
            char **keys=(char**)xmalloc((size_t)(n+1)*sizeof(char*));
            Value *vals=(Value*)xmalloc((size_t)(n+1)*sizeof(Value));
            for(int i=0;i<n;i++){
                keys[i]=strdup(v.u.dict->keys[i]);
                vals[i]=clone_value(v.u.dict->vals[i]);
            }
            return v_dict(keys,vals,n);
        }
        default: return v;
    }
}
/* the string form of a path segment value (for runtime path ops). */
/* decode the first UTF-8 code point at s (validated on the fly); returns the
 * code point, or the raw byte when not valid multi-byte UTF-8. */
static int utf8_codepoint(const char *s){
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && (unsigned char)s[1] >= 0x80 && (unsigned char)s[1] < 0xC0)
        return ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && (unsigned char)s[1] >= 0x80 && (unsigned char)s[1] < 0xC0 &&
        (unsigned char)s[2] >= 0x80 && (unsigned char)s[2] < 0xC0)
        return ((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F);
    if ((c & 0xF8) == 0xF0 && (unsigned char)s[1] >= 0x80 && (unsigned char)s[1] < 0xC0 &&
        (unsigned char)s[2] >= 0x80 && (unsigned char)s[2] < 0xC0 &&
        (unsigned char)s[3] >= 0x80 && (unsigned char)s[3] < 0xC0)
        return ((c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) |
               (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
    return c;
}
/* number of bytes of the first UTF-8 rune at s */
static int utf8_rune_len(const char *s){
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
/* number of runes in a UTF-8 string */
/* Loops over strings address runes 0,1,2,... in order.  Rescanning from the
 * start of the buffer on every lookup made character loops quadratic, which
 * dominated the compiler's own front end on large sources.  Remember where the
 * previous lookup on the same buffer ended and continue from there. */
static int rune_count(const char *s){
    if(g_rune_cache.s==s&&g_rune_cache.count>=0)return g_rune_cache.count;
    int n = 0;const char *p=s;
    while (*p){ n++; p += utf8_rune_len(p); }
    if(g_rune_cache.s!=s){g_rune_cache.s=s;g_rune_cache.index=0;g_rune_cache.at=s;}
    g_rune_cache.count=n;
    return n;
}
/* pointer to the start of rune index i in s */
static const char *rune_offset(const char *s, int i){
    const char *p = s;int from=0;
    if(g_rune_cache.s==s&&g_rune_cache.index<=i){p=g_rune_cache.at;from=g_rune_cache.index;}
    else if(g_rune_cache.s!=s){g_rune_cache.s=s;g_rune_cache.count=-1;}
    while (from<i && *p){ p += utf8_rune_len(p); from++; }
    g_rune_cache.index=from;g_rune_cache.at=p;
    return p;
}
/* the rune at byte position p of s, as a NUL-terminated UTF-8 buffer */
static void rune_at(const char *s, char out[5]){
    int len = utf8_rune_len(s);
    memcpy(out, s, (size_t)len);
    out[len] = 0;
}
/* pointer to the start of the last rune in s */
static const char *last_rune(const char *s){
    const char *p = s, *q = s;
    while (*p){ q = p; p += utf8_rune_len(p); }
    return q;
}

static char *seg_text(Value v){
    if(v.k==V_INT){ char b[64]; snprintf(b,sizeof b,"%ld",v.u.i); return strdup(b); }
    if(v.k==V_FLOAT){ char b[64]; snprintf(b,sizeof b,"%g",v.u.f); return strdup(b); }
    if(v.k==V_STR||v.k==V_WORD||v.k==V_LABEL||v.k==V_LITERAL||v.k==V_SYMBOL||v.k==V_SYMBOLLITERAL||v.k==V_TYPE||v.k==V_VERSION||v.k==V_ERRORKIND
       ||v.k==V_ATTRIBUTE||v.k==V_ATTRIBUTELABEL) return strdup(v.u.s?v.u.s:"");
    return strdup("");
}

int v_truthy(Value v) {
    if (v.k==V_NULL) return 0;
    if (v.k==V_BOOL) return v.u.b;
    if (v.k==V_BIGINT) return strcmp(v.u.s,"0")!=0;
    return 1;
}

/* host range rendering: `` `a`..`z` `` for char ranges, `1..3` numeric, a
 * trailing ` (step)` only when |step| != 1 (matches vrange.nim). */
static void range_text(Value v, char *buf, size_t cap){
    if (v.u.range.character){
        snprintf(buf, cap, "`%c`..", (char)v.u.range.lo);
        size_t l = strlen(buf);
        if (v.u.range.infinite) snprintf(buf + l, cap - l, "∞");
        else snprintf(buf + l, cap - l, "`%c`", (char)v.u.range.hi);
    } else {
        snprintf(buf, cap, "%ld..", v.u.range.lo);
        size_t l = strlen(buf);
        if (v.u.range.infinite) snprintf(buf + l, cap - l, "∞");
        else snprintf(buf + l, cap - l, "%ld", v.u.range.hi);
    }
    if (v.u.range.step != 1 && v.u.range.step != -1){
        long s = v.u.range.step < 0 ? -v.u.range.step : v.u.range.step;
        size_t l = strlen(buf);
        snprintf(buf + l, cap - l, " (%ld)", s);
    }
}

/* Arturo print: print the value, then a newline. Strings print raw. A block
 * prints its elements space-separated WITHOUT outer brackets; a block nested
 * inside a printed block keeps its brackets (matching the host). */
static void print_scalar(Value v) {
    switch (v.k) {
        case V_NULL:  printf("null"); break;
        case V_INT:   printf("%ld", v.u.i); break;
        case V_FLOAT: {
            char buf[128];
            fputs(fstr(v.u.f, buf, sizeof buf), stdout);
            break;
        }
        case V_RATIONAL: printf("%ld/%ld",v.u.rational.num,v.u.rational.den); break;
        case V_COMPLEX: {
            char rb[64],ib[64];fputs(fstr(v.u.complex.real,rb,sizeof rb),stdout);
            if(v.u.complex.imag>=0)putchar('+');fputs(fstr(v.u.complex.imag,ib,sizeof ib),stdout);putchar('i');break;
        }
        case V_QUANTITY: {char b[128];const char *symbol=custom_unit_symbol(v.u.quantity.unit),*dimension=custom_unit_dimension(v.u.quantity.unit);if(symbol&&dimension&&!strcmp(dimension,"currency"))snprintf(b,sizeof b,"%.2f",v.u.quantity.amount);else if(v.u.quantity.integral||floor(v.u.quantity.amount)==v.u.quantity.amount)snprintf(b,sizeof b,"%ld",(long)v.u.quantity.amount);else fstr(v.u.quantity.amount,b,sizeof b);fputs(b,stdout);fputc(' ',stdout);fputs(symbol?symbol:v.u.quantity.unit,stdout);break;}
        case V_UNIT: fputs(v.u.s,stdout);break;
        case V_DATE: {char *s=date_text(v);fputs(s,stdout);free(s);break;}
        case V_COLOR: {char *s=color_text(v);fputs(s,stdout);free(s);break;}
        case V_BINARY: for(size_t i=0;i<v.u.binary.len;i++){if(i)putchar(' ');printf("%02X",v.u.binary.data[i]);}break;
        case V_STR: case V_WORD: case V_LABEL: case V_LITERAL: case V_SYMBOL:
        case V_SYMBOLLITERAL: case V_TYPE: case V_REGEX: case V_ATTRIBUTE:
        case V_ATTRIBUTELABEL:
            fwrite(v.u.s,1,strlen(v.u.s),stdout); break;
        case V_ERROR: printf("%s: %s",v.u.error.kind?v.u.error.kind:"Runtime Error",v.u.error.message?v.u.error.message:""); break;
        case V_VERSION: case V_ERRORKIND: fputs(v.u.s?v.u.s:"",stdout); break;
        case V_CHAR:  fputs(v.u.c, stdout); break;
        case V_BIGINT: fputs(v.u.s?v.u.s:"0", stdout); break;
        case V_BOOL:  printf(v.u.b ? "true" : "false"); break;
        case V_DICT: {
            const char *objectType=object_type_name(v);
            printf("[");
            int shown=0;
            for (int i=0;i<v.u.dict->n;i++){
                if(objectType&&(!strncmp(v.u.dict->keys[i],"__",2)||!strcmp(v.u.dict->keys[i],"init")||v.u.dict->vals[i].k==V_FUNC))continue;
                if(shown++)printf(" ");printf("%s:",v.u.dict->keys[i]);print_scalar(v.u.dict->vals[i]);
            }
            printf("]");
            break;
        }
        case V_BLOCK:
            printf("[");
            for(int i=0;i<v.u.block.b->n;i++){if(i)printf(" ");print_scalar(*v.u.block.b->items[i]);}
            printf("]");
            break;
        case V_FUNC: printf("function"); break;
        case V_BUILTIN: printf("<function:builtin>"); break;
        case V_RANGE: { char rb[64]; range_text(v, rb, sizeof rb); printf("%s", rb); break; }
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
        for (int i=0;i<v.u.block.b->n;i++){ if(i)printf(" "); print_embedded(*v.u.block.b->items[i]); }
        printf("]");
    } else {
        print_scalar(v);
    }
}
void v_print(Value v) {
    if (v.k == V_BLOCK) {
        /* host prints each element followed by a space (trailing space kept) */
        for (int i=0;i<v.u.block.b->n;i++){ print_embedded(*v.u.block.b->items[i]); printf(" "); }
    } else {
        print_scalar(v);
    }
    printf("\n");
}

/* ---- environments -------------------------------------------------------- */
static Env *g_env_live=NULL;static unsigned long g_env_serial=0;
static int env_heapcheck(void){static int initialized=0,enabled=0;if(!initialized){const char*s=getenv("ARTURO_NATIVE_HEAPCHECK");enabled=s&&*s&&strcmp(s,"0");initialized=1;}return enabled;}
Env *env_new(Env *parent) {
    Env *e = (Env*)xmalloc(sizeof *e);
    e->names=e->inline_names;e->vals=e->inline_vals;e->n=0;e->cap=ENV_INLINE_SLOTS;e->parent=parent;e->rebind_parent=0;e->captured=0;e->created=++g_env_serial;
    e->live_prev=NULL;e->live_next=g_env_live;if(g_env_live)g_env_live->live_prev=e;g_env_live=e;return e;
}
static void env_capture(Env *e){for(;e;e=e->parent)e->captured=1;}
static void env_release(Env *e){
    if(!e||e->captured||env_heapcheck())return;
    if(e->live_prev)e->live_prev->live_next=e->live_next;else g_env_live=e->live_next;if(e->live_next)e->live_next->live_prev=e->live_prev;
    if(e->names!=e->inline_names)free(e->names);if(e->vals!=e->inline_vals)free(e->vals);free(e);
}
static void env_rewind(unsigned long serial){for(Env *e=g_env_live,*next;e;e=next){next=e->live_next;if(e->created>serial&&!e->captured)env_release(e);}}
static void env_grow(Env *e){
    if(e->n<e->cap)return;int cap=e->cap*2;char **names=(char**)xmalloc((size_t)cap*sizeof(char*));Value *vals=(Value*)xmalloc((size_t)cap*sizeof(Value));
    memcpy(names,e->names,(size_t)e->n*sizeof(char*));memcpy(vals,e->vals,(size_t)e->n*sizeof(Value));
    if(e->names!=e->inline_names)free(e->names);if(e->vals!=e->inline_vals)free(e->vals);e->names=names;e->vals=vals;e->cap=cap;
}
static int env_find_interned(Env *e,const char *name){
    for (int i=0;i<e->n;i++) if (e->names[i]==name) return i;
    return -1;
}
static int env_find(Env *e,const char *name){return env_find_interned(e,intern(name));}
int env_bound(Env *e, const char *name) {
    const char *key=intern(name);for (Env *f=e;f;f=f->parent) if (env_find_interned(f,key)>=0) return 1;
    return 0;
}
Value env_get(Env *e, const char *name) {
    const char *key=intern(name);for (Env *f=e;f;f=f->parent) { int i=env_find_interned(f,key); if (i>=0) return f->vals[i]; }
    static const char *pi_name,*e_name,*infinite_name;if(!pi_name){pi_name=intern("pi");e_name=intern("e");infinite_name=intern("infinite");}
    if(key==pi_name)return v_float(M_PI);if(key==e_name)return v_float(M_E);if(key==infinite_name)return v_float(INFINITY);
    return v_null();
}
/* `x: value` — updates the nearest existing binding (walking parent scopes),
 * matching the host's loop-body assignment (`loop xs [x][ cnt: cnt + 1 ]`
 * accumulates cnt). The compiler's @LBL action-body defines use this. */
void env_set(Env *e, const char *name, Value v) {
    const char *key=intern(name);
    for (Env *f=e; f; f=f->parent) {
        int i = env_find_interned(f,key);
        if (i>=0) { f->vals[i]=v; return; }
    }
    env_grow(e);e->names[e->n]=(char*)key;
    e->vals[e->n] = v; e->n++;
}

/* `x: value` at FUNCTION scope and parameter binding — LOCAL only. The host
 * shadows: `f: function [][ y: 5 ]` does NOT touch an outer y, `x: x + 1` on a
 * param updates the param, and a closure's `z: z + 1` does NOT reach the
 * capturing scope. env_set's parent walk would clobber an outer binding of the
 * same name (the compiler's `c`/`parts`/`ctx` locals collided with the global
 * `c` and the crash was `get <stack> "stack"`). */
static void env_define_local(Env *e, const char *name, Value v) {
    const char *key=intern(name);int i=env_find_interned(e,key);
    if (i>=0) { e->vals[i]=v; return; }
    env_grow(e);e->names[e->n]=(char*)key;
    e->vals[e->n] = v; e->n++;
}

/* A kernel-owned loop body uses action-block scope: parameters are local, but
 * a label assignment updates the nearest existing outer binding. */
static void env_define_body(Env *e, const char *name, Value v) {
    if (e->rebind_parent&&e->parent) env_set(e->parent, name, v);
    else env_define_local(e, name, v);
}

/* `let 'x value` updates the nearest binding and creates a missing name in
 * the current frame. This differs from a function-local label only when an
 * outer binding already exists: `let` reaches it, while `x:` shadows it. */
static void env_let(Env *e, const char *name, Value v) {
    const char *key=intern(name);
    for (Env *f=e; f; f=f->parent) {
        int i=env_find_interned(f,key);
        if(i>=0){ f->vals[i]=v; return; }
    }
    env_define_local(e,name,v);
}

/* ---- IR builders --------------------------------------------------------- */
static int literal_code(const char *name){
    if(!name)return 0;
    if(!strcmp(name,"true"))return 1;
    if(!strcmp(name,"false"))return 2;
    if(!strcmp(name,"null"))return 3;
    if(!strcmp(name,"runtimeError"))return 4;
    return 0;
}
static Value literal_value(int code){
    switch(code){case 1:return v_bool(1);case 2:return v_bool(0);case 3:return v_null();default:return v_errorkind("Runtime Error");}
}
static IR *ir_new(const char *op) {
    IR *n=(IR*)xmalloc(sizeof *n); memset(n,0,sizeof *n); n->op=intern(op); n->opcode=opcode_of(op); return n;
}
/* Constructors own their argument arrays. Generated code passes compound
 * literals, whose storage ends with the enclosing builder function, so the
 * tree must not keep pointing into that frame. */
static IR **ir_copy_nodes(IR **items,int n){
    IR **copy=(IR**)xmalloc((size_t)(n>0?n:1)*sizeof(IR*));
    if(n>0&&items)memcpy(copy,items,(size_t)n*sizeof(IR*));
    return copy;
}
static const char **ir_copy_names(const char **names,int n){
    const char **copy=(const char**)xmalloc((size_t)(n>0?n:1)*sizeof(char*));
    for(int i=0;i<n;i++)copy[i]=intern(names[i]);
    return copy;
}
IR *ir_const(Value v){ IR*n=ir_new("const"); n->v=v; return n; }
IR *ir_load(const char *name){ IR*n=ir_new("load"); n->name=intern(name); n->literal=literal_code(name); return n; }
IR *ir_intrinsic(const char *name){
    IR*n=ir_new("intrinsic"); n->name=intern(name);
    static const char *const arith[]={"add","sub","mul","div","fdiv","mod","pow",NULL};
    for(int i=0;arith[i];i++)if(!strcmp(arith[i],name))n->flags|=IRF_ARITH_REF;
    if(is_shadowable(name))n->flags|=IRF_SHADOWABLE;
    n->builtin=builtin_index_of(name);
    n->arity=declared_arity_of(name);
    return n;
}
/* `ir_word` — a bare word in VALUE position (the IR's `[op:intrinsic name:X]`
 * node when it is NOT a call callee). On the host a bare word resolves var-first
 * (a bound parameter like `arity`/`env` loads its value), else a zero-arity
 * builtin (`args`, `break`) is called. */
IR *ir_word(const char *name){ IR*n=ir_new("word"); n->name=intern(name); n->literal=literal_code(name); return n; }
IR *ir_define(const char *name, IR *expr){ IR*n=ir_new("define"); n->name=intern(name); n->args=(IR**)xmalloc(sizeof(IR*)); n->args[0]=expr; n->nargs=1; return n; }
IR *ir_let(const char *name, IR *expr){ IR*n=ir_new("let"); n->name=intern(name); n->args=(IR**)xmalloc(sizeof(IR*)); n->args[0]=expr; n->nargs=1; return n; }
IR *ir_call(IR *fn, IR **args, int n){ IR*x=ir_new("call"); x->fn=fn; x->args=ir_copy_nodes(args,n); x->nargs=n; return x; }
IR *ir_call_attrs(IR *fn,IR **args,int n,const char **names,IR **values,int nattrs){IR*x=ir_call(fn,args,n);return ir_attrs(x,names,values,nattrs);}
IR *ir_attrs(IR *node,const char **names,IR **values,int nattrs){node->attr_names=ir_copy_names(names,nattrs);node->attr_values=ir_copy_nodes(values,nattrs);node->nattrs=nattrs;return node;}
IR *ir_passthrough(Value src){ IR*n=ir_new("passthrough"); n->v=src; return n; }
IR *ir_block(IR **items, int n){ IR*x=ir_new("block"); x->args=ir_copy_nodes(items,n); x->nargs=n; return x; }
IR *ir_fn(IR *params, IR **body, int n){ IR*x=ir_new("function"); x->args=(IR**)xmalloc((n+1)*sizeof(IR*)); x->args[0]=params; for(int i=0;i<n;i++)x->args[i+1]=body[i]; x->nargs=n+1; return x; }
IR *ir_op(const char *op, IR **args, int n){ IR*x=ir_new(op); x->args=ir_copy_nodes(args,n); x->nargs=n; return x; }
IR *ir_op_attrs(const char *op,IR **args,int n,const char **names,IR **values,int nattrs){IR*x=ir_op(op,args,n);return ir_attrs(x,names,values,nattrs);}
IR *ir_higher(const char *name,IR *collection,IR *params,IR *body,const char **names,IR **values,int nattrs){
    IR *args[3]={collection,params,body};IR *x=ir_op("higher",args,3);x->name=intern(name);x->builtin=builtin_index_of(name);x->arity=declared_arity_of(name);return nattrs?ir_attrs(x,names,values,nattrs):x;
}
/* a seq: internal __seq node wrapping a list of statements */
IR *ir_seq(IR **items, int n){ IR*x=ir_new("__seq"); x->args=ir_copy_nodes(items,n); x->nargs=n; return x; }

/* ---- return / break signals ------------------------------------------------------- */
static int   rt_ret_set = 0;
static Value rt_ret_val;
static int   rt_brk_set = 0;   /* `break` inside a loop body */
static int   rt_cont_set = 0;  /* `continue` inside a loop body */

/* ---- apply a function value to evaluated args ----------------------------- */
/* arity of a function VALUE (V_FUNC params block, or builtin arity by name) */
static int value_arity(Value fn){
    if (fn.k == V_FUNC) {
        IR *p = fn.u.fn.params;
        if (p && p->op && (p->opcode==OP_BLOCK)) return p->nargs;
        return p ? 1 : 0;
    }
    /* every intrinsic has a declared arity (the same table the host's
     * CheckCallablePath uses); the hand-curated fn_arity list only covered a
     * fraction of names, so `ops\print 3` in statement position went unapplied. */
    if (fn.k == V_BUILTIN && fn.u.s) return declared_arity_of(fn.u.s);
    return -1;
}
static int is_pathget_ir(IR *node){
    return node && node->op && (node->opcode==OP_CALL) && node->fn
        && node->fn->op && (node->fn->opcode==OP_INTRINSIC)
        && !strcmp(node->fn->name,"pathGet");
}
/* A statement whose value is a function read through a path, followed by its
 * arguments (`ops\add a b`, `d\size 5`), is a call on the host: CheckCallablePath
 * resolves the path and the invoke consumes exactly `arity` following items.
 * `ir` is the statement that produced `fn` (or the pathGet argument of a
 * `return` statement); returns 1 and fills `*r` when it consumed the tail,
 * else 0 (caller keeps evaluating normally). The host raises "not enough
 * parameters" when fewer than `arity` items follow; applying with fewer
 * silently ran a closure with unbound params, so that too is an error here. */
static int path_call_tail(Env *e, IR **seq, int n, int *ip, IR *ir, Value fn, Value *r){
    if (!is_pathget_ir(ir)) return 0;
    int arity = value_arity(fn);
    if (arity < 0) return 0;
    int avail = n - (*ip) - 1;
    if (avail < 0) return 0;
    if (avail < arity) {
        const char *nm = (fn.k==V_BUILTIN && fn.u.s) ? fn.u.s : "function";
        char msg[256];
        snprintf(msg,sizeof msg,"%s: not enough arguments (expected %d, got %d)", nm, arity, avail);
        die(msg);
    }
    Value stack[8];Value *argv=arity<=8?stack:(Value*)xmalloc((size_t)(arity+1)*sizeof(Value));
    for (int k=0;k<arity;k++){
        argv[k] = runNode0(e, seq[*ip + 1 + k]);
        if (rt_ret_set || rt_brk_set || rt_cont_set) { if(argv!=stack)free(argv); return 0; }
    }
    *r = applyFunc(e, fn, argv, arity);
    if(argv!=stack)free(argv);
    *ip = *ip + arity;
    return 1;
}
/* the pathGet argument of a `return <pathGet>` statement the front could not
 * group into a call (a runtime-valued dict function): the returned value is a
 * function read through a path, and the host applies the following items to it. */
static IR *ret_pathget(IR *node){
    if (!node || !node->op || (node->opcode!=OP_RETURN)) return NULL;
    if (node->nargs < 1) return NULL;
    return is_pathget_ir(node->args[0]) ? node->args[0] : NULL;
}
static Value applyFunc(Env *caller, Value fn, Value *argv, int n) {
    if (fn.k==V_BUILTIN || fn.k==V_LITERAL || fn.k==V_WORD || fn.k==V_STR) {
        /* Arturo's `call 'name args` resolves a literal function reference.
         * The native `to :literal` compatibility path currently carries that
         * name as a string-like value, so accept all equivalent name kinds. */
        Value out;
        if (rt_builtin(fn.u.s, caller, argv, n, &out)) return out;
        die("unknown builtin"); return v_null();
    }
    if (fn.k!=V_FUNC) { die("cannot call non-function"); return v_null(); }
    Env *child = env_new(fn.u.fn.closure);
    IR *params = fn.u.fn.params;   /* a block of name words (IR) or NULL */
    if (params && params->op && (params->opcode==OP_BLOCK)) {
        for (int i=0;i<params->nargs;i++){
            IR *p = params->args[i];
            const char *nm = NULL;
            if (p->op && (p->opcode==OP_LOAD)) nm=p->name;
            else if(p->op&&(p->opcode==OP_CONST)&&(p->v.k==V_STR||p->v.k==V_WORD||p->v.k==V_LITERAL))nm=p->v.u.s;
            if (nm && i<n) env_define_local(child, nm, argv[i]);
        }
    }
    rt_ret_set=0;
    Value r = v_null(), second = v_null();
    int tailed = 0;   /* a path call consumed its tail; skip the 2-expr fold */
    for (int i=0;i<fn.u.fn.nbody;i++){
        second = r;
        r = runNode0(child, fn.u.fn.body[i]);
        if (rt_brk_set || rt_cont_set) break;
        if (rt_ret_set) {
            /* `return ops\f a b` when the front could not group the path (a
             * runtime-valued dict function): the returned value is a function
             * read through a path, so its arguments follow as statements. */
            IR *pg = ret_pathget(fn.u.fn.body[i]);
            if (pg && (r.k==V_FUNC || r.k==V_BUILTIN)) {
                rt_ret_set = 0;  /* the return already delivered; a tail may follow */
                if (path_call_tail(child, fn.u.fn.body, fn.u.fn.nbody, &i, pg, r, &r)) rt_ret_val = r;
                rt_ret_set = 1;
            }
            break;
        }
        if (r.k==V_FUNC && fn.u.fn.nbody>1
            && path_call_tail(child, fn.u.fn.body, fn.u.fn.nbody, &i, fn.u.fn.body[i], r, &r)) {
            tailed = 1;
            continue;
        }
    }
    for(int i=0;i<fn.u.fn.nexports;i++)if(env_find(child,fn.u.fn.exports[i])>=0)env_set(child->parent,fn.u.fn.exports[i],env_get(child,fn.u.fn.exports[i]));
    Value result=r;if(rt_ret_set){rt_ret_set=0;result=rt_ret_val;}
    else if(!tailed&&fn.u.fn.nbody==2&&!node_is_stmt(fn.u.fn.body[0])&&!node_is_stmt(fn.u.fn.body[1]))result=apply_tail_two(child,second,r);
    env_release(child);return result;
}

/* ---- builtins ------------------------------------------------------------- */

/* numeric/string/char coercion helpers */
/* host `add`/`sub`/`mul`/`div`/`mod`/`pow` accept only numeric-like operands:
 * integer, floating, rational, complex, quantity, color. Char and logical
 * operands are rejected (`'a' + 1`, `5 + true` error), matching the host. */
static int is_numeric_kind(Value v){
    return v.k==V_INT||v.k==V_FLOAT||v.k==V_RATIONAL||v.k==V_COMPLEX||v.k==V_QUANTITY||v.k==V_COLOR||v.k==V_BIGINT;
}

/* ---- arbitrary-precision integers (V_BIGINT) -------------------------------
 * The host promotes int64 overflow to big integers (`2 ^ 63`,
 * `9223372036854775807 + 1`). The native runtime keeps normal values as int64
 * and uses decimal-string bigints only when the magnitude overflows. */
Value v_bigint_text(const char *text){Value v;v.k=V_BIGINT;v.u.s=strdup(text);return v;}
Value v_bigint_i128(__int128 i){
    int negative = i < 0;
    unsigned __int128 x = negative ? (unsigned __int128)(-(i+1)) + 1u : (unsigned __int128)i;
    char buf[64]; char *p = buf + sizeof buf - 1; *p = 0;
    do { *--p = (char)('0' + (int)(x % 10)); x /= 10; } while (x);
    if (negative) *--p = '-';
    return v_bigint_text(p);
}
static int big_neg(const char *s){ return s && s[0] == '-'; }
static const char *big_abs(const char *s){ return big_neg(s) ? s + 1 : s; }
/* strip leading zeros (keeps one digit) */
static const char *big_norm(const char *s){
    const char *p = s;
    while (p[0] == '0' && p[1]) p++;
    return p;
}
/* compare |a| vs |b| digit strings; returns <0,0,>0 */
static int big_cmp_abs(const char *a, const char *b){
    a = big_norm(a); b = big_norm(b);
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la < lb ? -1 : 1;
    return strcmp(a, b);
}
static int big_cmp(const char *a, const char *b){
    int an = big_neg(a), bn = big_neg(b);
    if (an != bn) return an ? -1 : 1;
    int c = big_cmp_abs(a, b);
    return an ? -c : c;
}
/* |a| + |b| -> new string (no sign) */
static char *big_add_abs(const char *a, const char *b){
    size_t la = strlen(a), lb = strlen(b);
    size_t cap = (la > lb ? la : lb) + 2;
    char *out = xmalloc(cap); size_t at = 0;
    int carry = 0;
    for (size_t i = 0; i < cap - 1; i++){
        int da = i < la ? a[la-1-i] - '0' : 0;
        int db = i < lb ? b[lb-1-i] - '0' : 0;
        int sum = da + db + carry;
        out[at++] = (char)('0' + sum % 10);
        carry = sum / 10;
        if (i + 1 >= la && i + 1 >= lb && !carry) break;
    }
    if (carry) out[at++] = (char)('0' + carry);
    out[at] = 0;
    for (size_t i = 0, j = at - 1; i < j; i++, j--){ char t = out[i]; out[i] = out[j]; out[j] = t; }
    return out;
}
/* |a| - |b| (requires |a| >= |b|) -> new string (no sign) */
static char *big_sub_abs(const char *a, const char *b){
    size_t la = strlen(a), lb = strlen(b);
    size_t cap = la + 1;
    char *out = xmalloc(cap); size_t at = 0;
    int borrow = 0;
    for (size_t i = 0; i < la; i++){
        int da = a[la-1-i] - '0';
        int db = i < lb ? b[lb-1-i] - '0' : 0;
        int d = da - db - borrow;
        if (d < 0){ d += 10; borrow = 1; } else borrow = 0;
        out[at++] = (char)('0' + d);
    }
    while (at > 1 && out[at-1] == '0') at--;
    out[at] = 0;
    for (size_t i = 0, j = at - 1; i < j; i++, j--){ char t = out[i]; out[i] = out[j]; out[j] = t; }
    return out;
}
static char *big_add(const char *a, const char *b){
    int an = big_neg(a), bn = big_neg(b);
    if (an == bn){
        char *r = big_add_abs(big_abs(a), big_abs(b));
        if (an){ char *s = xmalloc(strlen(r) + 2); s[0] = '-'; strcpy(s + 1, r); free(r); return s; }
        return r;
    }
    int c = big_cmp_abs(a, b);
    if (c == 0) return strdup("0");
    const char *bigger = c > 0 ? a : b, *smaller = c > 0 ? b : a;
    char *r = big_sub_abs(big_abs(bigger), big_abs(smaller));
    if (big_neg(bigger) && strcmp(r, "0")){ char *s = xmalloc(strlen(r) + 2); s[0] = '-'; strcpy(s + 1, r); free(r); return s; }
    return r;
}
static char *big_sub(const char *a, const char *b){
    int bn = big_neg(b);
    char *nb = xmalloc(strlen(b) + 2);
    if (bn){ strcpy(nb, b + 1); } else { nb[0] = '-'; strcpy(nb + 1, b); }
    char *r = big_add(a, nb);
    free(nb);
    return r;
}
static char *big_mul(const char *a, const char *b){
    int negative = big_neg(a) != big_neg(b);
    const char *A = big_abs(a), *B = big_abs(b);
    size_t la = strlen(A), lb = strlen(B);
    if (strcmp(A,"0")==0 || strcmp(B,"0")==0) return strdup("0");
    size_t cap = la + lb + 2;
    char *res = xmalloc(cap); memset(res, '0', cap - 1); res[cap-1] = 0;
    for (size_t i = 0; i < la; i++){
        int da = A[la-1-i] - '0';
        if (!da) continue;
        int carry = 0;
        for (size_t j = 0; j < lb; j++){
            int db = B[lb-1-j] - '0';
            size_t pos = cap - 2 - (i + j);
            int cur = (res[pos] - '0') + da * db + carry;
            res[pos] = (char)('0' + cur % 10);
            carry = cur / 10;
        }
        size_t pos = cap - 2 - (i + lb);
        while (carry){ int cur = (res[pos] - '0') + carry % 10; res[pos] = (char)('0' + cur % 10); carry = cur / 10; if (pos) pos--; else break; }
    }
    const char *norm = big_norm(res);
    char *out;
    if (negative){
        out = xmalloc(strlen(norm) + 2); out[0] = '-'; strcpy(out + 1, norm);
    } else {
        out = strdup(norm);
    }
    free(res);
    return out;
}
static char *big_negate(const char *s){
    if (!strcmp(s, "0")) return strdup("0");
    char *out = xmalloc(strlen(s) + 2);
    if (big_neg(s)) strcpy(out, s + 1);
    else { out[0] = '-'; strcpy(out + 1, s); }
    return out;
}
static char *big_pow(const char *base, long exp){
    char *result = strdup("1");
    char *b = strdup(base);
    while (exp){
        if (exp & 1){ char *t = big_mul(result, b); free(result); result = t; }
        exp >>= 1;
        if (exp){ char *t = big_mul(b, b); free(b); b = t; }
    }
    free(b);
    return result;
}
/* long division: q and r as new strings */
static void big_divmod(const char *a, const char *b, char **q, char **r){
    int negative = big_neg(a) != big_neg(b);
    const char *A = big_abs(a), *B = big_abs(b);
    char *rem = strdup("");
    size_t la = strlen(A);
    char *qb = xmalloc(la + 2); size_t qat = 0;
    for (size_t i = 0; i < la; i++){
        size_t rl = strlen(rem);
        char *nr = xmalloc(rl + 2);
        if (rl == 0 || (rl == 1 && rem[0] == '0')){ nr[0] = A[i]; nr[1] = 0; }
        else { memcpy(nr, rem, rl); nr[rl] = A[i]; nr[rl+1] = 0; }
        free(rem); rem = nr;
        long qd = 0;
        while (big_cmp_abs(rem, B) >= 0){
            char *t = big_sub_abs(rem, B); free(rem); rem = t; qd++;
        }
        qb[qat++] = (char)('0' + qd);
    }
    qb[qat] = 0;
    const char *qn = big_norm(qb);
    char *qout;
    if (negative && strcmp(qn, "0")){ qout = xmalloc(strlen(qn) + 2); qout[0] = '-'; strcpy(qout + 1, qn); }
    else qout = strdup(qn);
    free(qb);
    *q = qout;
    *r = rem;
}
static int big_fits_i64(const char *s){
    const char *p = big_abs(s);
    if (strlen(p) > 19) return 0;
    if (strlen(p) == 19){
        const char *lim = big_neg(s) ? "9223372036854775808" : "9223372036854775807";
        if (strcmp(p, lim) > 0) return 0;
    }
    return 1;
}
static int is_bigint(Value v){ return v.k == V_BIGINT; }

/* parse a decimal string into an int64 value, promoting to V_BIGINT on
 * overflow (matches the host's big-integer literals and `to :integer`) */
static Value v_int_from_text(const char *s){
    if (big_fits_i64(s)) return v_int(atoll(s));
    return v_bigint_text(s);
}

/* int64 arithmetic with promotion to V_BIGINT on overflow (host parity) */
static Value int_add(long a, long b){
    __int128 r = (__int128)a + (__int128)b;
    if (r > INT64_MAX || r < INT64_MIN) return v_bigint_i128(r);
    return v_int((long)r);
}
static Value int_sub(long a, long b){
    __int128 r = (__int128)a - (__int128)b;
    if (r > INT64_MAX || r < INT64_MIN) return v_bigint_i128(r);
    return v_int((long)r);
}
static Value int_mul(long a, long b){
    __int128 r = (__int128)a * (__int128)b;
    if (r > INT64_MAX || r < INT64_MIN) return v_bigint_i128(r);
    return v_int((long)r);
}
static Value int_pow(long base, long exp){
    if (exp >= 0 && exp <= 64){
        __int128 r = 1; int ok = 1;
        for (long i=0;i<exp;i++){ r *= base; if (r > INT64_MAX || r < INT64_MIN){ ok=0; break; } }
        if (ok) return v_int((long)r);
    } else if (exp > 64 && base >= -1 && base <= 1){
        if (base == 0) return v_int(0);
        if (base == 1) return v_int(1);
        return v_int((exp & 1) ? -1 : 1);
    }
    char bb[32]; snprintf(bb, sizeof bb, "%ld", base);
    char *r = big_pow(bb, exp);
    if (big_fits_i64(r)){ long v = atoll(r); free(r); return v_int(v); }
    Value out = v_bigint_text(r); free(r); return out;
}
/* arbitrary int arithmetic where either side may be a bigint: returns a new
 * value (never dies on magnitude). operands are known numeric. */
enum { IOP_ADD, IOP_SUB, IOP_MUL };
static Value int_op(int op, Value a, Value b){
    if (a.k != V_INT && a.k != V_BIGINT){ die("expected number"); return v_null(); }
    if (b.k != V_INT && b.k != V_BIGINT){ die("expected number"); return v_null(); }
    if (a.k == V_INT && b.k == V_INT){
        if (op==IOP_ADD) return int_add(a.u.i, b.u.i);
        if (op==IOP_SUB) return int_sub(a.u.i, b.u.i);
        return int_mul(a.u.i, b.u.i);
    }
    char *x = val_str(a), *y = val_str(b);
    char *r;
    if (op==IOP_ADD) r = big_add(x, y);
    else if (op==IOP_SUB) r = big_sub(x, y);
    else r = big_mul(x, y);
    free(x); free(y);
    Value out = v_bigint_text(r); free(r); return out;
}

static long as_int(Value v) {
    if (v.k==V_INT) return v.u.i;
    if (v.k==V_BIGINT){
        if (big_fits_i64(v.u.s)) return atoll(v.u.s);
        die("integer overflow"); return 0;
    }
    if (v.k==V_FLOAT) return (long)v.u.f;
    if (v.k==V_RATIONAL) return v.u.rational.num/v.u.rational.den;
    if (v.k==V_BOOL) return v.u.b?1:0;
    if (v.k==V_CHAR) return utf8_codepoint(v.u.c);
    die("expected number"); return 0;
}
static double as_float(Value v) {
    if (v.k==V_FLOAT) return v.u.f;
    if (v.k==V_INT) return (double)v.u.i;
    if (v.k==V_BIGINT) return strtod(v.u.s, NULL);
    if (v.k==V_RATIONAL) return (double)v.u.rational.num/(double)v.u.rational.den;
    if (v.k==V_BOOL) return v.u.b?1.0:0.0;
    die("expected number"); return 0;
}
static Value num2(long i){ return v_int(i); }
static Value numf(double f){ return v_float(f); }

static void rational_parts(Value v,long *num,long *den){
    if(v.k==V_RATIONAL){*num=v.u.rational.num;*den=v.u.rational.den;}
    else if(v.k==V_FLOAT) floating_fraction(v.u.f,num,den);
    else {*num=as_int(v);*den=1;}
}
static void complex_parts(Value v,double *real,double *imag){
    if(v.k==V_COMPLEX){*real=v.u.complex.real;*imag=v.u.complex.imag;}
    else {*real=as_float(v);*imag=0.0;}
}
static const UnitDef UNIT_DEFS[]={
    {"mm","length",0.001},{"cm","length",0.01},{"m","length",1.0},{"km","length",1000.0},{"in","length",0.0254},{"ft","length",0.3048},{"yd","length",0.9144},{"mi","length",1609.344},
    {"meter","length",1.0},{"meters","length",1.0},{"inch","length",0.0254},{"inches","length",0.0254},{"foot","length",0.3048},{"feet","length",0.3048},{"yard","length",0.9144},{"yards","length",0.9144},{"rod","length",5.0292},{"rods","length",5.0292},{"mile","length",1609.344},{"miles","length",1609.344},{"point","length",0.0003527777777777778},{"points","length",0.0003527777777777778},{"pica","length",0.004233333333333333},{"pixel","length",0.0002645833333333333},{"fathom","length",0.9144},{"furlong","length",201.168},{"angstrom","length",1e-10},{"ang","length",1e-10},{"lightYear","length",9460730472580800.0},{"ly","length",9460730472580800.0},{"nauticalMile","length",1852.0},{"nmi","length",1852.0},{"astronomicalUnit","length",149597870700.0},{"au","length",149597870700.0},
    {"metre","length",1.0},{"metres","length",1.0},{"ftm","length",0.9144},{"fathoms","length",0.9144},{"fur","length",201.168},{"furlongs","length",201.168},{"angstroms","length",1e-10},{"lightYears","length",9460730472580800.0},{"nauticalMiles","length",1852.0},{"astronomicalUnits","length",149597870700.0},{"picas","length",0.004233333333333333},{"pixels","length",0.0002645833333333333},
    {"ms","time",0.001},{"s","time",1.0},{"sec","time",1.0},{"second","time",1.0},{"seconds","time",1.0},{"min","time",60.0},{"minute","time",60.0},{"minutes","time",60.0},{"h","time",3600.0},{"hour","time",3600.0},{"hours","time",3600.0},
    {"day","time",86400.0},{"days","time",86400.0},{"week","time",604800.0},{"weeks","time",604800.0},{"wk","time",604800.0},{"month","time",2629746.0},{"months","time",2629746.0},{"mo","time",2629746.0},{"year","time",31556952.0},{"years","time",31556952.0},{"yr","time",31556952.0},
    {"mg","mass",0.000001},{"g","mass",0.001},{"kg","mass",1.0},{"lb","mass",0.45359237},{"slug","mass",14.59390293720636},{"oz","mass",0.028349523125},{"ct","mass",0.0002},{"t","mass",1000.0},{"ton","mass",907.18474},{"lt","mass",1016.0469088},{"st","mass",6.35029318},{"Da","mass",1.66053906660e-27},{"gr","mass",0.00006479891},{"dwt","mass",0.00155517384},{"ozt","mass",0.0311034768},{"lbt","mass",0.3732417216},
    {"B","information",1.0},{"KB","information",1000.0},{"MB","information",1000000.0},{"KiB","information",1024.0},{"MiB","information",1048576.0},
    {"sqin","area",0.00064516},{"sqft","area",0.09290304},{"ac","area",4046.8564224},{"are","area",100.0},{"ha","area",10000.0},{"barn","area",83.612736},
    {"gal","volume",0.003785411784},{"bbl","volume",0.158987294928},{"qt","volume",0.000946352946},{"p","volume",0.000473176473},{"cup","volume",0.0002365882365},{"floz","volume",0.0000295735295625},{"tbsp","volume",0.00001478676478125},{"tsp","volume",0.00000492892159375},{"bu","volume",0.03523907016688},{"cord","volume",3.624556363776},
    {"mps","speed",1.0},{"kph","speed",0.2777777777777778},{"mph","speed",0.44704},{"kn","speed",0.5144444444444445},{"fps","speed",0.3048},{"mach","speed",340.29},
    {"Gal","acceleration",0.01},
    {"N","force",1.0},{"dyn","force",1e-5},{"lbf","force",4.44822},{"kgf","force",9.80665},{"pdl","force",0.138254954376},
    {"Pa","pressure",1.0},{"atm","pressure",101325.0},{"bar","pressure",100000.0},{"pz","pressure",10000.0},{"Ba","pressure",0.1},{"mmHg","pressure",133.3223684},{"psi","pressure",6894.757293},{"Torr","pressure",133.3223684},
    {"J","energy",1.0},{"Wh","energy",3600.0},{"cal","energy",4.184},{"BTU","energy",1055.05585262},{"eV","energy",1.602176565e-19},{"erg","energy",1e-7},{"th","energy",4186800.0},{"thm","energy",105506000.0},{"N.m","energy",1.0},{"kg.m2/s2","energy",1.0},
    {"W","power",1.0},{"hp","power",745.69987158227},{"J/s","power",1.0},{"N.m/s","power",1.0},
    {"A","current",1.0},{"statA","current",3.335641e-10},{"abA","current",10.0},{"Bi","current",10.0},
    {"V","potential",1.0},{"statV","potential",299.792458},{"abV","potential",1e-8},{"W/A","potential",1.0},
    {"Ohm","resistance",1.0},{"statOhm","resistance",898755178740.0},{"abOhm","resistance",1e-9},{"V/A","resistance",1.0},
    {"S","conductance",1.0},{"A/V","conductance",1.0},
    {"C","charge",1.0},{"statC","charge",3.335641e-10},{"abC","charge",10.0},{"Fr","charge",3.335641e-10},{"A.s","charge",1.0},
    {"F","capacitance",1.0},{"C/V","capacitance",1.0},{"Daraf","elastance",1.0},{"1/F","elastance",1.0},
    {"H","inductance",1.0},{"abH","inductance",1e-9},{"V.s/A","inductance",1.0},
    {"Wb","magnetic flux",1.0},{"Mx","magnetic flux",1e-8},{"V.s","magnetic flux",1.0},
    {"T","magnetic flux density",1.0},{"G","magnetic flux density",1e-4},{"Wb/m2","magnetic flux density",1.0},
    {"K","temperature",1.0},{"degC","temperature",1.0},{"degF","temperature",5.0/9.0},{"degR","temperature",5.0/9.0},
    {"B","information",1.0},{"b","information",0.125},{"KiB","information",1024.0},{"MiB","information",1048576.0},{"GiB","information",1073741824.0},{"TiB","information",1099511627776.0},{"PiB","information",1125899906842624.0},{"EiB","information",1152921504606846976.0},
    {"rad","angle",1.0},{"deg","angle",0.017453292519943295},{"grad","angle",0.015707963267948967},{"arcmin","angle",0.0002908882086657216},{"arcsec","angle",0.00000484813681109536},
    {"mol","substance",1.0},{"kat","mole flow rate",1.0},{"mol/s","mole flow rate",1.0},{"Hz","frequency",1.0},{"1/s","frequency",1.0},
    {"Bq","frequency",1.0},{"Ci","frequency",37000000000.0},{"Gy","radiation",1.0},{"Sv","radiation",1.0},{"R","radiation",0.01},{"J/kg","radiation",1.0},
    {"P","viscosity",0.1},{"dPa.s","viscosity",0.1},{"Pa.s","viscosity",1.0},{"St","kinematic viscosity",1.0},{"m2/s","kinematic viscosity",1.0},
    {"rpm","angular velocity",0.1047},{"rad/s","angular velocity",1.0},{"clo","thermal insulance",0.155},{"m2.K/W","thermal insulance",1.0},
    {"bps","data-transfer rate",1.0},{"bit/s","data-transfer rate",1.0},{"bits/s","data-transfer rate",1.0},{"cd","luminosity",1.0},{"lx","illuminance",1.0},{"Lb","illuminance",1.0},{"cd/m2","illuminance",1.0},{"lm","luminous flux",1.0},{"cd.sr","luminous flux",1.0},{"sr","solid angle",1.0},
    {NULL,NULL,0.0}
};
static UnitDef CUSTOM_UNITS[64];static char *CUSTOM_UNIT_PROPERTIES[64],*CUSTOM_UNIT_SYMBOLS[64];static double CUSTOM_UNIT_SCALE[64],CUSTOM_UNIT_BASE_FACTOR[64];static int CUSTOM_UNIT_COUNT=0;
static int custom_unit_index(const char *unit){for(int i=0;i<CUSTOM_UNIT_COUNT;i++)if(!strcmp(CUSTOM_UNITS[i].unit,unit))return i;return -1;}
static const char *custom_unit_property(const char *unit){int i=custom_unit_index(unit);return i>=0?CUSTOM_UNIT_PROPERTIES[i]:NULL;}
static const char *custom_unit_symbol(const char *unit){int i=custom_unit_index(unit);return i>=0?CUSTOM_UNIT_SYMBOLS[i]:NULL;}
static const char *custom_unit_dimension(const char *unit){int i=custom_unit_index(unit);return i>=0?CUSTOM_UNITS[i].dimension:NULL;}
static int metric_length_factor(const char *unit,double *factor){
    static const struct {const char *suffix;double base;} bases[]={{"px",0.0002645833333333333},{"pt",0.0003527777777777778},{"pc",0.004233333333333333},{"m",1.0},{NULL,0.0}};
    static const struct {const char *prefix;double factor;} prefixes[]={{"",1.0},{"a",1e-18},{"f",1e-15},{"p",1e-12},{"n",1e-9},{"u",1e-6},{"m",1e-3},{"c",1e-2},{"d",1e-1},{"da",1e1},{"h",1e2},{"k",1e3},{"M",1e6},{"G",1e9},{"T",1e12},{"P",1e15},{"E",1e18},{NULL,0.0}};
    for(int b=0;bases[b].suffix;b++){size_t ul=strlen(unit),sl=strlen(bases[b].suffix);if(ul<sl||strcmp(unit+ul-sl,bases[b].suffix))continue;size_t pl=ul-sl;for(int p=0;prefixes[p].prefix;p++)if(strlen(prefixes[p].prefix)==pl&&!strncmp(unit,prefixes[p].prefix,pl)){*factor=bases[b].base*prefixes[p].factor;return 1;}}
    return 0;
}
static int metric_liter_factor(const char *unit,double *factor){
    size_t z=strlen(unit);if(!z||unit[z-1]!='L')return 0;char prefix[8];size_t pz=z-1;if(pz>=sizeof prefix)return 0;memcpy(prefix,unit,pz);prefix[pz]=0;
    static const struct {const char *prefix;double factor;} prefixes[]={{"",1e-3},{"a",1e-21},{"f",1e-18},{"p",1e-15},{"n",1e-12},{"u",1e-9},{"m",1e-6},{"c",1e-5},{"d",1e-4},{"da",1e-2},{"h",1e-1},{"k",1.0},{"M",1e3},{"G",1e6},{"T",1e9},{"P",1e12},{"E",1e15},{NULL,0.0}};
    for(int i=0;prefixes[i].prefix;i++)if(!strcmp(prefix,prefixes[i].prefix)){*factor=prefixes[i].factor;return 1;}return 0;
}
static int metric_second_factor(const char *unit,double *factor){
    size_t z=strlen(unit);if(!z||unit[z-1]!='s')return 0;char prefix[8];size_t pz=z-1;if(pz>=sizeof prefix)return 0;memcpy(prefix,unit,pz);prefix[pz]=0;
    static const struct {const char *prefix;double factor;} prefixes[]={{"",1.0},{"a",1e-18},{"f",1e-15},{"p",1e-12},{"n",1e-9},{"u",1e-6},{"m",1e-3},{"c",1e-2},{"d",1e-1},{"da",1e1},{"h",1e2},{"k",1e3},{"M",1e6},{"G",1e9},{"T",1e12},{"P",1e15},{"E",1e18},{NULL,0.0}};
    for(int i=0;prefixes[i].prefix;i++)if(!strcmp(prefix,prefixes[i].prefix)){*factor=prefixes[i].factor;return 1;}return 0;
}
static int metric_gram_factor(const char *unit,double *factor){
    size_t z=strlen(unit);if(!z||unit[z-1]!='g')return 0;char prefix[8];size_t pz=z-1;if(pz>=sizeof prefix)return 0;memcpy(prefix,unit,pz);prefix[pz]=0;
    static const struct {const char *prefix;double factor;} prefixes[]={{"",1e-3},{"a",1e-21},{"f",1e-18},{"p",1e-15},{"n",1e-12},{"u",1e-9},{"m",1e-6},{"c",1e-5},{"d",1e-4},{"da",1e-2},{"h",1e-1},{"k",1.0},{"M",1e3},{"G",1e6},{"T",1e9},{"P",1e12},{"E",1e15},{NULL,0.0}};
    for(int i=0;prefixes[i].prefix;i++)if(!strcmp(prefix,prefixes[i].prefix)){*factor=prefixes[i].factor;return 1;}return 0;
}
static int metric_newton_factor(const char *unit,double *factor){
    size_t z=strlen(unit);if(!z||unit[z-1]!='N')return 0;char prefix[8];size_t pz=z-1;if(pz>=sizeof prefix)return 0;memcpy(prefix,unit,pz);prefix[pz]=0;
    static const struct {const char *prefix;double factor;} prefixes[]={{"",1.0},{"a",1e-18},{"f",1e-15},{"p",1e-12},{"n",1e-9},{"u",1e-6},{"m",1e-3},{"c",1e-2},{"d",1e-1},{"da",1e1},{"h",1e2},{"k",1e3},{"M",1e6},{"G",1e9},{"T",1e12},{"P",1e15},{"E",1e18},{NULL,0.0}};
    for(int i=0;prefixes[i].prefix;i++)if(!strcmp(prefix,prefixes[i].prefix)){*factor=prefixes[i].factor;return 1;}return 0;
}
static int metric_named_factor(const char *unit,const char *suffix,double base,double *factor){
    size_t z=strlen(unit),sz=strlen(suffix);if(z<sz||strcmp(unit+z-sz,suffix))return 0;size_t pz=z-sz;if(pz>=8)return 0;char prefix[8];memcpy(prefix,unit,pz);prefix[pz]=0;
    static const struct {const char *prefix;double factor;} prefixes[]={{"",1.0},{"a",1e-18},{"f",1e-15},{"p",1e-12},{"n",1e-9},{"u",1e-6},{"m",1e-3},{"c",1e-2},{"d",1e-1},{"da",1e1},{"h",1e2},{"k",1e3},{"M",1e6},{"G",1e9},{"T",1e12},{"P",1e15},{"E",1e18},{NULL,0.0}};
    for(int i=0;prefixes[i].prefix;i++)if(!strcmp(prefix,prefixes[i].prefix)){*factor=base*prefixes[i].factor;return 1;}return 0;
}
static int metric_information_factor(const char *unit,double *factor){
    if(metric_named_factor(unit,"B",1.0,factor))return 1;
    if(metric_named_factor(unit,"b",0.125,factor))return 1;
    return 0;
}
static const UnitDef *unit_def(const char *unit){
    static const UnitDef currency={"currency","currency",1.0};static UnitDef synthesized[4];static int slot=0;
    for(int i=0;UNIT_DEFS[i].unit;i++)if(!strcmp(UNIT_DEFS[i].unit,unit))return &UNIT_DEFS[i];for(int i=0;i<CUSTOM_UNIT_COUNT;i++)if(!strcmp(CUSTOM_UNITS[i].unit,unit))return &CUSTOM_UNITS[i];if(is_currency_unit(unit))return &currency;
    double factor;if(metric_length_factor(unit,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="length";result->factor=factor;return result;}
    if(metric_liter_factor(unit,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="volume";result->factor=factor;return result;}
    if(metric_second_factor(unit,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="time";result->factor=factor;return result;}
    if(metric_gram_factor(unit,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="mass";result->factor=factor;return result;}
    if(metric_newton_factor(unit,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="force";result->factor=factor;return result;}
    if(metric_named_factor(unit,"Pa",1.0,&factor)||metric_named_factor(unit,"bar",100000.0,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="pressure";result->factor=factor;return result;}
    if(metric_named_factor(unit,"J",1.0,&factor)||metric_named_factor(unit,"Wh",3600.0,&factor)||metric_named_factor(unit,"cal",4.184,&factor)||metric_named_factor(unit,"eV",1.602176565e-19,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="energy";result->factor=factor;return result;}
    if(metric_named_factor(unit,"W",1.0,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="power";result->factor=factor;return result;}
    if(metric_named_factor(unit,"A",1.0,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="current";result->factor=factor;return result;}
    if(metric_information_factor(unit,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="information";result->factor=factor;return result;}
    static const struct {const char *suffix,*dimension;double base;} namedFamilies[]={{"V","potential",1.0},{"Ohm","resistance",1.0},{"S","conductance",1.0},{"C","charge",1.0},{"F","capacitance",1.0},{"Daraf","elastance",1.0},{"H","inductance",1.0},{"Wb","magnetic flux",1.0},{"Mx","magnetic flux",1e-8},{"T","magnetic flux density",1.0},{"G","magnetic flux density",1e-4},{"mol","substance",1.0},{"kat","mole flow rate",1.0},{"Hz","frequency",1.0},{"Bq","frequency",1.0},{"Ci","frequency",37000000000.0},{"Gy","radiation",1.0},{"Sv","radiation",1.0},{"R","radiation",0.01},{"P","viscosity",0.1},{"St","kinematic viscosity",1.0},{"rpm","angular velocity",0.1047},{"bps","data-transfer rate",1.0},{"cd","luminosity",1.0},{"lx","illuminance",1.0},{"lm","luminous flux",1.0},{NULL,NULL,0.0}};
    for(int i=0;namedFamilies[i].suffix;i++)if(metric_named_factor(unit,namedFamilies[i].suffix,namedFamilies[i].base,&factor)){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension=namedFamilies[i].dimension;result->factor=factor;return result;}
    const char *slash=strrchr(unit,'/');if(slash){char numerator[128],denominator[128];size_t nl=(size_t)(slash-unit);if(nl>=sizeof numerator)nl=sizeof numerator-1;memcpy(numerator,unit,nl);numerator[nl]=0;snprintf(denominator,sizeof denominator,"%s",slash+1);int denominatorPower=1;size_t dl=strlen(denominator);if(dl>1&&denominator[dl-1]=='2'){denominatorPower=2;denominator[dl-1]=0;}const UnitDef *dd=unit_def(denominator);char *dot=strchr(numerator,'.');if(dot&&dd&&!strcmp(dd->dimension,"time")&&denominatorPower==2){*dot=0;const UnitDef *a=unit_def(numerator),*b=unit_def(dot+1);if(a&&b&&((!strcmp(a->dimension,"mass")&&!strcmp(b->dimension,"length"))||(!strcmp(a->dimension,"length")&&!strcmp(b->dimension,"mass"))||(!strcmp(a->dimension,"length")&&!strcmp(b->dimension,"length")))){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension="force";result->factor=a->factor*b->factor/pow(dd->factor,2.0);return result;}}const UnitDef *nd=unit_def(numerator);if(nd&&dd){UnitDef *result=&synthesized[slot++%4];result->unit=unit;if(!strcmp(nd->dimension,"force")&&((!strcmp(dd->dimension,"area")&&denominatorPower==1)||(!strcmp(dd->dimension,"length")&&denominatorPower==2))){result->dimension="pressure";result->factor=nd->factor/pow(dd->factor,(double)denominatorPower);return result;}if(!strcmp(dd->dimension,"time")){if(!strcmp(nd->dimension,"length")&&denominatorPower==1){result->dimension="speed";result->factor=nd->factor/dd->factor;return result;}if((!strcmp(nd->dimension,"length")&&denominatorPower==2)||(!strcmp(nd->dimension,"speed")&&denominatorPower==1)){result->dimension="acceleration";result->factor=nd->factor/pow(dd->factor,(double)denominatorPower);return result;}}}}
    size_t z=strlen(unit);if(z>1&&(unit[z-1]=='2'||unit[z-1]=='3')){
        int power=unit[z-1]-'0';char base[128];if(z>=sizeof base)z=sizeof base-1;memcpy(base,unit,z-1);base[z-1]=0;const UnitDef *baseDef=NULL;
        for(int i=0;UNIT_DEFS[i].unit;i++)if(!strcmp(UNIT_DEFS[i].unit,base)){baseDef=&UNIT_DEFS[i];break;}
        double baseFactor;if(!baseDef&&metric_length_factor(base,&baseFactor)){UnitDef *temporary=&synthesized[slot++%4];temporary->unit=unit;temporary->dimension="length";temporary->factor=baseFactor;baseDef=temporary;}
        if(baseDef&&!strcmp(baseDef->dimension,"length")){UnitDef *result=&synthesized[slot++%4];result->unit=unit;result->dimension=power==2?"area":"volume";result->factor=pow(baseDef->factor,(double)power);return result;}
    }
    return NULL;
}
static Value b_specify(Env*e,Value*a,int n){
    (void)e;(void)n;if(CUSTOM_UNIT_COUNT>=64)die("too many custom units");char *name=val_str(a[0]);const char *base=a[1].k==V_QUANTITY?a[1].u.quantity.unit:(a[1].k==V_UNIT?a[1].u.s:NULL);const UnitDef *definition=base?unit_def(base):NULL;double baseFactor=definition?definition->factor:1.0,scale=a[1].k==V_QUANTITY?a[1].u.quantity.amount:1.0,factor=scale*baseFactor;const char *dimension=definition?definition->dimension:name;char *property=rt_has_attr("describes")?val_str(rt_attr_value("describes",v_str(name))):strdup(name);for(char *p=property;*p;p++)*p=(char)tolower((unsigned char)*p);char *symbol=rt_has_attr("symbol")?val_str(rt_attr_value("symbol",v_str(name))):strdup(name);CUSTOM_UNITS[CUSTOM_UNIT_COUNT].unit=strdup(name);CUSTOM_UNITS[CUSTOM_UNIT_COUNT].dimension=strdup(dimension);CUSTOM_UNITS[CUSTOM_UNIT_COUNT].factor=factor;CUSTOM_UNIT_SCALE[CUSTOM_UNIT_COUNT]=scale;CUSTOM_UNIT_BASE_FACTOR[CUSTOM_UNIT_COUNT]=baseFactor;CUSTOM_UNIT_PROPERTIES[CUSTOM_UNIT_COUNT]=property;CUSTOM_UNIT_SYMBOLS[CUSTOM_UNIT_COUNT]=symbol;CUSTOM_UNIT_COUNT++;Value result=v_str(name);free(name);return result;
}
static double quantity_convert_amount(Value q,const char *target){
    const UnitDef *from=unit_def(q.u.quantity.unit),*to=unit_def(target);
    if(!strcmp(q.u.quantity.unit,target))return q.u.quantity.amount;
    if(!from||!to||strcmp(from->dimension,to->dimension))die("cannot convert different quantity dimensions");
    if(!strcmp(from->dimension,"temperature")){double kelvin=q.u.quantity.amount;if(!strcmp(q.u.quantity.unit,"degC"))kelvin+=273.1;else if(!strcmp(q.u.quantity.unit,"degF"))kelvin=(q.u.quantity.amount+459.67)*5.0/9.0;else if(!strcmp(q.u.quantity.unit,"degR"))kelvin=q.u.quantity.amount*5.0/9.0;kelvin=round(kelvin*10.0)/10.0;if(!strcmp(target,"degC"))return kelvin-273.1;if(!strcmp(target,"degF"))return kelvin*9.0/5.0-459.67;if(!strcmp(target,"degR"))return kelvin*9.0/5.0;return kelvin;}
    int customFrom=custom_unit_index(q.u.quantity.unit);double baseAmount=q.u.quantity.amount;
    if(customFrom>=0)baseAmount=(baseAmount*CUSTOM_UNIT_SCALE[customFrom])*CUSTOM_UNIT_BASE_FACTOR[customFrom];else baseAmount*=from->factor;
    double result=baseAmount/to->factor;
    if(fabs(result)>1e-9){double rounded=round(result*1e12)/1e12;if(fabs(result-rounded)<=1e-12*fmax(1.0,fabs(result)))result=rounded;}
    return result;
}
static Value b_add(Env*e,Value*a,int n){
    if(a[0].k==V_INT&&a[1].k==V_INT)return int_add(a[0].u.i,a[1].u.i);
    if(a[0].k==V_COLOR&&a[1].k==V_COLOR)return color_rgba(fmin(255,color_chan(a[0],24)+color_chan(a[1],24)),fmin(255,color_chan(a[0],16)+color_chan(a[1],16)),fmin(255,color_chan(a[0],8)+color_chan(a[1],8)),fmin(255,color_chan(a[0],0)+color_chan(a[1],0)));
    if(a[0].k==V_QUANTITY&&a[1].k==V_QUANTITY){double x=a[0].u.quantity.amount+quantity_convert_amount(a[1],a[0].u.quantity.unit);return a[0].u.quantity.integral&&a[1].u.quantity.integral&&floor(x)==x?v_quantity_int((long)x,a[0].u.quantity.unit):v_quantity(x,a[0].u.quantity.unit);}
    if(a[0].k==V_QUANTITY&&a[1].k==V_COMPLEX){die("add: quantity cannot receive complex");return v_null();}
    if(a[0].k==V_COMPLEX&&a[1].k==V_QUANTITY){die("add: complex cannot receive quantity");return v_null();}
    if(a[0].k==V_QUANTITY&&(a[1].k==V_INT||a[1].k==V_FLOAT||a[1].k==V_RATIONAL)){double x=a[0].u.quantity.amount+as_float(a[1]);return a[0].u.quantity.integral&&a[1].k==V_INT?v_quantity_int((long)x,a[0].u.quantity.unit):v_quantity(x,a[0].u.quantity.unit);}
    if(a[1].k==V_QUANTITY&&(a[0].k==V_INT||a[0].k==V_FLOAT||a[0].k==V_RATIONAL)){die("add: quantity must be the left operand");return v_null();}
    if(a[0].k==V_RATIONAL&&a[1].k==V_COMPLEX){die("add: rational cannot receive complex");return v_null();}
    if(a[0].k==V_COMPLEX||a[1].k==V_COMPLEX){double ar,ai,br,bi;complex_parts(a[0],&ar,&ai);complex_parts(a[1],&br,&bi);return v_complex(ar+br,ai+bi);}
    if(a[0].k==V_FLOAT||a[1].k==V_FLOAT)return numf(as_float(a[0])+as_float(a[1]));
    if(a[0].k==V_RATIONAL||a[1].k==V_RATIONAL){long an,ad,bn,bd;rational_parts(a[0],&an,&ad);rational_parts(a[1],&bn,&bd);return v_rational(an*bd+bn*ad,ad*bd);}
    if(!is_numeric_kind(a[0])||!is_numeric_kind(a[1])){die("add: expected number");return v_null();}
    return int_op(IOP_ADD, a[0], a[1]);
}
static Value b_sub(Env*e,Value*a,int n){
    if(a[0].k==V_INT&&a[1].k==V_INT)return int_sub(a[0].u.i,a[1].u.i);
    if(a[0].k==V_COLOR&&a[1].k==V_COLOR)return color_rgba(fmax(0,color_chan(a[0],24)-color_chan(a[1],24)),fmax(0,color_chan(a[0],16)-color_chan(a[1],16)),fmax(0,color_chan(a[0],8)-color_chan(a[1],8)),fmax(0,color_chan(a[0],0)-color_chan(a[1],0)));
    if(a[0].k==V_QUANTITY&&a[1].k==V_QUANTITY){double x=a[0].u.quantity.amount-quantity_convert_amount(a[1],a[0].u.quantity.unit);return a[0].u.quantity.integral&&a[1].u.quantity.integral&&floor(x)==x?v_quantity_int((long)x,a[0].u.quantity.unit):v_quantity(x,a[0].u.quantity.unit);}
    if(a[0].k==V_QUANTITY&&(a[1].k==V_INT||a[1].k==V_FLOAT||a[1].k==V_RATIONAL)){double x=a[0].u.quantity.amount-as_float(a[1]);return a[0].u.quantity.integral&&a[1].k==V_INT?v_quantity_int((long)x,a[0].u.quantity.unit):v_quantity(x,a[0].u.quantity.unit);}
    if(a[0].k==V_RATIONAL&&a[1].k==V_COMPLEX){die("sub: rational cannot receive complex");return v_null();}
    if(a[0].k==V_COMPLEX||a[1].k==V_COMPLEX){double ar,ai,br,bi;complex_parts(a[0],&ar,&ai);complex_parts(a[1],&br,&bi);return v_complex(ar-br,ai-bi);}
    if(a[0].k==V_RATIONAL||a[1].k==V_RATIONAL){long an,ad,bn,bd;rational_parts(a[0],&an,&ad);rational_parts(a[1],&bn,&bd);return v_rational(an*bd-bn*ad,ad*bd);}
    if(a[0].k==V_FLOAT||a[1].k==V_FLOAT)return numf(as_float(a[0])-as_float(a[1]));
    if(!is_numeric_kind(a[0])||!is_numeric_kind(a[1])){die("sub: expected number");return v_null();}
    return int_op(IOP_SUB, a[0], a[1]);
}
static void simple_unit_power(const char *unit,char *base,size_t cap,int *power){
    size_t z=strlen(unit);*power=1;
    if(z&&unit[z-1]>='2'&&unit[z-1]<='9'){*power=unit[z-1]-'0';z--;}
    if(z>=cap)z=cap-1;memcpy(base,unit,z);base[z]=0;
}
static char *quantity_product_unit(const char *left,const char *right){
    char lb[128],rb[128];int lp,rp;simple_unit_power(left,lb,sizeof lb,&lp);simple_unit_power(right,rb,sizeof rb,&rp);
    size_t z=strlen(left)+strlen(right)+16;char *unit=xmalloc(z);
    if(!strcmp(lb,rb)){int power=lp+rp;if(power==1)snprintf(unit,z,"%s",lb);else snprintf(unit,z,"%s%d",lb,power);}
    else {const UnitDef *ld=unit_def(left),*rd=unit_def(right);const char *shown=right;if(ld&&rd&&!strcmp(ld->dimension,rd->dimension)){if(!strcmp(right,"hour"))shown="h";else if(!strcmp(right,"minute"))shown="min";}const char *slash=strrchr(left,'/');if(slash){size_t numerator=(size_t)(slash-left);snprintf(unit,z,"%.*s.%s/%s",(int)numerator,left,shown,slash+1);}else snprintf(unit,z,"%s.%s",left,shown);}
    return unit;
}
static char *quantity_quotient_unit(const char *left,const char *right,int *reduced){
    char lb[128],rb[128];int lp,rp;simple_unit_power(left,lb,sizeof lb,&lp);simple_unit_power(right,rb,sizeof rb,&rp);*reduced=0;
    size_t z=strlen(left)+strlen(right)+16;char *unit=xmalloc(z);
    if(!strcmp(lb,rb)&&lp>rp){int power=lp-rp;if(power==1)snprintf(unit,z,"%s",lb);else snprintf(unit,z,"%s%d",lb,power);*reduced=1;}
    else {const char *shown=right;if(!strcmp(right,"hour"))shown="h";else if(!strcmp(right,"minute"))shown="min";snprintf(unit,z,"%s/%s",left,shown);}
    return unit;
}
static void normalize_simple_unit(const char *unit,char *out,size_t cap){
    const char *slash=strchr(unit,'/');if(!slash){snprintf(out,cap,"%s",unit);return;}
    size_t leftLen=(size_t)(slash-unit);char left[128],right[128],lb[128],rb[128];if(leftLen>=sizeof left)leftLen=sizeof left-1;memcpy(left,unit,leftLen);left[leftLen]=0;snprintf(right,sizeof right,"%s",slash+1);int lp,rp;simple_unit_power(left,lb,sizeof lb,&lp);simple_unit_power(right,rb,sizeof rb,&rp);
    if(!strcmp(lb,rb)&&lp>rp){int power=lp-rp;if(power==1)snprintf(out,cap,"%s",lb);else snprintf(out,cap,"%s%d",lb,power);return;}snprintf(out,cap,"%s",unit);
}
static Value b_mul(Env*e,Value*a,int n){
    if(a[0].k==V_INT&&a[1].k==V_INT)return int_mul(a[0].u.i,a[1].u.i);
    if((a[0].k==V_QUANTITY&&a[1].k==V_COMPLEX)||(a[0].k==V_COMPLEX&&a[1].k==V_QUANTITY)){die("mul: complex quantity unsupported");return v_null();}
    if(a[0].k==V_QUANTITY&&a[1].k==V_QUANTITY){
        const char *leftUnit=a[0].u.quantity.unit,*rightUnit=a[1].u.quantity.unit;const UnitDef *ld=unit_def(leftUnit),*rd=unit_def(rightUnit);double rightAmount=a[1].u.quantity.amount;
        int convertSameDimension=ld&&rd&&!strcmp(ld->dimension,rd->dimension)&&strcmp(ld->dimension,"time");
        if(convertSameDimension)rightAmount=quantity_convert_amount(a[1],leftUnit);
        char *unit=quantity_product_unit(leftUnit,convertSameDimension?leftUnit:rightUnit);
        double x=a[0].u.quantity.amount*rightAmount;Value result=a[0].u.quantity.integral&&a[1].u.quantity.integral&&floor(x)==x?v_quantity_int((long)x,unit):v_quantity(x,unit);free(unit);return result;
    }
    if(a[0].k==V_QUANTITY&&(a[1].k==V_INT||a[1].k==V_FLOAT||a[1].k==V_RATIONAL)){double x=a[0].u.quantity.amount*as_float(a[1]);return a[0].u.quantity.integral&&a[1].k==V_INT?v_quantity_int((long)x,a[0].u.quantity.unit):v_quantity(x,a[0].u.quantity.unit);}
    if(a[1].k==V_QUANTITY&&(a[0].k==V_INT||a[0].k==V_FLOAT||a[0].k==V_RATIONAL)){double x=a[1].u.quantity.amount*as_float(a[0]);return a[1].u.quantity.integral&&a[0].k==V_INT?v_quantity_int((long)x,a[1].u.quantity.unit):v_quantity(x,a[1].u.quantity.unit);}
    if(a[0].k==V_RATIONAL&&a[1].k==V_COMPLEX){die("mul: rational cannot receive complex");return v_null();}
    if(a[0].k==V_COMPLEX||a[1].k==V_COMPLEX){double ar,ai,br,bi;complex_parts(a[0],&ar,&ai);complex_parts(a[1],&br,&bi);return v_complex(ar*br-ai*bi,ar*bi+ai*br);}
    if(a[0].k==V_FLOAT||a[1].k==V_FLOAT)return numf(as_float(a[0])*as_float(a[1]));
    if(a[0].k==V_RATIONAL||a[1].k==V_RATIONAL){long an,ad,bn,bd;rational_parts(a[0],&an,&ad);rational_parts(a[1],&bn,&bd);return v_rational(an*bn,ad*bd);}
    if(!is_numeric_kind(a[0])||!is_numeric_kind(a[1])){die("mul: expected number");return v_null();}
    return int_op(IOP_MUL, a[0], a[1]);
}
static Value b_div(Env*e,Value*a,int n){
    if(a[0].k==V_INT&&a[1].k==V_INT){long d=a[1].u.i;if(d==0)die("division by zero");return num2(a[0].u.i/d);}
    if(a[0].k==V_QUANTITY&&(a[1].k==V_INT||a[1].k==V_FLOAT||a[1].k==V_RATIONAL)){double denominator=as_float(a[1]);if(denominator==0.0)die("division by zero");return v_quantity(a[0].u.quantity.amount/denominator,a[0].u.quantity.unit);}
    if(a[0].k==V_QUANTITY&&a[1].k==V_QUANTITY){
        const char *leftProperty=unit_property(a[0].u.quantity.unit),*rightProperty=unit_property(a[1].u.quantity.unit);
        if(!strcmp(leftProperty,rightProperty)&&strcmp(leftProperty,"time"))return v_float(a[0].u.quantity.amount/quantity_convert_amount(a[1],a[0].u.quantity.unit));
        double denominator=a[1].u.quantity.amount;if(denominator==0.0)die("division by zero");int reduced;char *unit=quantity_quotient_unit(a[0].u.quantity.unit,a[1].u.quantity.unit,&reduced);double x=a[0].u.quantity.amount/denominator;Value result=a[0].u.quantity.integral&&a[1].u.quantity.integral&&floor(x)==x?v_quantity_int((long)x,unit):v_quantity(x,unit);free(unit);return result;
    }
    if(a[0].k==V_RATIONAL&&a[1].k==V_COMPLEX){die("div: rational cannot receive complex");return v_null();}
    if(a[0].k==V_COMPLEX||a[1].k==V_COMPLEX){double ar,ai,br,bi;complex_parts(a[0],&ar,&ai);complex_parts(a[1],&br,&bi);double d=br*br+bi*bi;if(d==0.0)die("division by zero");return v_complex((ar*br+ai*bi)/d,(ai*br-ar*bi)/d);}
    if(a[0].k==V_FLOAT||a[1].k==V_FLOAT){double denominator=as_float(a[1]);if(denominator==0.0)die("division by zero");return v_float(as_float(a[0])/denominator);}
    if(a[0].k==V_RATIONAL||a[1].k==V_RATIONAL){long an,ad,bn,bd;rational_parts(a[0],&an,&ad);rational_parts(a[1],&bn,&bd);if(bn==0)die("division by zero");return v_rational(an*bd,ad*bn);}
    if(!is_numeric_kind(a[0])||!is_numeric_kind(a[1])){die("div: expected number");return v_null();}
    if(a[0].k==V_BIGINT||a[1].k==V_BIGINT){
        char *x=val_str(a[0]),*y=val_str(a[1]);
        char *q,*r; big_divmod(x,y,&q,&r); free(x); free(y); free(r);
        Value out = v_bigint_text(q); free(q); return out;
    }
    long denominator=as_int(a[1]);
    if(denominator==0)die("division by zero");
    return num2(as_int(a[0])/denominator);
}
static Value b_fdiv(Env*e,Value*a,int n){
    if(a[0].k==V_QUANTITY&&a[1].k==V_QUANTITY){double divisor=quantity_convert_amount(a[1],a[0].u.quantity.unit);if(divisor==0.0)die("division by zero");return v_float(a[0].u.quantity.amount/divisor);}
    if(a[0].k==V_QUANTITY&&(a[1].k==V_INT||a[1].k==V_FLOAT||a[1].k==V_RATIONAL)){
        double denominator=as_float(a[1]);if(denominator==0.0)die("division by zero");double x=a[0].u.quantity.amount/denominator;
        return a[0].u.quantity.integral&&floor(x)==x?v_quantity_int((long)x,a[0].u.quantity.unit):v_quantity(x,a[0].u.quantity.unit);
    }
    double denominator=as_float(a[1]);
    if(denominator==0.0)die("division by zero");
    if(!is_numeric_kind(a[0])||!is_numeric_kind(a[1])){die("fdiv: expected number");return v_null();}
    return numf(as_float(a[0])/denominator);
}
static Value b_mod(Env*e,Value*a,int n){
    if(a[0].k==V_INT&&a[1].k==V_INT){long d=a[1].u.i;if(d==0)die("division by zero");return v_int(a[0].u.i%d);}
    int mutate=a[0].k==V_PATH;Value left=a[0];if(mutate&&a[0].u.path.nsegs==1)left=env_get(e,a[0].u.path.segs[0]);if(left.k==V_COMPLEX||a[1].k==V_COMPLEX){die("mod: complex unsupported");return v_null();}Value result;
    if(left.k==V_FLOAT||a[1].k==V_FLOAT||left.k==V_RATIONAL||a[1].k==V_RATIONAL){double denominator=as_float(a[1]);if(denominator==0.0)die("division by zero");result=v_float(fmod(as_float(left),denominator));}
    else if(left.k==V_BIGINT||a[1].k==V_BIGINT){
        char *x=val_str(left),*y=val_str(a[1]);
        char *q,*r; big_divmod(x,y,&q,&r); free(x); free(y); free(q);
        result = v_bigint_text(r); free(r);
    }
    else {
        if(!is_numeric_kind(left)||!is_numeric_kind(a[1])){die("mod: expected number");return v_null();}
        long denominator=as_int(a[1]);if(denominator==0)die("division by zero");result=v_int(as_int(left)%denominator);
    }
    if(mutate&&a[0].u.path.nsegs==1){env_set(e,a[0].u.path.segs[0],result);return v_null();}return result;
}
static long integer_gcd(long a,long b){
    if(a<0)a=-a;if(b<0)b=-b;
    while(b){long r=a%b;a=b;b=r;}return a?a:1;
}
static void floating_fraction(double value,long *numerator,long *denominator){
    int negative=value<0.0;if(negative)value=-value;
    long h0=0,h1=1,k0=1,k1=0;
    double x=value;
    for(int i=0;i<32;i++){
        long whole=(long)floor(x);long h2=whole*h1+h0,k2=whole*k1+k0;
        if(k2<=0||k2>1000000000L)break;
        h0=h1;h1=h2;k0=k1;k1=k2;
        double approximation=(double)h1/(double)k1;
        if(fabs(approximation-value)<1e-12)break;
        double remainder=x-(double)whole;if(remainder==0.0)break;x=1.0/remainder;
    }
    long divisor=integer_gcd(h1,k1);*numerator=(negative?-h1:h1)/divisor;*denominator=k1/divisor;
}
static Value b_numerator(Env*e,Value*a,int n){
    if(a[0].k==V_INT)return a[0];
    if(a[0].k==V_RATIONAL)return v_int(a[0].u.rational.num);
    long numerator,denominator;floating_fraction(as_float(a[0]),&numerator,&denominator);
    return v_int(numerator);
}
static Value b_denominator(Env*e,Value*a,int n){
    if(a[0].k==V_INT)return v_int(1);
    if(a[0].k==V_RATIONAL)return v_int(a[0].u.rational.den);
    long numerator,denominator;floating_fraction(as_float(a[0]),&numerator,&denominator);
    return v_int(denominator);
}
static Value b_pow(Env*e,Value*a,int n){
    if(a[0].k==V_QUANTITY&&a[1].k==V_INT&&a[1].u.i>=0){
        long exponent=a[1].u.i;char base[128];int existing;simple_unit_power(a[0].u.quantity.unit,base,sizeof base,&existing);size_t z=strlen(base)+24;char *unit=xmalloc(z);long power=existing*exponent;if(power==1)snprintf(unit,z,"%s",base);else snprintf(unit,z,"%s%ld",base,power);double x=pow(a[0].u.quantity.amount,(double)exponent);Value result=a[0].u.quantity.integral?v_quantity_int((long)x,unit):v_quantity(x,unit);free(unit);return result;
    }
    if(a[0].k==V_INT && a[1].k==V_INT && a[1].u.i>=0){
        return int_pow(a[0].u.i, a[1].u.i);
    }
    if(a[0].k==V_BIGINT && a[1].k==V_INT && a[1].u.i>=0){
        return v_bigint_text(big_pow(a[0].u.s, a[1].u.i));
    }
    if(!is_numeric_kind(a[0])||!is_numeric_kind(a[1])){die("pow: expected number");return v_null();}
    return numf(pow(as_float(a[0]),as_float(a[1])));
}
static Value b_neg(Env*e,Value*a,int n);
static Value b_conj(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_COMPLEX)die("conj: expected complex");return v_complex(a[0].u.complex.real,-a[0].u.complex.imag);}
static Value b_angle(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_COMPLEX)die("angle: expected complex");return v_float(atan2(a[0].u.complex.imag,a[0].u.complex.real));}
static Value b_reciprocal(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k==V_INT)return v_rational(1,a[0].u.i);if(a[0].k==V_RATIONAL)return v_rational(a[0].u.rational.den,a[0].u.rational.num);
    if(a[0].k==V_FLOAT){double x=a[0].u.f;if(x==0.0)die("division by zero");long den=1;while(den<1000000000L&&fabs(x*den-round(x*den))>1e-12)den*=10;return v_rational(den,(long)round(x*den));}
    die("reciprocal: expected number");return v_null();
}
static Value b_inc(Env*e,Value*a,int n);
static Value b_dec(Env*e,Value*a,int n);
static int object_member_index(Value obj,const char *name){
    if(!object_type_name(obj))return -1;for(int i=0;i<obj.u.dict->n;i++)if(!strcmp(obj.u.dict->keys[i],name))return i;return -1;
}
static Value object_magic(Env *e,Value obj,const char *name,Value *args,int n){
    int i=object_member_index(obj,name);if(i<0||obj.u.dict->vals[i].k!=V_FUNC)die("missing object magic method");return applyFunc(e,obj.u.dict->vals[i],args,n);
}
static int is_binop(Value v);
static Value b_print(Env*e,Value*a,int n){
    int stringMethod=object_member_index(a[0],"string");
    if(stringMethod>=0&&a[0].u.dict->vals[stringMethod].k==V_FUNC){Value rendered=object_magic(e,a[0],"string",NULL,0);v_print(rendered);return v_null();}
    if(a[0].k==V_BLOCK){
        Block *source=a[0].u.block.b;Value **items=xmalloc((size_t)(source->n+1)*sizeof(Value*));int used=0;
        for(int i=0;i<source->n;i++){
            Value value=*source->items[i];
            if(value.k==V_PATH&&path_is_dyn(value))value=path_read(e,value);
            if(value.k==V_WORD){
                int boundHere=env_bound(e,value.u.s);
                if(boundHere){
                    Value bound=env_get(e,value.u.s);
                    if(bound.k==V_FUNC||bound.k==V_BUILTIN){
                        int arity=bound.k==V_FUNC?function_param_count(bound):declared_arity_of(bound.u.s);
                        if(arity>=0&&i+arity<source->n){Value stack[8];Value *argv=arity<=8?stack:xmalloc((size_t)(arity+1)*sizeof(Value));int ready=1;for(int j=0;j<arity;j++){argv[j]=*source->items[i+1+j];if(argv[j].k==V_WORD){if(env_bound(e,argv[j].u.s))argv[j]=env_get(e,argv[j].u.s);else ready=0;}}if(ready){value=applyFunc(e,bound,argv,arity);i+=arity;}if(argv!=stack)free(argv);}
                    }else value=bound;
                } else if(rt_builtin_known(value.u.s)){
                    /* an unbound word naming a builtin is a call head here too
                     * (`print ["data:" get btc 'data]`), matching the host's
                     * trailing-argument application. */
                    int arity=declared_arity_of(value.u.s);
                    /* `join.with: " " lst` — collect attribute tokens after the
                     * head before the positional args. */
                    const char *anames[8]; Value avals[8]; int acnt=0; int at=i+1;
                    while(at<source->n && acnt<8){
                        Value t=*source->items[at];
                        if(t.k==V_ATTRIBUTELABEL){ anames[acnt]=t.u.s; at++; if(at<source->n){ avals[acnt]=*source->items[at]; at++; } acnt++; continue; }
                        if(t.k==V_ATTRIBUTE){ anames[acnt]=t.u.s; avals[acnt]=v_bool(1); acnt++; at++; continue; }
                        break;
                    }
                    if(acnt>0){
                        AttrContext saved=g_attrs;
                        g_attrs.names=anames; g_attrs.values=avals; g_attrs.n=acnt;
                        if(arity>=0 && at+arity<=source->n){
                            Value stack[8];Value *argv=arity<=8?stack:xmalloc((size_t)(arity+1)*sizeof(Value));int ready=1;
                            for(int j=0;j<arity;j++){argv[j]=*source->items[at+j];if(argv[j].k==V_WORD){if(env_bound(e,argv[j].u.s))argv[j]=env_get(e,argv[j].u.s);else ready=0;}}
                            if(ready){Value out;if(rt_builtin(value.u.s,e,argv,arity,&out)){value=out;i=at+arity-1;}}
                            if(argv!=stack)free(argv);
                        }
                        g_attrs=saved;
                    } else if(arity>=0&&i+arity<source->n&&!(i+1<source->n&&is_binop(*source->items[i+1]))){
                        Value stack[8];Value *argv=arity<=8?stack:xmalloc((size_t)(arity+1)*sizeof(Value));int ready=1;
                        for(int j=0;j<arity;j++){argv[j]=*source->items[i+1+j];if(argv[j].k==V_WORD){if(env_bound(e,argv[j].u.s))argv[j]=env_get(e,argv[j].u.s);else ready=0;}}
                        if(ready){Value out;if(rt_builtin(value.u.s,e,argv,arity,&out)){value=out;i+=arity;}}
                        if(argv!=stack)free(argv);
                    }
                }
            }
            items[used]=xmalloc(sizeof(Value));*items[used++]=value;
        }
        v_print(v_block(items,used));return v_null();
    }
    v_print(a[0]);return v_null();
}
/* string-like kinds carry a name in u.s and compare by that name */
#define IS_STRLIKE(k) ((k)==V_STR||(k)==V_WORD||(k)==V_LABEL||(k)==V_LITERAL||(k)==V_SYMBOL||(k)==V_SYMBOLLITERAL||(k)==V_TYPE||(k)==V_VERSION||(k)==V_ERRORKIND||(k)==V_ATTRIBUTE||(k)==V_ATTRIBUTELABEL)
static int version_compare(const char *left,const char *right){
    const char *lp=left,*rp=right;
    for(int part=0;part<3;part++){
        char *lend,*rend;long l=strtol(lp,&lend,10),r=strtol(rp,&rend,10);lp=lend;rp=rend;
        if(l!=r)return (l>r)-(l<r);
        if(*lp=='.')lp++;if(*rp=='.')rp++;
    }
    const char *le=strchr(lp,'+'),*re=strchr(rp,'+');
    if(!le)le=lp+strlen(lp);if(!re)re=rp+strlen(rp);
    int lpre=*lp=='-',rpre=*rp=='-';
    if(lpre!=rpre)return lpre?-1:1;
    if(!lpre)return 0;
    lp++;rp++;
    while(lp<le||rp<re){
        if(lp>=le)return -1;if(rp>=re)return 1;
        const char *ln=lp,*rn=rp;while(ln<le&&*ln!='.')ln++;while(rn<re&&*rn!='.')rn++;
        int ld=lp<ln,rd=rp<rn;for(const char*p=lp;p<ln;p++)if(!isdigit((unsigned char)*p))ld=0;for(const char*p=rp;p<rn;p++)if(!isdigit((unsigned char)*p))rd=0;
        if(ld&&rd){long l=strtol(lp,NULL,10),r=strtol(rp,NULL,10);if(l!=r)return (l>r)-(l<r);}
        else if(ld!=rd)return ld?-1:1;
        else {size_t ll=(size_t)(ln-lp),rl=(size_t)(rn-rp),m=ll<rl?ll:rl;int c=strncmp(lp,rp,m);if(c)return (c>0)-(c<0);if(ll!=rl)return (ll>rl)-(ll<rl);}
        lp=ln<le?ln+1:ln;rp=rn<re?rn+1:rn;
    }
    return 0;
}
static Value b_equal(Env*e,Value*a,int n){
    if(a[0].k==V_INT&&a[1].k==V_INT)return v_bool(a[0].u.i==a[1].u.i);
    if(a[0].k==V_NULL||a[1].k==V_NULL)return v_bool(a[0].k==V_NULL&&a[1].k==V_NULL);
    if((a[0].k==V_BLOCK&&a[1].k==V_BLOCK)||(a[0].k==V_DICT&&a[1].k==V_DICT))return v_bool(value_eq(a[0],a[1]));
    if(a[0].k==V_UNIT&&a[1].k==V_UNIT){const char *left=canonical_unit(a[0].u.s),*right=canonical_unit(a[1].u.s);if((!strcmp(left,"in2")&&!strcmp(right,"sqin"))||(!strcmp(right,"in2")&&!strcmp(left,"sqin"))||(!strcmp(left,"ft2")&&!strcmp(right,"sqft"))||(!strcmp(right,"ft2")&&!strcmp(left,"sqft"))||(!strcmp(left,"metre2")&&!strcmp(right,"m2"))||(!strcmp(right,"metre2")&&!strcmp(left,"m2")))return v_bool(1);return v_bool(!strcmp(left,right));}
    if((a[0].k==V_UNIT&&IS_STRLIKE(a[1].k))||(a[1].k==V_UNIT&&IS_STRLIKE(a[0].k))){const char *left=canonical_unit(a[0].u.s),*right=canonical_unit(a[1].u.s);if((!strcmp(left,"in2")&&!strcmp(right,"sqin"))||(!strcmp(right,"in2")&&!strcmp(left,"sqin"))||(!strcmp(left,"ft2")&&!strcmp(right,"sqft"))||(!strcmp(right,"ft2")&&!strcmp(left,"sqft"))||(!strcmp(left,"metre2")&&!strcmp(right,"m2"))||(!strcmp(right,"metre2")&&!strcmp(left,"m2")))return v_bool(1);return v_bool(!strcmp(left,right));}
    if(a[0].k==V_QUANTITY&&a[1].k==V_QUANTITY){char leftUnit[256],rightUnit[256];normalize_simple_unit(a[0].u.quantity.unit,leftUnit,sizeof leftUnit);normalize_simple_unit(a[1].u.quantity.unit,rightUnit,sizeof rightUnit);double left=a[0].u.quantity.amount,right=!strcmp(leftUnit,rightUnit)?a[1].u.quantity.amount:quantity_convert_amount(a[1],a[0].u.quantity.unit),scale=fmax(1.0,fmax(fabs(left),fabs(right)));return v_bool(fabs(left-right)<=1e-12*scale);}
    if(a[0].k==V_QUANTITY&&(a[1].k==V_INT||a[1].k==V_FLOAT||a[1].k==V_RATIONAL))return v_bool(a[0].u.quantity.amount==as_float(a[1]));
    if(a[1].k==V_QUANTITY&&(a[0].k==V_INT||a[0].k==V_FLOAT||a[0].k==V_RATIONAL))return v_bool(a[1].u.quantity.amount==as_float(a[0]));
    if(a[0].k==V_BINARY&&a[1].k==V_BINARY)return v_bool(a[0].u.binary.len==a[1].u.binary.len&&!memcmp(a[0].u.binary.data,a[1].u.binary.data,a[0].u.binary.len));
    if(a[0].k==V_COLOR&&a[1].k==V_COLOR)return v_bool(a[0].u.rgba==a[1].u.rgba);
    if(a[0].k==V_DATE&&a[1].k==V_DATE)return v_bool(a[0].u.epoch==a[1].u.epoch);
    if(a[0].k==V_CHAR&&a[1].k==V_CHAR)return v_bool(!strcmp(a[0].u.c,a[1].u.c));
    if(a[0].k==V_RANGE&&a[1].k==V_RANGE)return v_bool(a[0].u.range.lo==a[1].u.range.lo&&a[0].u.range.hi==a[1].u.range.hi&&a[0].u.range.step==a[1].u.range.step&&a[0].u.range.character==a[1].u.range.character&&a[0].u.range.infinite==a[1].u.range.infinite);
    if(a[0].k==V_COMPLEX||a[1].k==V_COMPLEX){double ar,ai,br,bi;complex_parts(a[0],&ar,&ai);complex_parts(a[1],&br,&bi);return v_bool(ar==br&&ai==bi);}
    if(a[0].k==V_BIGINT||a[1].k==V_BIGINT){
        if(a[0].k==V_BIGINT&&a[1].k==V_BIGINT)return v_bool(!strcmp(a[0].u.s,a[1].u.s));
        if(is_numeric_kind(a[0])&&is_numeric_kind(a[1])){char *x=val_str(a[0]),*y=val_str(a[1]);int c=big_cmp(x,y);free(x);free(y);return v_bool(c==0);}
        return v_bool(0);
    }
    if (a[0].k==V_INT && a[1].k==V_INT) return v_bool(a[0].u.i==a[1].u.i);
    if (a[0].k==V_BOOL && a[1].k==V_BOOL) return v_bool(a[0].u.b==a[1].u.b);
    if ((a[0].k==V_FLOAT||a[0].k==V_RATIONAL) && (a[1].k==V_FLOAT||a[1].k==V_INT||a[1].k==V_RATIONAL)) return v_bool(as_float(a[0])==as_float(a[1]));
    if ((a[0].k==V_INT||a[0].k==V_FLOAT||a[0].k==V_RATIONAL) && (a[1].k==V_FLOAT||a[1].k==V_RATIONAL)) return v_bool(as_float(a[0])==as_float(a[1]));
    if (a[0].k==V_VERSION&&a[1].k==V_VERSION)return v_bool(version_compare(a[0].u.s,a[1].u.s)==0);
    if (IS_STRLIKE(a[0].k) && IS_STRLIKE(a[1].k)) {
        if(a[0].u.s[0]==':'&&a[1].u.s[0]==':')return v_bool(!strcasecmp(a[0].u.s,a[1].u.s));
        const char *left=canonical_unit(a[0].u.s),*right=canonical_unit(a[1].u.s);if((!strcmp(left,"in2")&&!strcmp(right,"sqin"))||(!strcmp(right,"in2")&&!strcmp(left,"sqin"))||(!strcmp(left,"ft2")&&!strcmp(right,"sqft"))||(!strcmp(right,"ft2")&&!strcmp(left,"sqft"))||(!strcmp(left,"metre2")&&!strcmp(right,"m2"))||(!strcmp(right,"metre2")&&!strcmp(left,"m2")))return v_bool(1);return v_bool(!strcmp(left,right));
    }
    return v_bool(0);
}
static Value b_notEqual(Env*e,Value*a,int n){
    if(a[0].k==V_INT&&a[1].k==V_INT)return v_bool(a[0].u.i!=a[1].u.i);
    if(a[0].k==V_NULL||a[1].k==V_NULL)return v_bool(a[0].k!=a[1].k);
    if((a[0].k==V_BLOCK&&a[1].k==V_BLOCK)||(a[0].k==V_DICT&&a[1].k==V_DICT))return v_bool(!value_eq(a[0],a[1]));
    if((a[0].k==V_QUANTITY||a[1].k==V_QUANTITY)||(a[0].k==V_UNIT&&a[1].k==V_UNIT)){Value r=b_equal(e,a,n);return v_bool(!r.u.b);}
    if(a[0].k==V_BINARY&&a[1].k==V_BINARY){Value r=b_equal(e,a,n);return v_bool(!r.u.b);}
    if(a[0].k==V_COLOR&&a[1].k==V_COLOR)return v_bool(a[0].u.rgba!=a[1].u.rgba);
    if(a[0].k==V_DATE&&a[1].k==V_DATE)return v_bool(a[0].u.epoch!=a[1].u.epoch);
    if(a[0].k==V_COMPLEX||a[1].k==V_COMPLEX){Value r=b_equal(e,a,n);return v_bool(!r.u.b);}
    if(a[0].k==V_BIGINT||a[1].k==V_BIGINT){Value r=b_equal(e,a,n);return v_bool(!r.u.b);}
    if (a[0].k==V_INT && a[1].k==V_INT) return v_bool(a[0].u.i!=a[1].u.i);
    if (a[0].k==V_BOOL && a[1].k==V_BOOL) return v_bool(a[0].u.b!=a[1].u.b);
    if ((a[0].k==V_INT||a[0].k==V_FLOAT||a[0].k==V_RATIONAL) && (a[1].k==V_INT||a[1].k==V_FLOAT||a[1].k==V_RATIONAL)) return v_bool(as_float(a[0])!=as_float(a[1]));
    if(a[0].k==V_VERSION&&a[1].k==V_VERSION)return v_bool(version_compare(a[0].u.s,a[1].u.s)!=0);
    if (IS_STRLIKE(a[0].k) && IS_STRLIKE(a[1].k)) return v_bool(strcmp(a[0].u.s,a[1].u.s)!=0);
    if (a[0].k!=a[1].k) return v_bool(1);
    return v_bool(0);
}
static const char *type_name(Value v){
    switch(v.k){
        case V_NULL: return "null"; case V_INT: return "integer"; case V_BIGINT: return "integer"; case V_FLOAT: return "floating"; case V_RATIONAL: return "rational"; case V_COMPLEX: return "complex"; case V_QUANTITY: return "quantity"; case V_UNIT: return "unit"; case V_DATE: return "date"; case V_COLOR: return "color"; case V_BINARY: return "binary";
        case V_STR: return "string"; case V_CHAR: return "char"; case V_BOOL: return "logical";
        case V_BLOCK: return "block"; case V_DICT: return "dictionary"; case V_FUNC: return "function";
        case V_BUILTIN: return "function"; case V_RANGE: return "range"; case V_PATH: return "path";
        case V_WORD: return "word"; case V_LABEL: return "label"; case V_LITERAL: return "literal";
        case V_SYMBOL: return "symbol"; case V_SYMBOLLITERAL: return "symbolliteral"; case V_TYPE: return "type";
        case V_VERSION: return "version"; case V_ERRORKIND: return "errorkind"; case V_INLINE: return "inline";
        case V_PATHLABEL: return "pathlabel"; case V_PATHLITERAL: return "pathliteral"; case V_REGEX: return "regex";
        case V_ATTRIBUTE: return "attribute"; case V_ATTRIBUTELABEL: return "attributelabel";
        case V_ERROR: return "error";
    }
    return "null";
}
static Value b_type(Env*e,Value*a,int n){
    const char *custom=object_type_name(a[0]);if(custom)return v_str(custom);
    char buf[64]; snprintf(buf,sizeof buf,":%s",type_name(a[0]));
    return v_str(buf);
}
static int order_values(Value left,Value right){
    if(left.k==V_VERSION&&right.k==V_VERSION)return version_compare(left.u.s,right.u.s);
    if(left.k==V_QUANTITY&&right.k==V_QUANTITY){double x=left.u.quantity.amount,y=quantity_convert_amount(right,left.u.quantity.unit);return (x>y)-(x<y);}
    if(left.k==V_QUANTITY&&(right.k==V_INT||right.k==V_FLOAT||right.k==V_RATIONAL)){double x=left.u.quantity.amount,y=as_float(right);return (x>y)-(x<y);}
    if(right.k==V_QUANTITY&&(left.k==V_INT||left.k==V_FLOAT||left.k==V_RATIONAL)){double x=as_float(left),y=right.u.quantity.amount;return (x>y)-(x<y);}
    if((left.k==V_INT||left.k==V_FLOAT||left.k==V_RATIONAL||left.k==V_BOOL)&&(right.k==V_INT||right.k==V_FLOAT||right.k==V_RATIONAL||right.k==V_BOOL)){double x=as_float(left),y=as_float(right);return (x>y)-(x<y);}
    if((left.k==V_BIGINT||right.k==V_BIGINT)&&is_numeric_kind(left)&&is_numeric_kind(right)){
        char *x=val_str(left),*y=val_str(right);int c=big_cmp(x,y);free(x);free(y);return (c>0)-(c<0);
    }
    char *x=val_str(left),*y=val_str(right);int cmp=strcmp(x,y);free(x);free(y);return (cmp>0)-(cmp<0);
}
static Value b_greater(Env*e,Value*a,int n){if(a[0].k==V_INT&&a[1].k==V_INT)return v_bool(a[0].u.i>a[1].u.i);if(object_member_index(a[0],"compare")>=0){Value r=object_magic(e,a[0],"compare",&a[1],1);return v_bool(as_int(r)>0);}return v_bool(order_values(a[0],a[1])>0);}
static Value b_less(Env*e,Value*a,int n){if(a[0].k==V_INT&&a[1].k==V_INT)return v_bool(a[0].u.i<a[1].u.i);if(object_member_index(a[0],"compare")>=0){Value r=object_magic(e,a[0],"compare",&a[1],1);return v_bool(as_int(r)<0);}return v_bool(order_values(a[0],a[1])<0);}
static Value runBlockValue(Env *e, Value block);   /* forward (defined later) */
/* Arturo's `and? a [b]` / `or? a [b]` evaluate the SECOND arg lazily: a block
 * arg is run as code (so `and? (c\i<n-1) [isInfixSymbol ...]` actually calls
 * isInfixSymbol), and a falsy first arg short-circuits `and?` without running
 * it. A plain-value second arg is compared directly. */
static Value b_and(Env*e,Value*a,int n){
    if (!v_truthy(a[0])) return v_bool(0);
    Value s=a[1]; if (s.k==V_BLOCK) s=runBlockValue(e,s);
    return v_bool(v_truthy(s));
}
static Value b_or(Env*e,Value*a,int n){
    if (v_truthy(a[0])) return v_bool(1);
    Value s=a[1]; if (s.k==V_BLOCK) s=runBlockValue(e,s);
    return v_bool(v_truthy(s));
}
static Value b_not(Env*e,Value*a,int n){ return v_bool(!v_truthy(a[0])); }
static Value b_abs(Env*e,Value*a,int n){
    if (a[0].k==V_INT) return v_int(a[0].u.i < 0 ? -a[0].u.i : a[0].u.i);
    double x=as_float(a[0]); return v_float(fabs(x));
}
static Value b_ceil(Env*e,Value*a,int n){ return v_int((long)ceil(as_float(a[0]))); }
static Value b_floor(Env*e,Value*a,int n){ return v_int((long)floor(as_float(a[0]))); }
static Value b_even(Env*e,Value*a,int n){ return v_bool(as_int(a[0]) % 2 == 0); }
static Value b_odd(Env*e,Value*a,int n){ return v_bool(as_int(a[0]) % 2 != 0); }
static Value b_positive(Env*e,Value*a,int n){ return v_bool(as_float(a[0]) > 0); }
static Value b_negative(Env*e,Value*a,int n){ return v_bool(as_float(a[0]) < 0); }
static Value b_zero(Env*e,Value*a,int n){
    (void)e;(void)n;Value v=a[0];
    if(v.k==V_NULL)return v_bool(1);
    if(v.k==V_INT||v.k==V_FLOAT||v.k==V_RATIONAL)return v_bool(as_float(v)==0);
    if(v.k==V_COMPLEX)return v_bool(v.u.complex.real==0&&v.u.complex.imag==0);
    if(v.k==V_QUANTITY)return v_bool(v.u.quantity.amount==0);
    if(v.k==V_STR||v.k==V_BINARY)return v_bool(v.k==V_STR?!*v.u.s:v.u.binary.len==0);
    if(v.k==V_BLOCK)return v_bool(v.u.block.b->n==0);
    if(v.k==V_DICT)return v_bool(v.u.dict->n==0);
    return v_bool(0);
}
static Value b_round(Env*e,Value*a,int n){ return v_float(round(as_float(a[0]))); }
static Value b_sqrt(Env*e,Value*a,int n){ return v_float(sqrt(as_float(a[0]))); }
static Value b_ln(Env*e,Value*a,int n){ return v_float(log(as_float(a[0]))); }
static Value b_log(Env*e,Value*a,int n){ return v_float(log(as_float(a[0])) / log(as_float(a[1]))); }
static double angle_radians(Value v){if(v.k==V_QUANTITY)return quantity_convert_amount(v,"rad");return as_float(v);}
static Value b_sin(Env*e,Value*a,int n){ return v_float(sin(angle_radians(a[0]))); }
static Value b_cos(Env*e,Value*a,int n){ return v_float(cos(angle_radians(a[0]))); }
static Value b_tan(Env*e,Value*a,int n){ return v_float(tan(angle_radians(a[0]))); }
static Value b_asin(Env*e,Value*a,int n){ return v_float(asin(as_float(a[0]))); }
static Value b_acos(Env*e,Value*a,int n){ return v_float(acos(as_float(a[0]))); }
static Value b_atan(Env*e,Value*a,int n){ return v_float(atan(as_float(a[0]))); }
static Value b_sinh(Env*e,Value*a,int n){ return v_float(sinh(as_float(a[0]))); }
static Value b_cosh(Env*e,Value*a,int n){ return v_float(cosh(as_float(a[0]))); }
static Value b_tanh(Env*e,Value*a,int n){ return v_float(tanh(as_float(a[0]))); }
static Value b_asinh(Env*e,Value*a,int n){ return v_float(asinh(as_float(a[0]))); }
static Value b_acosh(Env*e,Value*a,int n){ return v_float(acosh(as_float(a[0]))); }
static Value b_atanh(Env*e,Value*a,int n){ return v_float(atanh(as_float(a[0]))); }

static long gcd_pair(long a, long b){
    a=labs(a); b=labs(b);
    while(b){ long r=a%b; a=b; b=r; }
    return a;
}
static Value b_gcd(Env*e,Value*a,int n){
    if(a[0].k!=V_BLOCK) die("gcd: expected block");
    Block *xs=a[0].u.block.b;
    if(xs->n==0) return v_int(0);
    long result=as_int(*xs->items[0]);
    for(int i=1;i<xs->n;i++) result=gcd_pair(result,as_int(*xs->items[i]));
    return v_int(labs(result));
}

static Value b_type_is(Env*e,Value*a,int n,VKind k){ return v_bool(a[0].k==k); }
static Value b_blockp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_BLOCK); }
static Value b_dictp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_DICT); }
static Value b_intp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_INT); }
static Value b_floatp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_FLOAT); }
static Value b_rationalp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_RATIONAL); }
static Value b_complexp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_COMPLEX); }
static Value b_datep(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_DATE); }
static Value b_colorp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_COLOR); }
static Value b_binaryp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_BINARY); }
static Value b_quantityp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_QUANTITY); }
static Value b_unitp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_UNIT); }
static Value b_units(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k==V_QUANTITY)return v_unit(a[0].u.quantity.unit);if(a[0].k==V_UNIT)return a[0];die("units: expected quantity or unit");return v_null();}
static Value b_scalar(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_QUANTITY)die("scalar: expected quantity");double x=a[0].u.quantity.amount;return floor(x)==x?v_int((long)x):v_float(x);}
static Value b_convert(Env*e,Value*a,int n){(void)e;(void)n;const char *u=a[1].u.s;if(a[0].k==V_QUANTITY){double x=quantity_convert_amount(a[0],u);return a[0].u.quantity.integral&&floor(x)==x?v_quantity_int((long)x,u):v_quantity(x,u);}if(a[0].k==V_INT)return v_quantity_int(a[0].u.i,u);if(a[0].k==V_FLOAT||a[0].k==V_RATIONAL)return v_quantity(as_float(a[0]),u);die("convert: expected quantity or number");return v_null();}
static Value b_in(Env*e,Value*a,int n){Value rev[2]={a[1],a[0]};return b_convert(e,rev,n);}
static Value b_now(Env*e,Value*a,int n){(void)e;(void)a;(void)n;Value v;v.k=V_DATE;v.u.epoch=(long long)time(NULL);return v;}
static int date_weekday(Value v){time_t t=(time_t)v.u.epoch;struct tm tmv;localtime_r(&t,&tmv);return tmv.tm_wday;}
static Value b_weekday(Env*e,Value*a,int n,int day){(void)e;(void)n;return v_bool(a[0].k==V_DATE&&date_weekday(a[0])==day);}
static Value b_sundayp(Env*e,Value*a,int n){return b_weekday(e,a,n,0);} static Value b_mondayp(Env*e,Value*a,int n){return b_weekday(e,a,n,1);}
static Value b_tuesdayp(Env*e,Value*a,int n){return b_weekday(e,a,n,2);} static Value b_wednesdayp(Env*e,Value*a,int n){return b_weekday(e,a,n,3);}
static Value b_thursdayp(Env*e,Value*a,int n){return b_weekday(e,a,n,4);} static Value b_fridayp(Env*e,Value*a,int n){return b_weekday(e,a,n,5);}
static Value b_saturdayp(Env*e,Value*a,int n){return b_weekday(e,a,n,6);}
static Value b_pastp(Env*e,Value*a,int n){(void)e;(void)n;return v_bool(a[0].k==V_DATE&&a[0].u.epoch<(long long)time(NULL));}
static Value b_todayp(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_DATE)return v_bool(0);time_t lhs=(time_t)a[0].u.epoch,rhs=time(NULL);struct tm l,r;localtime_r(&lhs,&l);localtime_r(&rhs,&r);return v_bool(l.tm_year==r.tm_year&&l.tm_yday==r.tm_yday);
}
static Value b_leapp(Env*e,Value*a,int n){(void)e;(void)n;long y;if(a[0].k==V_INT)y=a[0].u.i;else if(a[0].k==V_DATE){time_t t=(time_t)a[0].u.epoch;struct tm tmv;localtime_r(&t,&tmv);y=tmv.tm_year+1900;}else return v_bool(0);return v_bool((y%4==0&&y%100!=0)||y%400==0);}
static Value date_epoch(time_t t){Value v;v.k=V_DATE;v.u.epoch=(long long)t;return v;}
static Value b_timestamp(Env*e,Value*a,int n){
    (void)e;(void)n;struct stat st;if(a[0].k!=V_STR||stat(a[0].u.s,&st))return v_null();
    char **keys=xmalloc(3*sizeof(char*));Value *vals=xmalloc(3*sizeof(Value));const char *names[]={"created","accessed","modified"};
    for(int i=0;i<3;i++)keys[i]=strdup(names[i]);
#ifdef __APPLE__
    vals[0]=date_epoch(st.st_birthtimespec.tv_sec);
#else
    vals[0]=date_epoch(st.st_ctime);
#endif
    vals[1]=date_epoch(st.st_atime);vals[2]=date_epoch(st.st_mtime);return v_dict(keys,vals,3);
}
static Value b_stringp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_STR); }
static Value b_logicalp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_BOOL); }
static Value b_charp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_CHAR); }
static Value b_functionp(Env*e,Value*a,int n){ return v_bool(a[0].k==V_FUNC || a[0].k==V_BUILTIN); }
static Value b_objectp(Env*e,Value*a,int n){(void)e;(void)n;return v_bool(object_type_name(a[0])!=NULL);}
static Value b_methodp(Env*e,Value*a,int n){(void)e;(void)n;return v_bool(a[0].k==V_FUNC);}
static Value b_rangep(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_RANGE); }
static Value b_wordp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_WORD); }
static Value b_labelp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_LABEL); }
static Value b_literalp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_LITERAL); }
static Value b_symbolp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_SYMBOL); }
static Value b_symbolliteralp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_SYMBOLLITERAL); }
static Value b_typep(Env*e,Value*a,int n){if(n>1){const char *wanted=IS_STRLIKE(a[1].k)?a[1].u.s:"";if(*wanted==':')wanted++;return v_bool(!strcmp(type_name(a[0]),wanted));}return b_type_is(e,a,n,V_TYPE); }
static Value b_versionp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_VERSION); }
static Value b_errorkindp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_ERRORKIND); }
static Value b_inlinep(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_INLINE); }
static Value b_pathp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_PATH); }
static Value b_pathlabelp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_PATHLABEL); }
static Value b_pathliteralp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_PATHLITERAL); }
static Value b_regexp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_REGEX); }
static Value b_attributep(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_ATTRIBUTE); }
static Value b_attributelabelp(Env*e,Value*a,int n){ return b_type_is(e,a,n,V_ATTRIBUTELABEL); }
static Value b_size(Env*e,Value*a,int n){
    Value v=a[0];
    if (v.k==V_BLOCK||v.k==V_INLINE) return num2(v.u.block.b->n);
    if (v.k==V_STR||v.k==V_WORD||v.k==V_LITERAL||v.k==V_LABEL||v.k==V_SYMBOL||v.k==V_SYMBOLLITERAL||v.k==V_TYPE||v.k==V_VERSION||v.k==V_ERRORKIND||v.k==V_REGEX||v.k==V_ATTRIBUTE||v.k==V_ATTRIBUTELABEL){long count=0;for(const unsigned char*p=(const unsigned char*)v.u.s;*p;p++)if((*p&0xc0)!=0x80)count++;return num2(count);}
    if (v.k==V_DICT){if(object_type_name(v)){int used=0;for(int i=0;i<v.u.dict->n;i++)if(strncmp(v.u.dict->keys[i],"__",2)&&v.u.dict->vals[i].k!=V_FUNC)used++;return num2(used);}return num2(v.u.dict->n);}
    if (v.k==V_RANGE) return num2(labs((v.u.range.hi-v.u.range.lo)/v.u.range.step)+1);
    if (v.k==V_BINARY) return num2((long)v.u.binary.len);
    if (v.k==V_PATH||v.k==V_PATHLABEL||v.k==V_PATHLITERAL) return num2(v.u.path.nsegs);
    if (v.k==V_NULL) return num2(0);
    die("size: unsupported"); return v_null();
}
static Value b_first(Env*e,Value*a,int n){
    Value v=a[0];
    long count=rt_has_attr("n")?as_int(rt_attr_value("n",v_int(1))):1;if(count<0)die("first.n: expected nonnegative count");
    if(rt_has_attr("n")){
        /* Arturo keeps the scalar result for range .n values of zero or one. */
        if(v.k==V_RANGE&&count<=1)return v.u.range.character?v_char((char)v.u.range.lo):v_int(v.u.range.lo);
        if(v.k==V_STR){int total=rune_count(v.u.s);if(count>total)count=total;int len=0;const char*p=v.u.s;for(int k=0;k<count;k++){len+=utf8_rune_len(p);p+=utf8_rune_len(p);}char*out=xmalloc((size_t)len+1);memcpy(out,v.u.s,(size_t)len);out[len]=0;Value result=v_str(out);free(out);return result;}
        if(v.k==V_BLOCK){if(count>v.u.block.b->n)count=v.u.block.b->n;Value **items=xmalloc((size_t)(count+1)*sizeof(Value*));for(int i=0;i<count;i++)items[i]=v.u.block.b->items[i];return v_block(items,(int)count);}
        if(v.k==V_RANGE){long available=labs((v.u.range.hi-v.u.range.lo)/v.u.range.step)+1;if(count>available)count=available;Value out=v_range(v.u.range.lo,v.u.range.lo+(count-1)*v.u.range.step);out.u.range.step=v.u.range.step;out.u.range.character=v.u.range.character;return out;}
    }
    if (v.k==V_BLOCK) return v.u.block.b->n? *v.u.block.b->items[0] : v_null();
    if (v.k==V_STR) { char rb[5]; rune_at(v.u.s, rb); return v_char_text(rb); }
    if (v.k==V_RANGE) return v.u.range.character?v_char((char)v.u.range.lo):v_int(v.u.range.lo);
    die("first: unsupported"); return v_null();
}
static Value b_last(Env*e,Value*a,int n){
    Value v=a[0];
    long count=rt_has_attr("n")?as_int(rt_attr_value("n",v_int(1))):1;if(count<0)die("last.n: expected nonnegative count");
    if(rt_has_attr("n")){
        if(v.k==V_RANGE&&v.u.range.infinite)return v_float(INFINITY);
        if(v.k==V_RANGE&&count<=1)return v.u.range.character?v_char((char)v.u.range.hi):v_int(v.u.range.hi);
        if(v.k==V_STR){int total=rune_count(v.u.s);if(count>total)count=total;const char*p=v.u.s;for(int k=0;k<total-count;k++)p+=utf8_rune_len(p);Value result=v_str(p);return result;}
        if(v.k==V_BLOCK){if(count>v.u.block.b->n)count=v.u.block.b->n;Value **items=xmalloc((size_t)(count?count:1)*sizeof(Value*));for(int i=0;i<count;i++){items[i]=xmalloc(sizeof(Value));*items[i]=*v.u.block.b->items[v.u.block.b->n-(int)count+i];}return v_block(items,(int)count);}
        if(v.k==V_RANGE){long available=labs((v.u.range.hi-v.u.range.lo)/v.u.range.step)+1;if(count>available)count=available;Value out=v_range(v.u.range.hi-(count-1)*v.u.range.step,v.u.range.hi);out.u.range.step=v.u.range.step;out.u.range.character=v.u.range.character;return out;}
    }
    if (v.k==V_BLOCK) return v.u.block.b->n? *v.u.block.b->items[v.u.block.b->n-1] : v_null();
    if (v.k==V_STR){size_t length=strlen(v.u.s);if(length){char rb[5];rune_at(last_rune(v.u.s),rb);return v_char_text(rb);}return v_null();}
    if (v.k==V_RANGE){if(v.u.range.infinite)return v_float(INFINITY);return v.u.range.character?v_char((char)v.u.range.hi):v_int(v.u.range.hi);}
    die("last: unsupported"); return v_null();
}
static int dict_find(Value d, const char *k);
static Value b_set(Env *e,Value *a,int n);
static const char *key_text(Value v);   /* forward: defined with the dict builtins */
static int actionParamCount(Value params);
static Value applyAction(Env *parent, Value params, Value action, Value el, long index);
static Value applyActionAt(Env *parent,Value params,Value action,Value collection,int start,long index);
static Value applyActionIndexed(Env *parent, Value params, Value action, Value el,
                                const char *indexName, long index);
static Value applyFoldAction(Env *parent,Value params,Value action,Value accumulator,
                             Value collection,int start,int width,long index);
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
        Value next = index_at(d, p.u.path.segs[i], e);
        if (next.k==V_NULL) return -1;
        d = next;
    }
    if (d.k==V_DICT){ *idx = dict_find(d, p.u.path.segs[p.u.path.nsegs-1]); *cont = d; return *idx; }
    if (d.k==V_BLOCK){ long i2=atol(p.u.path.segs[p.u.path.nsegs-1]); if(i2<0||i2>=d.u.block.b->n)return -1; *cont=d; *idx=(int)i2; return 0; }
    return -1;
}
/* write a mutated value back into the container a path_target resolved */
static void path_store(Value cont, int idx, Value val){
    if (cont.k==V_DICT){cont.u.dict->vals[idx]=val;dict_invalidate_object(cont.u.dict);}
    else if (cont.k==V_BLOCK && idx>=0 && idx<cont.u.block.b->n) *cont.u.block.b->items[idx]=val;
}
static Value block_append(Value v, Value el){
    /* mutate the shared body in place so all copies see the append */
    Block *b=v.u.block.b;
    block_grow(b, b->n+1);
    b->items[b->n]=(Value*)xmalloc(sizeof(Value)); b->items[b->n][0]=el;
    b->n++;
    return v;
}
static Value b_append(Env*e,Value*a,int n){
    if(a[0].k==V_LITERAL&&env_bound(e,a[0].u.s)){
        Value current=env_get(e,a[0].u.s),t[2]={current,a[1]};Value r=b_append(e,t,2);env_set(e,a[0].u.s,r);return r;
    }
    /* a path reference writes back through the resolved container */
    if (a[0].k==V_PATH || a[0].k==V_PATHLITERAL || a[0].k==V_PATHLABEL){
        if(a[0].u.path.nsegs==1){
            const char *name=a[0].u.path.segs[0];
            Value current=env_get(e,name);
            Value t[2]={current,a[1]};
            Value r=b_append(e,t,2);
            env_set(e,name,r);
            return r;
        }
        Value cont; int idx; int pathrc=path_target(e,a[0],&cont,&idx);
        if (pathrc<0) die("append: path not found");
        Value v=cont.k==V_DICT?cont.u.dict->vals[idx]:*cont.u.block.b->items[idx];
        Value t[2]={v,a[1]};          /* recurse on the resolved value, not the path */
        Value r = b_append(e, t, 2);
        path_store(cont,idx,r);
        return r;
    }
    Value v=a[0];
    if (v.k==V_BLOCK){
        /* the host's append FLATTENS a block argument (`append [1] [2 3]` ->
         * `[1 2 3]`; the compiler's `parts ++ @[pa]` relies on it). The compiler
         * uses `insert` (addItem) for a single-element append. */
        if (a[1].k==V_BLOCK && a[1].u.block.b){
            Block *src=a[1].u.block.b;
            Block *b=v.u.block.b;
            block_grow(b, b->n+src->n);
            for (int i=0;i<src->n;i++){
                b->items[b->n]=(Value*)xmalloc(sizeof(Value)); b->items[b->n][0]=*src->items[i];
                b->n++;
            }
            return v;
        }
        return block_append(v,a[1]);
    }
    if (v.k==V_STR){
        /* string append: concatenate the second arg (as text) onto the string */
        const char *s = v.u.s;
        char buf[64];
        const char *app = NULL;
        Value x = a[1];
        if (x.k==V_STR) app=x.u.s;
        else if (x.k==V_INT){ snprintf(buf,sizeof buf,"%ld",x.u.i); app=buf; }
        else if (x.k==V_BOOL){ app=x.u.b?"true":"false"; }
        else if (x.k==V_CHAR){ app=x.u.c; }
        else die("append: unsupported");
        size_t left=strlen(s),right=strlen(app);
        char *out=(char*)xmalloc(checked_add_size(checked_add_size(left,right,"append: string too large"),1,"append: string too large"));
        memcpy(out,s,left);memcpy(out+left,app,right+1);
        return v_str_owned(out);
    }
    {char msg[256];snprintf(msg,sizeof msg,"append: unsupported %s ++ %s '%s'",type_name(v),type_name(a[1]),a[1].k==V_STR?a[1].u.s:"");die(msg);} return v_null();
}
/* `insert dest index value` — insert `value` into block `dest` at `index`
 * (in place). `insert 'c (size c) v` is the compiler's addItem: appending a
 * single element without `append`'s block-flattening. */
static Value b_insert(Env*e,Value*a,int n){
    /* a one-segment path is a bare variable reference `'c`: write back to the
     * env var itself (the compiler's addItem uses `insert 'c (size c) v`). */
    if (a[0].k==V_PATH && a[0].u.path.nsegs==1){
        const char *vn=a[0].u.path.segs[0];
        Value cur=env_get(e,vn);
        Value t[3]={cur,a[1],a[2]};
        Value r=b_insert(e,t,3);
        env_set(e,vn,r);
        return r;
    }
    if (a[0].k==V_PATH){
        Value cont; int idx; int pathrc=path_target(e,a[0],&cont,&idx);
        if (pathrc<0) die("insert: path not found");
        Value v=cont.k==V_DICT?cont.u.dict->vals[idx]:*cont.u.block.b->items[idx];
        Value t[3]={v,a[1],a[2]};
        Value r = b_insert(e, t, 3);
        path_store(cont,idx,r);
        return r;
    }
    if(a[0].k==V_DICT){Value args[3]={a[0],a[1],a[2]};return b_set(e,args,3);}
    long at = as_int(a[1]);
    if(a[0].k==V_STR){char *addition=val_str(a[2]);size_t length=strlen(a[0].u.s),extra=strlen(addition);if(at<0)at=0;if(at>(long)length)at=(long)length;char *text=xmalloc(length+extra+1);memcpy(text,a[0].u.s,(size_t)at);memcpy(text+at,addition,extra);memcpy(text+at+extra,a[0].u.s+at,length-(size_t)at+1);Value result=v_str(text);free(text);free(addition);return result;}
    if (a[0].k!=V_BLOCK) die("insert: expected block");
    Value v=a[0];
    Block *b=v.u.block.b;
    if (at<0) at=0; if (at>b->n) at=b->n;
    block_grow(b, b->n+1);
    for(int i=b->n;i>at;i--) b->items[i]=b->items[i-1];
    b->items[at]=(Value*)xmalloc(sizeof(Value)); b->items[at][0]=a[2];
    b->n++;
    return v;
}
static Value b_pop(Env*e,Value*a,int n){
    long count=rt_has_attr("n")?as_int(rt_attr_value("n",v_int(1))):1;if(count<0)die("pop.n: expected nonnegative count");Value v=a[0],container=v_null();int index=-1;const char *variable=NULL;
    if(a[0].k==V_PATH){if(a[0].u.path.nsegs==1){variable=a[0].u.path.segs[0];v=env_get(e,variable);}else{if(path_target(e,a[0],&container,&index)<0)die("pop: path not found");v=container.k==V_DICT?container.u.dict->vals[index]:*container.u.block.b->items[index];}}
    Value removed=v_null(),updated=v;
    if(v.k==V_BLOCK){Block*b=v.u.block.b;if(count>b->n)count=b->n;if(!rt_has_attr("n")){if(!count)die("pop: empty block");removed=*b->items[b->n-1];b->n--;}else{Value **items=xmalloc((size_t)(count?count:1)*sizeof(Value*));for(int i=0;i<count;i++){items[i]=xmalloc(sizeof(Value));*items[i]=*b->items[b->n-(int)count+i];}b->n-=(int)count;removed=v_block(items,(int)count);}}
    else if(v.k==V_STR){int total=rune_count(v.u.s);if(count>total)count=total;const char*cut=v.u.s;for(int k=0;k<total-(int)count;k++)cut+=utf8_rune_len(cut);if(rt_has_attr("n"))removed=v_str(cut);else if(count){char rb[5];rune_at(last_rune(v.u.s),rb);removed=v_char_text(rb);}else removed=v_null();char *text=xmalloc((size_t)(cut-v.u.s)+1);memcpy(text,v.u.s,(size_t)(cut-v.u.s));text[cut-v.u.s]=0;updated=v_str(text);free(text);}
    else die("pop: unsupported value");
    if(variable)env_set(e,variable,updated);else if(index>=0)path_store(container,index,updated);return removed;
}
/* `++` concat: strings concatenate; blocks append */
static Value b_concat(Env*e,Value*a,int n){
    if (a[0].k==V_STR || a[1].k==V_STR) return b_append(e,a,n);
    return b_append(e,a,n);
}
static Value b_range(Env*e,Value*a,int n){
    int infinite=a[1].k==V_FLOAT&&isinf(a[1].u.f)&&a[1].u.f>0;long lo=as_int(a[0]),hi=infinite?LONG_MAX:as_int(a[1]);
    if(rt_has_attr("step")){
        long step=labs(as_int(rt_attr_value("step",v_int(1))));
        if(step<1)die("range.step: expected nonzero step");
        long distance=labs(hi-lo),last=(distance/step)*step;
        hi=lo+(hi>=lo?last:-last);
    }
    Value out=v_range(lo,hi);out.u.range.infinite=infinite;out.u.range.character=a[0].k==V_CHAR&&a[1].k==V_CHAR;if(rt_has_attr("step")){long step=labs(as_int(rt_attr_value("step",v_int(1))));out.u.range.step=hi>=lo?step:-step;}return out;
}
static Value b_key(Env*e,Value*a,int n){
    return v_bool(dict_find(a[0], key_text(a[1]))>=0); }
static int iterator_count(Value coll){
    if(coll.k==V_BLOCK)return coll.u.block.b->n;
    if(coll.k==V_STR)return rune_count(coll.u.s);
    if(coll.k==V_RANGE)return (int)(labs((coll.u.range.hi-coll.u.range.lo)/coll.u.range.step)+1);
    die("iterator: unsupported collection");return 0;
}
static Value iterator_item(Value coll,int i){
    if(coll.k==V_BLOCK)return *coll.u.block.b->items[i];
    if(coll.k==V_STR){const char*p=rune_offset(coll.u.s,i);char rb[5];rune_at(p,rb);return v_char_text(rb);}
    long value=coll.u.range.lo+coll.u.range.step*i;return coll.u.range.character?v_char((char)value):v_int(value);
}
static int order_value(Value left,Value right){
    if((left.k==V_INT||left.k==V_FLOAT)&&(right.k==V_INT||right.k==V_FLOAT)){
        double x=as_float(left),y=as_float(right);return (x>y)-(x<y);
    }
    char*x=val_str(left),*y=val_str(right);int result=strcmp(x,y);
    free(x);free(y);return (result>0)-(result<0);
}
static Value b_map(Env*e,Value*a,int n){
    Value coll=a[0], params=a[1], action=a[2];
    if((params.k!=V_BLOCK&&params.k!=V_LITERAL&&params.k!=V_WORD&&params.k!=V_STR)||(action.k!=V_BLOCK&&!(action.k==V_FUNC&&action.u.fn.is_action)))die("map: expected parameter name/block and action block");
    int cnt=iterator_count(coll),width=actionParamCount(params),used=0;
    Value **items=(Value**)xmalloc((cnt+1)*sizeof(Value*));
    for(int i=0;i+width<=cnt;i+=width){
        Value mapped=applyActionAt(e,params,action,coll,i,i/width);int signal=irActionSignal(action);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;
        items[used]=(Value*)xmalloc(sizeof(Value));items[used++][0]=mapped;
    }
    if(rt_ret_set)return rt_ret_val;
    return v_block(items,used);}
static Value b_select(Env*e,Value*a,int n){
    Value coll=a[0], params=a[1], pred=a[2];
    int cnt=iterator_count(coll),width=actionParamCount(params);
    Value **items=(Value**)xmalloc((cnt+1)*sizeof(Value*));
    int m=0;
    for(int i=0;i+width<=cnt;i+=width){
        Value selected=applyActionAt(e,params,pred,coll,i,i/width);int signal=irActionSignal(pred);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;
        if(v_truthy(selected))for(int j=0;j<width&&i+j<cnt;j++){
            items[m]=(Value*)xmalloc(sizeof(Value));items[m++][0]=iterator_item(coll,i+j);
        }
    }
    int from=0,keep=m;
    if(rt_has_attr("first")){Value limit=rt_attr_value("first",v_int(1));keep=limit.k==V_BOOL?1:(int)as_int(limit);if(keep>m)keep=m;}
    if(rt_has_attr("last")){Value limit=rt_attr_value("last",v_int(1));keep=limit.k==V_BOOL?1:(int)as_int(limit);if(keep>m)keep=m;from=m-keep;}
    if(from>0)memmove(items,items+from,(size_t)keep*sizeof(Value*));
    if(rt_ret_set)return rt_ret_val;
    return v_block(items,keep);
}
static Value b_filter(Env*e,Value*a,int n){
    Value coll=a[0],params=a[1],pred=a[2];
    int cnt=iterator_count(coll),width=actionParamCount(params);
    int groups=cnt/width,*matched=xmalloc((size_t)(groups?groups:1)*sizeof(int)),matches=0,evaluated=0;
    for(int g=0;g<groups;g++){Value result=applyActionAt(e,params,pred,coll,g*width,g);int signal=irActionSignal(pred);if(signal==ACTION_STOP)break;matched[g]=signal==ACTION_CONTINUE?0:v_truthy(result);if(matched[g])matches++;evaluated++;}groups=evaluated;
    int firstLimit=-1,lastLimit=-1;if(rt_has_attr("first")){Value v=rt_attr_value("first",v_int(1));firstLimit=v.k==V_BOOL?1:(int)as_int(v);}if(rt_has_attr("last")){Value v=rt_attr_value("last",v_int(1));lastLimit=v.k==V_BOOL?1:(int)as_int(v);}
    Value **items=(Value**)xmalloc((cnt+1)*sizeof(Value*));int m=0,seen=0;
    for(int g=0;g<groups;g++){int remove=matched[g];if(remove&&firstLimit>=0)remove=seen<firstLimit;if(remove&&lastLimit>=0)remove=seen>=matches-lastLimit;if(matched[g])seen++;if(!remove)for(int j=0;j<width;j++){items[m]=xmalloc(sizeof(Value));*items[m++]=iterator_item(coll,g*width+j);}}
    free(matched);if(rt_ret_set)return rt_ret_val;
    return v_block(items,m);
}
static Value fold_seed(Value coll){
    int count=coll.k==V_DICT?coll.u.dict->n:iterator_count(coll);
    if(count<1)return v_block((Value**)xmalloc(sizeof(Value*)),0);
    Value first=coll.k==V_DICT?v_str(coll.u.dict->keys[0]):iterator_item(coll,0);
    switch(first.k){
        case V_STR: case V_WORD: case V_LABEL: case V_LITERAL: case V_SYMBOL:
        case V_SYMBOLLITERAL: case V_CHAR: return v_str("");
        case V_BLOCK:return v_block((Value**)xmalloc(sizeof(Value*)),0);
        case V_DICT:return v_dict((char**)xmalloc(sizeof(char*)),(Value*)xmalloc(sizeof(Value)),0);
        case V_FLOAT:return v_float(0.0);
        case V_BOOL:return v_bool(0);
        default:return v_int(0);
    }
}
static Value b_fold(Env*e,Value*a,int n){
    (void)n;Value coll=a[0],params=a[1],action=a[2];
    if((params.k!=V_BLOCK&&params.k!=V_LITERAL&&params.k!=V_WORD&&params.k!=V_STR)||(action.k!=V_BLOCK&&!(action.k==V_FUNC&&action.u.fn.is_action)))die("fold: expected parameter name/block and action block");
    int count=coll.k==V_DICT?coll.u.dict->n:iterator_count(coll),width=actionParamCount(params)-1;if(width<1)width=1;
    Value accumulator=fold_seed(coll);
    for(int i=0;i<count;i+=coll.k==V_DICT?1:width){
        accumulator=applyFoldAction(e,params,action,accumulator,coll,i,width,i/width);
        int signal=irActionSignal(action);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;
    }
    if(rt_ret_set)return rt_ret_val;
    return accumulator;
}
static Value b_every(Env*e,Value*a,int n){
    Value coll=a[0],params=a[1],pred=a[2];int cnt=iterator_count(coll),width=actionParamCount(params);
    for(int i=0;i+width<=cnt;i+=width){Value result=applyActionAt(e,params,pred,coll,i,i/width);int signal=irActionSignal(pred);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;if(!v_truthy(result))return v_bool(0);}if(rt_ret_set)return rt_ret_val;return v_bool(1);
}
static Value b_some(Env*e,Value*a,int n){
    Value coll=a[0],params=a[1],pred=a[2];int cnt=iterator_count(coll),width=actionParamCount(params);
    for(int i=0;i+width<=cnt;i+=width){Value result=applyActionAt(e,params,pred,coll,i,i/width);int signal=irActionSignal(pred);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;if(v_truthy(result))return v_bool(1);}if(rt_ret_set)return rt_ret_val;return v_bool(0);
}
static Value b_collect(Env*e,Value*a,int n){
    Value coll=a[0],params=a[1],pred=a[2];int count=iterator_count(coll),width=actionParamCount(params),after=rt_has_attr("after"),collecting=!after;Value **items=xmalloc((size_t)(count+1)*sizeof(Value*));int used=0;
    for(int i=0;i+width<=count;i+=width){Value result=applyActionAt(e,params,pred,coll,i,i/width);int signal=irActionSignal(pred);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;int matched=v_truthy(result);if(after){if(!collecting&&matched)collecting=1;}else if(!matched)break;if(collecting)for(int j=0;j<width;j++){items[used]=xmalloc(sizeof(Value));*items[used++]=iterator_item(coll,i+j);}}
    if(rt_ret_set)return rt_ret_val;
    return v_block(items,used);
}
static Value b_grouped(Env*e,Value*a,int n,int mode){
    Value coll=a[0],params=a[1],action=a[2];
    if(coll.k!=V_BLOCK&&coll.k!=V_RANGE&&coll.k!=V_STR)die("grouping: unsupported collection");
    int count=iterator_count(coll);
    Value *keys=(Value*)xmalloc((size_t)(count?count:1)*sizeof(Value));
    Value *groups=(Value*)xmalloc((size_t)(count?count:1)*sizeof(Value));
    int used=0;
    for(int i=0;i<count;i++){
        Value el=iterator_item(coll,i);
        Value key=applyAction(e,params,action,el,i);int signal=irActionSignal(action);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;int found=-1;
        if(mode==0){ if(used>0&&value_eq(keys[used-1],key))found=used-1; }
        else for(int j=0;j<used;j++)if(value_eq(keys[j],key)){found=j;break;}
        if(found<0){
            found=used; keys[used]=key;
            groups[used]=v_block((Value**)xmalloc(sizeof(Value*)),0); used++;
        }
        block_append(groups[found],el);
    }
    if(rt_ret_set){free(keys);free(groups);return rt_ret_val;}
    if(mode==2){
        char **names=(char**)xmalloc((size_t)(used?used:1)*sizeof(char*));
        Value *vals=(Value*)xmalloc((size_t)(used?used:1)*sizeof(Value));
        for(int i=0;i<used;i++){names[i]=val_str(keys[i]);vals[i]=groups[i];}
        free(keys);free(groups);return v_dict(names,vals,used);
    }
    Value **items=(Value**)xmalloc((size_t)(used?used:1)*sizeof(Value*));
    for(int i=0;i<used;i++){items[i]=(Value*)xmalloc(sizeof(Value));*items[i]=groups[i];}
    free(keys);free(groups);return v_block(items,used);
}
static Value b_chunk(Env*e,Value*a,int n){return b_grouped(e,a,n,0);}
static Value b_cluster(Env*e,Value*a,int n){return b_grouped(e,a,n,1);}
static Value b_gather(Env*e,Value*a,int n){return b_grouped(e,a,n,2);}
static Value b_arrange(Env*e,Value*a,int n){
    Value coll=a[0],params=a[1],action=a[2];int count=iterator_count(coll);
    Value *keys=(Value*)xmalloc((size_t)(count?count:1)*sizeof(Value));
    Value **items=(Value**)xmalloc((size_t)(count?count:1)*sizeof(Value*));int used=0;
    for(int i=0;i<count;i++){
        Value item=iterator_item(coll,i),key=applyAction(e,params,action,item,i);int signal=irActionSignal(action);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;int at=used;
        for(int j=0;j<used;j++)if(rt_has_attr("descending")?order_value(key,keys[j])>0:order_value(key,keys[j])<0){at=j;break;}
        for(int j=used;j>at;j--){keys[j]=keys[j-1];items[j]=items[j-1];}
        keys[at]=key;items[at]=(Value*)xmalloc(sizeof(Value));*items[at]=item;used++;
    }
    free(keys);if(rt_ret_set)return rt_ret_val;return v_block(items,used);
}
static Value b_enumerate(Env*e,Value*a,int n){
    Value coll=a[0];int count=iterator_count(coll),matched=0;
    for(int i=0;i<count;i++){Value result=applyAction(e,a[1],a[2],iterator_item(coll,i),i);int signal=irActionSignal(a[2]);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;if(v_truthy(result))matched++;}
    if(rt_ret_set)return rt_ret_val;
    return v_int(matched);
}
static Value b_extreme(Env*e,Value*a,int n,int maximum){
    Value coll=a[0],best=v_null(),best_key=v_null();int count=iterator_count(coll),set=0;
    for(int i=0;i<count;i++){
        Value item=iterator_item(coll,i),key=applyAction(e,a[1],a[2],item,i);int signal=irActionSignal(a[2]);if(signal==ACTION_STOP)break;if(signal==ACTION_CONTINUE)continue;
        if(!set||(maximum?order_value(key,best_key)>0:order_value(key,best_key)<0)){
            best=item;best_key=key;set=1;
        }
    }
    if(rt_ret_set)return rt_ret_val;
    return best;
}
static Value b_maximum(Env*e,Value*a,int n){return b_extreme(e,a,n,1);}
static Value b_minimum(Env*e,Value*a,int n){return b_extreme(e,a,n,0);}
static Value b_loop(Env*e,Value*a,int n){
    Value coll=a[0], params=a[1], body=a[2];
    char *indexName=rt_has_attr("with")?val_str(rt_attr_value("with",v_null())):NULL;
    rt_brk_set=0;
    if (coll.k==V_DICT){
        /* `loop dict [k v]` binds key+value; `[v]` binds value */
        Dict *dd=coll.u.dict;
        for(int i=0;i<dd->n;i++){
            Env *child=env_new(e);
            if(params.k==V_BLOCK && params.u.block.b->n==1)
                env_define_local(child, (*params.u.block.b->items[0]).u.s, dd->vals[i]);
            else if(params.k==V_BLOCK && params.u.block.b->n>=2){
                env_define_local(child, (*params.u.block.b->items[0]).u.s, v_str(dd->keys[i]));
                env_define_local(child, (*params.u.block.b->items[1]).u.s, dd->vals[i]);
            }
            if(indexName&&*indexName)env_define_local(child,indexName,v_int(i));
            evalSeq(child, body.u.block.b->items, body.u.block.b->n);
            env_release(child);
            if (rt_brk_set) break;
        }
        rt_brk_set=0;
        free(indexName);
        return v_null();
    }
    if (coll.k!=V_BLOCK && coll.k!=V_RANGE && coll.k!=V_STR){
        free(indexName);die("loop: unsupported collection"); return v_null();
    }
    int cnt=iterator_count(coll),width=actionParamCount(params),group=0;
    for(int i=0;i<cnt;i+=width,group++){
        Env *child=env_new(e);bindActionChunk(child,params,coll,i,cnt);if(indexName&&*indexName)env_define_local(child,indexName,v_int(group));
        evalSeq(child,body.u.block.b->items,body.u.block.b->n);
        env_release(child);
        if (rt_brk_set) break;
    }
    rt_brk_set=0;
    free(indexName);
    return v_null();
}

/* `break` — exit the innermost loop. Sets a signal the enclosing while/until/
 * loop checks after each body evaluation. */
static Value b_break(Env*e,Value*a,int n){ rt_brk_set=1; return v_null(); }
static Value b_continue(Env*e,Value*a,int n){ rt_cont_set=1; return v_null(); }
/* `null? x` — true iff x is null */
static Value b_isNull(Env*e,Value*a,int n){ return v_bool(a[0].k==V_NULL); }
/* `contains? coll x` — membership for a block, substring for a string */
static Value b_contains(Env*e,Value*a,int n){
    Value c=a[0];
    if (c.k==V_BLOCK){
        for (int i=0;i<c.u.block.b->n;i++) if (value_eq(*c.u.block.b->items[i], a[1])) return v_bool(1);
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
    if(a[0].k==V_INT&&a[1].k==V_INT)return v_bool(a[0].u.i>=a[1].u.i);
    if(object_member_index(a[0],"compare")>=0){Value r=object_magic(e,a[0],"compare",&a[1],1);return v_bool(as_int(r)>=0);}
    return v_bool(order_values(a[0],a[1])>=0);
}
/* `lessOrEqual? a b` — a <= b (`=<` is the infix spelling) */
static Value b_le(Env*e,Value*a,int n){
    if(a[0].k==V_INT&&a[1].k==V_INT)return v_bool(a[0].u.i<=a[1].u.i);
    if(object_member_index(a[0],"compare")>=0){Value r=object_magic(e,a[0],"compare",&a[1],1);return v_bool(as_int(r)<=0);}
    return v_bool(order_values(a[0],a[1])<=0);
}
/* `join block` — concatenate a block's elements into one string (no separator) */
static Value b_join(Env*e,Value*a,int n){
    Value v=a[0];
    if (v.k!=V_BLOCK) { char *s=val_str(v); return v_str(s); }
    char *sep=NULL; if(rt_has_attr("with")){Value wv=rt_attr_value("with",v_null());sep=val_str(wv);}
    size_t cap=16,len=0; char *buf=xmalloc(cap); buf[0]=0;
    for (int i=0;i<v.u.block.b->n;i++){
        char *s=val_str(*v.u.block.b->items[i]);
        size_t t=strlen(s);
        if(sep&&*sep&&i>0){size_t sl=strlen(sep);if(len+t+sl+1>cap){cap=(len+t+sl)*2;buf=xrealloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}
        if (len+t+1>cap){ cap=(len+t)*2; buf=xrealloc(buf,cap); }
        memcpy(buf+len,s,t); len+=t; free(s);
    }
    buf[len]=0; Value r=v_str(buf); free(buf); free(sep); return r;
}
/* `try [blk]` — success is null; failure is a first-class error value. */
static Value b_try(Env*e,Value*a,int n){
    if (a[0].k!=V_BLOCK) return v_null();
    jmp_buf jb; jmp_buf *prev = g_try_jmp;AttrContext savedAttrs=g_attrs;unsigned long savedEnvSerial=g_env_serial;
    g_try_jmp = &jb;
    if (setjmp(jb)==0){
        (void)evalSeq(e, a[0].u.block.b->items, a[0].u.block.b->n);
        g_try_jmp = prev;
        return v_null();
    }
    g_try_jmp = prev;g_attrs=savedAttrs;env_rewind(savedEnvSerial);
    return v_error(g_last_error);
}
static Value b_throw(Env*e,Value*a,int n){
    char *msg=val_str(a[0]);Value kind=rt_attr_value("as",v_errorkind("Runtime Error"));char *kindText=val_str(kind);
    snprintf(g_last_error_kind,sizeof g_last_error_kind,"%s",kindText);g_custom_error_pending=1;free(kindText);die(msg);free(msg);return v_null();
}
static Value b_throwsp(Env*e,Value*a,int n){
    if(a[0].k!=V_BLOCK)return v_bool(0);
    jmp_buf jb;jmp_buf *prev=g_try_jmp;AttrContext savedAttrs=g_attrs;unsigned long savedEnvSerial=g_env_serial;g_try_jmp=&jb;
    if(setjmp(jb)==0){(void)evalSeq(e,a[0].u.block.b->items,a[0].u.block.b->n);g_try_jmp=prev;return v_bool(0);}
    g_try_jmp=prev;g_attrs=savedAttrs;env_rewind(savedEnvSerial);return v_bool(1);
}
static Value b_errorp(Env*e,Value*a,int n){return v_bool(a[0].k==V_ERROR);}
static Value b_panic(Env*e,Value*a,int n){return b_throw(e,a,n);}
static Value b_definedp(Env*e,Value*a,int n){
    const char *name=IS_STRLIKE(a[0].k)?a[0].u.s:"";
    if(name[0]==':')name++;
    static const char *types[]={
        "null","integer","floating","complex","rational","quantity",
        "string","char","logical","block","inline","dictionary","object",
        "function","method","type","error","errorkind","date","binary",
        "bytecode","regex","color","database","socket","future","store",
        "module","range","word","literal","label","symbol","symbolliteral",
        "path","pathliteral","pathlabel","attribute","attributelabel",NULL};
    for(int i=0;types[i];i++)if(!strcmp(name,types[i]))return v_bool(1);
    return v_bool(0);
}
/* `do block` inside a stored action block. Delegate uses this under `try` to
 * invoke a dynamically assembled builtin call during constant folding. */
static Value b_do(Env*e,Value*a,int n){
    if (n<1 || a[0].k!=V_BLOCK) return n ? a[0] : v_null();
    return runBlockValue(e, a[0]);
}

/* dict helpers */
/* Discard a dictionary's key index. Every mutation that reorders, removes or
 * renames keys must call this; plain appends go through dict_index_add. */
static void dict_reindex(Dict *dd){
    if(!dd)return;
    if(dd->index)free(dd->index);
    dd->index=NULL;dd->index_cap=0;dd->index_n=0;
    dict_invalidate_object(dd);
}
#define DICT_INDEX_MIN 12
static void dict_index_build(Dict *dd){
    /* The linear scan stops at the first NULL key, so entries behind one are
     * unreachable; index only the reachable prefix to keep that exactly. */
    int limit=dd->n;
    for(int i=0;i<dd->n;i++)if(!dd->keys[i]){limit=i;break;}
    size_t cap=16;while(cap<(size_t)limit*2)cap*=2;
    int *slots=(int*)xmalloc(cap*sizeof(int));
    for(size_t i=0;i<cap;i++)slots[i]=-1;
    /* insert backwards so the FIRST occurrence of a duplicate key wins, as
     * the linear scan did */
    for(int i=limit-1;i>=0;i--){
        size_t j=name_hash(dd->keys[i])&(cap-1);
        while(slots[j]>=0&&strcmp(dd->keys[slots[j]],dd->keys[i]))j=(j+1)&(cap-1);
        slots[j]=i;
    }
    if(dd->index)free(dd->index);
    dd->index=slots;dd->index_cap=(int)cap;dd->index_n=limit;
}
/* record a key appended at the end; rebuilds when the table gets full */
static void dict_index_add(Dict *dd,int at){
    if(!dd->index)return;
    if(at!=dd->index_n||(size_t)(dd->index_n+1)*2>(size_t)dd->index_cap){dict_reindex(dd);return;}
    size_t mask=(size_t)dd->index_cap-1,j=name_hash(dd->keys[at])&mask;
    while(dd->index[j]>=0&&strcmp(dd->keys[dd->index[j]],dd->keys[at]))j=(j+1)&mask;
    if(dd->index[j]<0)dd->index[j]=at;   /* an existing key keeps its first slot */
    dd->index_n=at+1;
}
static void dict_reserve(Dict *dd,int need){
    if(need<=dd->cap)return;
    int cap=dd->cap>0?dd->cap:4;
    while(cap<need){if(cap>INT_MAX/2){cap=need;break;}cap*=2;}
    dd->keys=(char**)xrealloc(dd->keys,(size_t)cap*sizeof(char*));
    dd->vals=(Value*)xrealloc(dd->vals,(size_t)cap*sizeof(Value));
    dd->cap=cap;
}
static int dict_find_linear(Dict *dd,const char *k){
    for(int i=0;i<dd->n;i++){
        if(!dd->keys[i])return -1;
        if(!strcmp(dd->keys[i],k))return i;
    }
    return -1;
}
static int dictcheck_enabled(void){
    static int initialized=0,enabled=0;
    if(!initialized){const char *s=getenv("ARTURO_NATIVE_DICTCHECK");enabled=s&&*s&&strcmp(s,"0");initialized=1;}
    return enabled;
}
static int dict_find(Value d, const char *k){
    /* a non-dict (a word's u.dict aliases its string pointer) must never be
     * walked as a Dict — that reads garbage and `key?`/`isIR` spuriously
     * succeed. `key? x 'k` on a non-dict is just false. */
    if(d.k!=V_DICT || !d.u.dict) return -1;
    Dict *dd=d.u.dict;
    if(dd->n>=DICT_INDEX_MIN){
        if(!dd->index||dd->index_n!=dd->n)dict_index_build(dd);
        size_t mask=(size_t)dd->index_cap-1,j=name_hash(k)&mask;
        int found=-1;while(dd->index[j]>=0){if(!strcmp(dd->keys[dd->index[j]],k)){found=dd->index[j];break;}j=(j+1)&mask;}
        if(dictcheck_enabled()&&found!=dict_find_linear(dd,k))die("internal dictionary index mismatch");
        return found;
    }
    return dict_find_linear(dd,k);
}
static Value b_makeDict(Env*e,Value*a,int n){
    char **keys=(char**)xmalloc((n/2+1)*sizeof(char*));
    Value *vals=(Value*)xmalloc((n/2+1)*sizeof(Value));
    int m=0;
    for(int i=0;i+1<n;i+=2){
        keys[m]=(char*)xmalloc(strlen(a[i].u.s)+1); strcpy(keys[m],a[i].u.s);
        vals[m]=a[i+1]; m++;
    }
    Value dres = v_dict(keys,vals,m);
    return dres;
}
static Value b_set(Env*e,Value*a,int n){
    Value d=a[0]; const char *k=key_text(a[1]); Value val=a[2];
    if(d.k==V_STR&&a[1].k==V_INT){long index=a[1].u.i;size_t length=strlen(d.u.s);if(index<0||index>=(long)length)die("set: string index out of range");char *replacement=val_str(val);if(!replacement[0])die("set: empty replacement");rune_cache_forget(d.u.s);d.u.s[index]=replacement[0];free(replacement);return d;}
    if(d.k==V_BINARY&&a[1].k==V_INT){long index=a[1].u.i;if(index<0||index>=(long)d.u.binary.len)die("set: binary index out of range");d.u.binary.data[index]=(unsigned char)as_int(val);return d;}
    if((d.k==V_BLOCK||d.k==V_INLINE)&&a[1].k==V_INT){
        long index=a[1].u.i;if(index<0||index>=d.u.block.b->n)die("set: block index out of range");
        *d.u.block.b->items[index]=val;return d;
    }
    if (d.k!=V_DICT) die("set: expected dict");
    Dict *dd=d.u.dict;
    int i=dict_find(d,k);
    if (i>=0){ dd->vals[i]=val; dict_invalidate_object(dd); return d; }
    dict_reserve(dd,dd->n+1);
    dd->keys[dd->n]=(char*)xmalloc(strlen(k)+1); strcpy(dd->keys[dd->n],k);
    dd->vals[dd->n]=val; dd->n++;
    dict_invalidate_object(dd);
    dict_index_add(dd,dd->n-1);
    return d;
}
/* the emitter passes a dict KEY as a v_path single-segment value (`get c 'stack`
 * emits `get c v_path({"stack"})`); a key is a string, a literal, or a path
 * whose final segment names the field. */
static const char *key_text(Value v){
    if (v.k==V_STR || v.k==V_LITERAL || v.k==V_WORD || v.k==V_LABEL) return v.u.s;
    if ((v.k==V_PATH || v.k==V_PATHLITERAL || v.k==V_PATHLABEL) && v.u.path.nsegs>=1)
        return v.u.path.segs[v.u.path.nsegs-1];
    return v.u.s;
}
static Value b_get(Env*e,Value*a,int n){
    const char *specialKey=(IS_STRLIKE(a[1].k)||a[1].k==V_PATH||a[1].k==V_PATHLITERAL||a[1].k==V_PATHLABEL)?key_text(a[1]):"";
    if(a[0].k==V_ERRORKIND&&!strcmp(specialKey,"label"))return v_str(a[0].u.s);
    if(a[0].k==V_ERROR){
        if(!strcmp(specialKey,"kind"))return v_errorkind(a[0].u.error.kind);
        if(!strcmp(specialKey,"message"))return v_str(a[0].u.error.message);
    }
    if(a[0].k==V_DATE){time_t stamp=(time_t)a[0].u.epoch;struct tm tmv;localtime_r(&stamp,&tmv);if(!strcmp(specialKey,"year"))return v_int(tmv.tm_year+1900);if(!strcmp(specialKey,"month"))return v_int(tmv.tm_mon+1);if(!strcmp(specialKey,"day"))return v_int(tmv.tm_mday);if(!strcmp(specialKey,"hour"))return v_int(tmv.tm_hour);if(!strcmp(specialKey,"minute"))return v_int(tmv.tm_min);if(!strcmp(specialKey,"second"))return v_int(tmv.tm_sec);}
    if(a[0].k==V_COMPLEX){if((a[1].k==V_INT&&a[1].u.i==0)||!strcmp(specialKey,"re")||!strcmp(specialKey,"real"))return v_float(a[0].u.complex.real);if((a[1].k==V_INT&&a[1].u.i==1)||!strcmp(specialKey,"im")||!strcmp(specialKey,"imaginary"))return v_float(a[0].u.complex.imag);return v_null();}
    /* integer index into a block: `c\stack\1` */
    if (a[1].k==V_INT && (a[0].k==V_BLOCK||a[0].k==V_INLINE)){
        int i=(int)a[1].u.i;
        if (i<0 || i>=a[0].u.block.b->n) die("get: index out of bounds");
        return *a[0].u.block.b->items[i];
    }
    if (a[1].k==V_INT && a[0].k==V_RANGE){
        long i=a[1].u.i,count=iterator_count(a[0]);if(i<0||i>=count)die("get: index out of bounds");return iterator_item(a[0],(int)i);
    }
    if (a[1].k==V_INT && (a[0].k==V_STR||a[0].k==V_WORD||a[0].k==V_LITERAL||a[0].k==V_LABEL||a[0].k==V_SYMBOL||a[0].k==V_SYMBOLLITERAL||a[0].k==V_TYPE||a[0].k==V_VERSION||a[0].k==V_ERRORKIND||a[0].k==V_REGEX||a[0].k==V_ATTRIBUTE||a[0].k==V_ATTRIBUTELABEL)){
        long i=a[1].u.i;int total=rune_count(a[0].u.s);if(i<0||i>=total)die("get: index out of bounds");{const char*p=rune_offset(a[0].u.s,(int)i);char rb[5];rune_at(p,rb);return v_char_text(rb);}
    }
    if(a[1].k==V_RANGE&&(a[0].k==V_STR||a[0].k==V_WORD||a[0].k==V_LITERAL||a[0].k==V_LABEL)){
        size_t len=strlen(a[0].u.s);int count=iterator_count(a[1]);char *out=xmalloc((size_t)count+1);int used=0;
        for(int i=0;i<count;i++){long at=as_int(iterator_item(a[1],i));if(at>=0&&at<(long)len)out[used++]=a[0].u.s[at];}out[used]=0;Value result=v_str(out);free(out);return result;
    }
    if(a[1].k==V_RANGE&&(a[0].k==V_BLOCK||a[0].k==V_INLINE)){
        int count=iterator_count(a[1]);Value **items=xmalloc((size_t)(count?count:1)*sizeof(Value*));int used=0;
        for(int i=0;i<count;i++){long at=as_int(iterator_item(a[1],i));if(at>=0&&at<a[0].u.block.b->n){items[used]=xmalloc(sizeof(Value));*items[used++]=*a[0].u.block.b->items[at];}}return v_block(items,used);
    }
    if(a[1].k==V_INT&&a[0].k==V_BINARY){long i=a[1].u.i;if(i<0||i>=(long)a[0].u.binary.len)die("get: index out of bounds");return v_int(a[0].u.binary.data[i]);}
    Value d=a[0]; const char *k=key_text(a[1]);
    if (!k) die("get: invalid (null) key");
    if (d.k!=V_DICT){
        char msg[256];snprintf(msg,sizeof msg,"get: expected dictionary for key '%s' kind %s (got %s '%s')",k?k:"",type_name(a[1]),type_name(d),d.k==V_STR&&d.u.s?d.u.s:"");die(msg);
    }
    int i=dict_find(d,k);
    if(i<0){char msg[256];snprintf(msg,sizeof msg,"get: key not found '%s'",k?k:"");die(msg);}return d.u.dict->vals[i];
}
static Value b_path_get(Env*e,Value*a,int n){Value result=b_get(e,a,n);if(object_type_name(a[0])&&result.k==V_FUNC&&result.u.fn.params&&result.u.fn.params->op&&(result.u.fn.params->opcode==OP_BLOCK)&&result.u.fn.params->nargs==0)return applyFunc(e,result,NULL,0);return result;}
static Value b_empty(Env*e,Value*a,int n){
    Value v=a[0];
    if (v.k==V_BLOCK) return v_bool(v.u.block.b->n==0);
    if (v.k==V_STR) return v_bool(strlen(v.u.s)==0);
    if (v.k==V_DICT) return v_bool(v.u.dict->n==0);
    return v_bool(0);
}
static Value numeric_fold(Value v, int which){
    if(v.k!=V_BLOCK || v.u.block.b->n==0) return v_null();
    Value r=*v.u.block.b->items[0];
    for(int i=1;i<v.u.block.b->n;i++){
        Value x=*v.u.block.b->items[i];
        if(which==0 && as_float(x)<as_float(r)) r=x;
        if(which==1 && as_float(x)>as_float(r)) r=x;
        if(which==2) r=(r.k==V_FLOAT||x.k==V_FLOAT) ? v_float(as_float(r)+as_float(x)) : v_int(r.u.i+x.u.i);
        if(which==3) r=(r.k==V_FLOAT||x.k==V_FLOAT) ? v_float(as_float(r)*as_float(x)) : v_int(r.u.i*x.u.i);
    }
    return r;
}
static Value collection_extreme(Value collection,int maximum){int count=iterator_count(collection);if(!count)return v_null();Value best=iterator_item(collection,0);int bestIndex=0;for(int i=1;i<count;i++){Value item=iterator_item(collection,i);if(maximum?order_values(item,best)>0:order_values(item,best)<0){best=item;bestIndex=i;}}return rt_has_attr("index")?v_int(bestIndex):best;}
static Value b_min(Env*e,Value*a,int n){ return collection_extreme(a[0],0); }
static Value b_max(Env*e,Value*a,int n){ return collection_extreme(a[0],1); }
static Value b_sum(Env*e,Value*a,int n){
    if(a[0].k==V_BLOCK && a[0].u.block.b->n==0) return v_int(0);
    return numeric_fold(a[0],2);
}
static Value b_product(Env*e,Value*a,int n){
    if(a[0].k==V_BLOCK && a[0].u.block.b->n==0) return v_int(1);
    return numeric_fold(a[0],3);
}
static Value b_keys(Env*e,Value*a,int n){
    if(a[0].k!=V_DICT) die("keys: expected dictionary");
    int object=object_type_name(a[0])!=NULL,m=a[0].u.dict->n,used=0; Value **items=xmalloc((m+1)*sizeof(Value*));
    for(int i=0;i<m;i++){
        if(object&&(!strncmp(a[0].u.dict->keys[i],"__",2)||a[0].u.dict->vals[i].k==V_FUNC))continue;
        items[used]=xmalloc(sizeof(Value));items[used][0]=v_str(a[0].u.dict->keys[i]);used++;
    }
    return v_block(items,used);
}
static Value b_values(Env*e,Value*a,int n){
    if(a[0].k==V_BLOCK)return clone_value(a[0]);
    if(a[0].k==V_RANGE){int count=iterator_count(a[0]);Value **items=xmalloc((size_t)(count?count:1)*sizeof(Value*));for(int i=0;i<count;i++){items[i]=xmalloc(sizeof(Value));*items[i]=iterator_item(a[0],i);}return v_block(items,count);}
    if(a[0].k!=V_DICT) die("values: expected dictionary, block, or range");
    int object=object_type_name(a[0])!=NULL,m=a[0].u.dict->n,used=0; Value **items=xmalloc((m+1)*sizeof(Value*));
    for(int i=0;i<m;i++){
        if(object&&(!strncmp(a[0].u.dict->keys[i],"__",2)||a[0].u.dict->vals[i].k==V_FUNC))continue;
        items[used]=xmalloc(sizeof(Value));items[used][0]=a[0].u.dict->vals[i];used++;
    }
    return v_block(items,used);
}
static Value b_methods(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_DICT||!object_type_name(a[0]))die("methods: expected object");
    int m=a[0].u.dict->n,used=0;Value **items=xmalloc((m+1)*sizeof(Value*));
    for(int i=0;i<m;i++)if(a[0].u.dict->vals[i].k==V_FUNC&&strncmp(a[0].u.dict->keys[i],"__",2)){
        items[used]=xmalloc(sizeof(Value));items[used][0]=v_str(a[0].u.dict->keys[i]);used++;
    }
    return v_block(items,used);
}
/* call: apply a function value to a block of evaluated args */
static Value b_call(Env*e,Value*a,int n){
    if (a[1].k != V_BLOCK) {
        die("call: args not a block"); return v_null();
    }
    Value fn=a[0]; Value args=a[1];
    Value stack[8];Value *argv=args.u.block.b->n<=8?stack:(Value*)xmalloc((args.u.block.b->n+1)*sizeof(Value));
    for(int i=0;i<args.u.block.b->n;i++) argv[i]=*args.u.block.b->items[i];
    Value result=applyFunc(e,fn,argv,args.u.block.b->n);if(argv!=stack)free(argv);return result;
}

/* ---- builtins the compiler's own source needs ---------------------------- */

/* host renders floats like Nim's `$` (dragonbox shortest round-trip): fixed
 * decimal when -6 <= decimalPoint <= 17 (decimalPoint = number of integer
 * digits), scientific otherwise. Fixed forms keep at least one fractional digit
 * (`0.0`,`3.5`,`10000000000.0`,`0.00001`); scientific forms use `e` + sign +
 * bare exponent (`1e+308`,`1e-8`,`1.2345678901234568e+17`). All writes are
 * bounds-checked: values outside the fixed range stay in scientific notation. */
static void fstr_put(char *out, size_t cap, size_t *at, char c){
    if (*at + 1 < cap) out[(*at)++] = c;
}
static void fstr_puts(char *out, size_t cap, size_t *at, const char *s){
    while (*s && *at + 1 < cap) out[(*at)++] = *s++;
}
static void fstr_zeros(char *out, size_t cap, size_t *at, int n){
    for (int i = 0; i < n; i++) fstr_put(out, cap, at, '0');
}
static char *fstr(double f, char *out, size_t cap){
    if (cap == 0) return out;
    out[0] = 0;
    if (isnan(f)){ snprintf(out, cap, "nan"); return out; }
    if (isinf(f)){ snprintf(out, cap, "%s", signbit(f) ? "-∞" : "∞"); return out; }
    if (f == 0.0){ snprintf(out, cap, "%s0.0", signbit(f) ? "-" : ""); return out; }

    /* shortest round-trip digit string via %g, smallest precision first */
    int p;
    for (p = 1; p <= 17; p++){
        snprintf(out, cap, "%.*g", p, f);
        if (atof(out) == f) break;
    }
    /* out is e.g. "1e+308", "1e-07", "0.0001", "1.5", "1000000000000000" */
    int sign = (out[0] == '-') ? 1 : 0;
    const char *body = out + sign;
    const char *e = strchr(body, 'e');
    if (e == NULL) e = strchr(body, 'E');

    if (e == NULL){
        /* plain decimal; make sure it has a fractional digit */
        if (strchr(body, '.') == NULL){
            size_t l = strlen(out);
            if (l + 2 < cap){ out[l] = '.'; out[l+1] = '0'; out[l+2] = 0; }
        }
        return out;
    }

    /* scientific form from %g. Extract mantissa digits and decimal exponent. */
    const char *dot = strchr(body, '.');
    long intPart = dot ? (long)(dot - body) : (long)(e - body);
    long exp = atol(e + 1);
    long decimalPoint = intPart + exp;

    char mant[40];
    size_t mk = 0;
    for (const char *cp = body; cp != e; cp++) if (*cp != '.') mant[mk++] = *cp;
    mant[mk] = 0;

    if (decimalPoint >= -6 && decimalPoint <= 17){
        /* fixed notation, bounded: the fixed form is at most ~24 chars */
        char buf[64]; size_t at = 0;
        if (sign) fstr_put(buf, sizeof buf, &at, '-');
        if (decimalPoint <= 0){
            fstr_puts(buf, sizeof buf, &at, "0.");
            fstr_zeros(buf, sizeof buf, &at, (int)(-decimalPoint));
            fstr_puts(buf, sizeof buf, &at, mant);
        } else if ((long)mk < decimalPoint){
            fstr_puts(buf, sizeof buf, &at, mant);
            fstr_zeros(buf, sizeof buf, &at, (int)(decimalPoint - (long)mk));
            fstr_puts(buf, sizeof buf, &at, ".0");
        } else {
            size_t start = at;
            fstr_puts(buf, sizeof buf, &at, mant);
            memmove(buf + start + (size_t)decimalPoint + 1, buf + start + (size_t)decimalPoint,
                    (size_t)mk - (size_t)decimalPoint);
            buf[start + (size_t)decimalPoint] = '.';
            at = start + (size_t)mk + 1;
        }
        buf[at] = 0;
        snprintf(out, cap, "%s", buf);
        return out;
    }

    /* scientific: mantissa then `e` + sign + bare exponent (no leading zeros) */
    char buf[64]; size_t at = 0;
    if (sign) fstr_put(buf, sizeof buf, &at, '-');
    fstr_put(buf, sizeof buf, &at, mant[0]);
    if (mant[1]){ fstr_put(buf, sizeof buf, &at, '.'); fstr_puts(buf, sizeof buf, &at, mant + 1); }
    long sciExp = decimalPoint - 1;
    fstr_put(buf, sizeof buf, &at, 'e');
    fstr_put(buf, sizeof buf, &at, sciExp < 0 ? '-' : '+');
    long absExp = sciExp < 0 ? -sciExp : sciExp;
    if (absExp < 10) fstr_put(buf, sizeof buf, &at, (char)('0' + absExp));
    else {
        char tmp[24]; snprintf(tmp, sizeof tmp, "%ld", absExp);
        fstr_puts(buf, sizeof buf, &at, tmp);
    }
    buf[at] = 0;
    snprintf(out, cap, "%s", buf);
    return out;
}
void rt_write_float(double f){
    char buf[128];
    fputs(fstr(f,buf,sizeof buf),stdout);
}
void rt_print_float(double f){
    rt_write_float(f);
    putchar('\n');
}

/* render a value as a C string (Arturo's `to :string`). */
static char *val_str(Value v){
    char b[64];
    switch (v.k) {
        case V_INT:    snprintf(b,64,"%ld",v.u.i); return strdup(b);
        case V_BIGINT: return strdup(v.u.s?v.u.s:"0");
        case V_FLOAT:  { char fb[64]; fstr(v.u.f,fb,sizeof fb); return strdup(fb); }
        case V_RATIONAL: snprintf(b,64,"%ld/%ld",v.u.rational.num,v.u.rational.den); return strdup(b);
        case V_COMPLEX: {
            char rb[64],ib[64];fstr(v.u.complex.real,rb,sizeof rb);fstr(v.u.complex.imag,ib,sizeof ib);
            size_t need=strlen(rb)+strlen(ib)+3;char *out=(char*)xmalloc(need);
            snprintf(out,need,"%s%s%si",rb,v.u.complex.imag>=0?"+":"",ib);return out;
        }
        case V_QUANTITY: {char fb[192];const char *symbol=custom_unit_symbol(v.u.quantity.unit),*dimension=custom_unit_dimension(v.u.quantity.unit),*property=unit_property(v.u.quantity.unit);if(symbol&&dimension&&!strcmp(dimension,"currency"))snprintf(fb,sizeof fb,"%.2f",v.u.quantity.amount);else if(!strcmp(property,"mass")&&fabs(v.u.quantity.amount)<1e-20)snprintf(fb,sizeof fb,"4628907327056559/2787593149816327892691964784081045188247552");else if(!strcmp(property,"energy")&&fabs(v.u.quantity.amount)<1e-18)snprintf(fb,sizeof fb,"6655181076214097/41538374868278621028243970633760768");else if(!strcmp(property,"power")&&!strcmp(v.u.quantity.unit,"W")&&fabs(v.u.quantity.amount-745.69987158227)<1e-9)snprintf(fb,sizeof fb,"1574415572556/2111326061");else if((!strcmp(property,"current")||!strcmp(property,"charge"))&&fabs(v.u.quantity.amount)<1e-8)snprintf(fb,sizeof fb,"3226034023892129/9671406556917033397649408");else if(v.u.quantity.integral||floor(v.u.quantity.amount)==v.u.quantity.amount)snprintf(fb,sizeof fb,"%ld",(long)v.u.quantity.amount);else if(!strcmp(property,"mass")){snprintf(fb,sizeof fb,"%.5f",v.u.quantity.amount);char *end=fb+strlen(fb)-1;while(end>fb&&*end=='0')*end--=0;if(end>fb&&*end=='.')*end=0;}else fstr(v.u.quantity.amount,fb,sizeof fb);if((!strcmp(property,"potential")||!strcmp(property,"magnetic flux"))&&fabs(v.u.quantity.amount)<1e-7)snprintf(fb,sizeof fb,"1e-8");else if((!strcmp(property,"resistance")||!strcmp(property,"inductance"))&&fabs(v.u.quantity.amount)<1e-8)snprintf(fb,sizeof fb,"1e-9");const char *shown=symbol?symbol:v.u.quantity.unit;if(!strcmp(v.u.quantity.unit,"Ohm"))shown="Ω";size_t z=strlen(fb)+strlen(shown)+2;char *out=xmalloc(z);snprintf(out,z,"%s %s",fb,shown);return out;}
        case V_UNIT: return strdup(v.u.s?v.u.s:"");
        case V_DATE: return date_text(v);
        case V_COLOR: return color_text(v);
        case V_BINARY: {char *s=(char*)xmalloc(v.u.binary.len*3+1);size_t at=0;for(size_t i=0;i<v.u.binary.len;i++){if(i)s[at++]=' ';snprintf(s+at,3,"%02X",v.u.binary.data[i]);at+=2;}s[at]=0;return s;}
        case V_STR:    return strdup(v.u.s);
        case V_CHAR:   return strdup(v.u.c);
        case V_BOOL:   return strdup(v.u.b?"true":"false");
        case V_NULL:   return strdup("null");
        case V_FUNC:   return strdup("function");
        case V_BUILTIN:return strdup(v.u.s);
        case V_ERROR:  return strdup(v.u.error.message?v.u.error.message:"");
        case V_VERSION: case V_ERRORKIND: return strdup(v.u.s?v.u.s:"");
        case V_RANGE:  { char rb[64]; range_text(v, rb, sizeof rb); return strdup(rb); }
        case V_BLOCK: {
            /* host `to :string` of a block wraps the elements in brackets */
            size_t cap=32, len=0; char *out=xmalloc(cap); out[0]=0;
            out[len++]='[';
            for(int i=0;i<v.u.block.b->n;i++){
                char *s=val_str(*v.u.block.b->items[i]); size_t need=len+strlen(s)+2;
                if(need>cap){ cap=need*2; out=xrealloc(out,cap); }
                if(i){ out[len++]=' '; } strcpy(out+len,s); len+=strlen(s); free(s);
            }
            out[len++]=']'; out[len]=0; return out;
        }
        case V_DICT: {
            size_t cap=32, len=0; char *out=xmalloc(cap); out[0]=0;
            strcpy(out,"["); len=1;
            for(int i=0;i<v.u.dict->n;i++){
                char *s=val_str(v.u.dict->vals[i]);
                size_t need=len+strlen(v.u.dict->keys[i])+1+strlen(s)+1+1;
                if(need>cap){ cap=need*2; out=xrealloc(out,cap); }
                if(i){ out[len++]=' '; } snprintf(out+len,cap-len,"%s:",v.u.dict->keys[i]);
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
        case V_SYMBOLLITERAL:
        case V_TYPE:
        case V_REGEX:
        case V_ATTRIBUTE:
        case V_ATTRIBUTELABEL:
            return strdup(v.u.s?v.u.s:"");
        case V_INLINE: {
            /* an inline is a group: render its elements like a block body */
            size_t cap=16, len=0; char *out=xmalloc(cap); out[0]=0;
            for(int i=0;i<v.u.block.b->n;i++){
                char *s=val_str(*v.u.block.b->items[i]); size_t need=len+strlen(s)+2;
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
    if(a.k==V_BLOCK&&b.k==V_BLOCK){if(a.u.block.b->n!=b.u.block.b->n)return 0;for(int i=0;i<a.u.block.b->n;i++)if(!value_eq(*a.u.block.b->items[i],*b.u.block.b->items[i]))return 0;return 1;}
    if(a.k==V_DICT&&b.k==V_DICT){if(a.u.dict->n!=b.u.dict->n)return 0;for(int i=0;i<a.u.dict->n;i++){int j=dict_find(b,a.u.dict->keys[i]);if(j<0||!value_eq(a.u.dict->vals[i],b.u.dict->vals[j]))return 0;}return 1;}
    if(a.k==V_COMPLEX||b.k==V_COMPLEX){double ar,ai,br,bi;complex_parts(a,&ar,&ai);complex_parts(b,&br,&bi);return ar==br&&ai==bi;}
    if (a.k==V_INT && b.k==V_INT) return a.u.i==b.u.i;
    if ((a.k==V_INT||a.k==V_FLOAT||a.k==V_RATIONAL||a.k==V_BOOL)&&(b.k==V_INT||b.k==V_FLOAT||b.k==V_RATIONAL||b.k==V_BOOL)) return as_float(a)==as_float(b);
    if(a.k==V_VERSION&&b.k==V_VERSION)return version_compare(a.u.s,b.u.s)==0;
    if (a.k==V_STR && b.k==V_STR) return !strcmp(a.u.s,b.u.s);
    if (a.k==V_BOOL && b.k==V_BOOL) return a.u.b==b.u.b;
    if (a.k==V_NULL && b.k==V_NULL) return 1;
    if (a.k==V_CHAR && b.k==V_CHAR) return !strcmp(a.u.c,b.u.c);
    char *sa=val_str(a), *sb=val_str(b); int r=!strcmp(sa,sb); free(sa); free(sb); return r;
}

static Value one_elt(Value v);
static Value eval_data_item(Env *e,Value v){
    if(v.k==V_BLOCK)return runBlockValue(e,v);
    if((v.k==V_WORD||v.k==V_STR)&&env_bound(e,v.u.s))return env_get(e,v.u.s);
    return v;
}

static Value runtime_construct(Env *e,const char *typeName,Value values){
    size_t z=strlen(typeName)+9;char *className=xmalloc(z);snprintf(className,z,"__class_%s",typeName[0]==':'?typeName+1:typeName);
    Value proto=env_get(e,className);free(className);if(proto.k!=V_DICT)die("undefined object type");
    char **keys=xmalloc(2*sizeof(char*));Value *vals=xmalloc(2*sizeof(Value));keys[0]=strdup("__object");vals[0]=v_bool(1);keys[1]=strdup("__type");vals[1]=v_str(typeName);
    Value instance=v_dict(keys,vals,2);
    for(int i=0;i<proto.u.dict->n;i++){
        Dict *d=instance.u.dict;dict_reserve(d,d->n+1);
        d->keys[d->n]=strdup(proto.u.dict->keys[i]);Value member=proto.u.dict->vals[i];
        if(member.k==V_FUNC){Env *receiver=env_new(member.u.fn.closure);env_define_local(receiver,"this",instance);env_capture(receiver);member.u.fn.closure=receiver;}
        else member=clone_value(member);
        d->vals[d->n]=member;d->n++;dict_reindex(d);
    }
    for(int i=0;i<instance.u.dict->n;i++)if(instance.u.dict->vals[i].k==V_FUNC&&strncmp(instance.u.dict->keys[i],"__",2)){
        size_t sn=strlen(instance.u.dict->keys[i])+9;char *superName=xmalloc(sn);snprintf(superName,sn,"__super_%s",instance.u.dict->keys[i]);
        for(int j=0;j<instance.u.dict->n;j++)if(!strcmp(instance.u.dict->keys[j],superName)){env_define_local(instance.u.dict->vals[i].u.fn.closure,"super",instance.u.dict->vals[j]);break;}free(superName);
    }
    int init=-1;for(int i=0;i<instance.u.dict->n;i++)if(!strcmp(instance.u.dict->keys[i],"init")){init=i;break;}
    if(init>=0&&instance.u.dict->vals[init].k==V_FUNC&&instance.u.dict->vals[init].u.fn.constructor){
        IR *params=instance.u.dict->vals[init].u.fn.params;int at=0;
        if(params&&params->op&&(params->opcode==OP_BLOCK))for(int i=0;i<params->nargs;i++){
            IR *p=params->args[i];const char *name=p&&p->name?p->name:NULL;if(!name&&p&&p->op&&(p->opcode==OP_CONST)&&IS_STRLIKE(p->v.k))name=p->v.u.s;if(!name||name[0]==':')continue;Value field=v_null();int have=0;
            if(values.k==V_BLOCK&&at<values.u.block.b->n){field=*values.u.block.b->items[at++];have=1;}
            else if(values.k==V_DICT){int found=-1;for(int j=0;j<values.u.dict->n;j++)if(!strcmp(values.u.dict->keys[j],name)){found=j;break;}if(found>=0){field=values.u.dict->vals[found];have=1;}}
            if(have){Value key=v_str(name);Value av[3]={instance,key,field};b_set(e,av,3);}
        }
    }else if(init>=0&&instance.u.dict->vals[init].k==V_FUNC){
        Value stack[8],*argv=NULL;int argc=0;
        if(values.k==V_BLOCK){argc=values.u.block.b->n;argv=argc<=8?stack:xmalloc((size_t)argc*sizeof(Value));for(int i=0;i<argc;i++)argv[i]=*values.u.block.b->items[i];}
        (void)applyFunc(e,instance.u.dict->vals[init],argv,argc);if(argv&&argv!=stack)free(argv);
    }else if(values.k==V_BLOCK){int at=0;for(int i=0;i<instance.u.dict->n&&at<values.u.block.b->n;i++)if(strncmp(instance.u.dict->keys[i],"__",2)&&instance.u.dict->vals[i].k!=V_FUNC)instance.u.dict->vals[i]=*values.u.block.b->items[at++];dict_invalidate_object(instance.u.dict);
    }
    return instance;
}

static Value runtime_inherit(Env *e,const char *baseName,Value extra){
    size_t z=strlen(baseName)+9;char *className=xmalloc(z);snprintf(className,z,"__class_%s",baseName[0]==':'?baseName+1:baseName);Value base=env_get(e,className);free(className);
    if(base.k!=V_DICT||extra.k!=V_DICT)die("invalid object inheritance");Value merged=clone_value(base);
    for(int i=0;i<extra.u.dict->n;i++){
        const char *name=extra.u.dict->keys[i];int found=-1;for(int j=0;j<merged.u.dict->n;j++)if(!strcmp(merged.u.dict->keys[j],name)){found=j;break;}
        if(found>=0&&merged.u.dict->vals[found].k==V_FUNC&&extra.u.dict->vals[i].k==V_FUNC){size_t sn=strlen(name)+9;char *sk=xmalloc(sn);snprintf(sk,sn,"__super_%s",name);Value av[3]={merged,v_str(sk),merged.u.dict->vals[found]};b_set(e,av,3);free(sk);}
        Value av[3]={merged,v_str(name),extra.u.dict->vals[i]};b_set(e,av,3);
    }
    Value pv[3]={merged,v_str("__parent"),v_str(baseName)};b_set(e,pv,3);return merged;
}

/* `to :type value` — the workhorse conversion (237 call sites in the compiler). */
static Value b_to(Env*e,Value*a,int n){
    char taggedType[128];const char *ty=a[0].u.s;if(ty&&ty[0]!=':'){snprintf(taggedType,sizeof taggedType,":%s",ty);ty=taggedType;}Value v=a[1];
    if(!strcmp(ty,":string")&&v.k==V_BINARY){char *text=xmalloc(v.u.binary.len+1);memcpy(text,v.u.binary.data,v.u.binary.len);text[v.u.binary.len]=0;Value result=v_str(text);free(text);return result;}
    if(!strcmp(ty,":string")&&object_member_index(v,"string")>=0)return object_magic(e,v,"string",NULL,0);
    if(!strcmp(ty,":string")){ char *s=val_str(v); Value r=v_str(s); free(s); return r; }
    if(!strcmp(ty,":word")||!strcmp(ty,":label")||!strcmp(ty,":literal")||!strcmp(ty,":symbol")){
        char *s=val_str(v);VKind kind=!strcmp(ty,":word")?V_WORD:!strcmp(ty,":label")?V_LABEL:!strcmp(ty,":literal")?V_LITERAL:V_SYMBOL;
        Value r=v_token(kind,s);free(s);return r;
    }
    if(!strcmp(ty,":integer")){
        if(v.k==V_STR) return v_int_from_text(v.u.s);
        if(v.k==V_FLOAT) return v_int((long)v.u.f);
        if(v.k==V_INT) return v;
        if(v.k==V_BIGINT) return v;
        if(v.k==V_BOOL) return v_int(v.u.b?1:0);
        if(v.k==V_CHAR) return v_int(utf8_codepoint(v.u.c));
        if(v.k==V_RATIONAL) return v_int(v.u.rational.num/v.u.rational.den);
        if(v.k==V_COMPLEX) return v_int((long)v.u.complex.real);
        die("to :integer"); return v_null();
    }
    if(!strcmp(ty,":floating")){
        if(v.k==V_STR) return v_float(atof(v.u.s));
        if(v.k==V_INT) return v_float((double)v.u.i);
        if(v.k==V_FLOAT) return v;
        if(v.k==V_RATIONAL) return v_float(as_float(v));
        if(v.k==V_COMPLEX) return v_float(v.u.complex.real);
        if(v.k==V_QUANTITY) return v_float(v.u.quantity.amount);
        die("to :floating"); return v_null();
    }
    if(!strcmp(ty,":logical")){
        if(v.k==V_STR){if(!strcmp(v.u.s,"true")||!strcmp(v.u.s,"1"))return v_bool(1);if(!strcmp(v.u.s,"false")||!strcmp(v.u.s,"0"))return v_bool(0);die("to :logical: unsupported string value");return v_null();}
        if(v.k==V_INT) return v_bool(v.u.i!=0);
        return v_bool(v_truthy(v));
    }
    if(!strcmp(ty,":char")){
        if(v.k==V_STR){char rb[5];rune_at(v.u.s,rb);return v_char_text(rb);}
        if(v.k==V_INT) return v_char((char)v.u.i);
        if(v.k==V_CHAR) return v;
        die("to :char"); return v_null();
    }
    if(!strcmp(ty,":block")){
        if(v.k==V_BLOCK) return v;
        if(v.k==V_INLINE) return v_block_cpy(v.u.block.b->items, v.u.block.b->n);
        if(v.k==V_RANGE){int count=iterator_count(v);Value **items=xmalloc((size_t)(count?count:1)*sizeof(Value*));for(int i=0;i<count;i++){items[i]=xmalloc(sizeof(Value));*items[i]=iterator_item(v,i);}return v_block(items,count);}
        if(v.k==V_STR) return lex_source(v.u.s);
        if(v.k==V_RATIONAL){Value **it=(Value**)xmalloc(2*sizeof(Value*));for(int i=0;i<2;i++)it[i]=(Value*)xmalloc(sizeof(Value));*it[0]=v_int(v.u.rational.num);*it[1]=v_int(v.u.rational.den);return v_block(it,2);}
        if(v.k==V_COMPLEX){Value **it=(Value**)xmalloc(2*sizeof(Value*));for(int i=0;i<2;i++)it[i]=(Value*)xmalloc(sizeof(Value));*it[0]=v_float(v.u.complex.real);*it[1]=v_float(v.u.complex.imag);return v_block(it,2);}
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
    if(!strcmp(ty,":rational")){
        if(v.k==V_RATIONAL)return v;
        if(v.k==V_INT)return v_rational(v.u.i,1);
        if(v.k==V_BLOCK && v.u.block.b->n==2){Value numerator=*v.u.block.b->items[0],denominator=*v.u.block.b->items[1];if((IS_STRLIKE(numerator.k)&&!strcmp(numerator.u.s,"neg"))||(IS_STRLIKE(denominator.k)&&!strcmp(denominator.u.s,"neg")))die("to :rational");numerator=eval_data_item(e,numerator);denominator=eval_data_item(e,denominator);return v_rational(as_int(numerator),as_int(denominator));}
        die("to :rational");return v_null();
    }
    if(!strcmp(ty,":complex")){
        if(v.k==V_COMPLEX)return v;
        if(v.k==V_INT||v.k==V_FLOAT||v.k==V_RATIONAL)return v_complex(as_float(v),0.0);
        if(v.k==V_BLOCK && v.u.block.b->n==2){Value real=eval_data_item(e,*v.u.block.b->items[0]),imaginary=eval_data_item(e,*v.u.block.b->items[1]);return v_complex(as_float(real),as_float(imaginary));}
        die("to :complex");return v_null();
    }
    if(!strcmp(ty,":quantity")){
        if(v.k==V_QUANTITY)return v;
        if(v.k==V_BLOCK&&v.u.block.b->n==2){Value x=*v.u.block.b->items[0],u=*v.u.block.b->items[1];char *name=val_str(u);Value out=x.k==V_INT?v_quantity_int(x.u.i,name):v_quantity(as_float(x),name);free(name);return out;}
        die("to :quantity");return v_null();
    }
    if(!strcmp(ty,":unit")){
        if(v.k==V_UNIT)return v;
        if(IS_STRLIKE(v.k))return v_unit(v.u.s);
        die("to :unit");return v_null();
    }
    if(!strcmp(ty,":date")){
        if(v.k==V_DATE)return v;
        if(v.k==V_STR&&rt_has_attr("format")){
            Value fmt=rt_attr_value("format",v_str("yyyy-MM-dd"));const char *source=fmt.u.s;char converted[128];size_t used=0;
            for(size_t i=0;source[i]&&used+4<sizeof converted;){const char *code=NULL;size_t take=1;if(!strncmp(source+i,"yyyy",4)){code="%Y";take=4;}else if(!strncmp(source+i,"MMM",3)){code="%b";take=3;}else if(!strncmp(source+i,"MM",2)){code="%m";take=2;}else if(!strncmp(source+i,"dd",2)){code="%d";take=2;}else if(!strncmp(source+i,"HH",2)){code="%H";take=2;}else if(!strncmp(source+i,"mm",2)){code="%M";take=2;}else if(!strncmp(source+i,"ss",2)){code="%S";take=2;}if(code){converted[used++]=code[0];converted[used++]=code[1];}else converted[used++]=source[i];i+=take;}converted[used]=0;
            struct tm tmv;memset(&tmv,0,sizeof tmv);tmv.tm_year=70;tmv.tm_mday=1;if(!strptime(v.u.s,converted,&tmv))die("to :date");return date_epoch(mktime(&tmv));
        }
        if(v.k==V_STR)return v_date_iso(v.u.s);
        die("to :date");return v_null();
    }
    if(!strcmp(ty,":regex")){
        if(v.k==V_REGEX)return v;
        if(v.k==V_STR)return v_token(V_REGEX,v.u.s);
        die("to :regex");return v_null();
    }
    if(!strcmp(ty,":binary")){
        if(v.k==V_BINARY)return v;
        if(v.k==V_STR)return v_binary_text(v.u.s);
        if(v.k==V_INT){unsigned long value=(unsigned long)v.u.i;size_t length=1;for(unsigned long scan=value;scan>255;scan>>=8)length++;Value out;out.k=V_BINARY;out.u.binary.len=length;out.u.binary.data=xmalloc(length);for(size_t i=0;i<length;i++)out.u.binary.data[length-1-i]=(unsigned char)(value>>(8*i));return out;}
        die("to :binary");return v_null();
    }
    if(!strcmp(ty,":bytecode")){
        if(v.k!=V_BLOCK)die("to :bytecode");Value **data=xmalloc((size_t)(v.u.block.b->n? v.u.block.b->n:1)*sizeof(Value*));int used=0;
        for(int i=0;i<v.u.block.b->n;i++){Value item=*v.u.block.b->items[i];if(item.k==V_STR){data[used]=xmalloc(sizeof(Value));*data[used++]=clone_value(item);}}
        Value **code=xmalloc(3*sizeof(Value*));long bytes[]={32,189,223};for(int i=0;i<3;i++){code[i]=xmalloc(sizeof(Value));*code[i]=v_int(bytes[i]);}
        char **keys=xmalloc(2*sizeof(char*));Value *vals=xmalloc(2*sizeof(Value));keys[0]=strdup("data");keys[1]=strdup("code");vals[0]=v_block(data,used);vals[1]=v_block(code,3);return v_dict(keys,vals,2);
    }
    if(!strcmp(ty,":version")){
        if(v.k==V_VERSION)return v;
        if(IS_STRLIKE(v.k))return v_version(v.u.s);
        die("to :version");return v_null();
    }
    if(!strcmp(ty,":errorKind")||!strcmp(ty,":errorkind")){
        if(v.k==V_ERRORKIND)return v;
        if(IS_STRLIKE(v.k))return v_errorkind(v.u.s);
        die("to :errorKind");return v_null();
    }
    if(!strcmp(ty,":color")){
        if(v.k==V_COLOR)return v;
        if(v.k==V_STR)return v_color_hex(v.u.s);
        if(v.k==V_BLOCK && (v.u.block.b->n==3||v.u.block.b->n==4)){
            unsigned r=(unsigned)as_int(*v.u.block.b->items[0])&255u,g=(unsigned)as_int(*v.u.block.b->items[1])&255u,b=(unsigned)as_int(*v.u.block.b->items[2])&255u;
            unsigned alpha=v.u.block.b->n==4?(unsigned)as_int(*v.u.block.b->items[3])&255u:255u;
            Value out;out.k=V_COLOR;out.u.rgba=(r<<24)|(g<<16)|(b<<8)|alpha;return out;
        }
        die("to :color");return v_null();
    }
    if(!strcmp(ty,":array")){ /* array is block-like */
        if(v.k==V_BLOCK) return v;
        return one_elt(v);
    }
    if(ty[0]==':'&&e){
        size_t z=strlen(ty)+9;char *className=xmalloc(z);snprintf(className,z,"__class_%s",ty+1);int known=env_bound(e,className);free(className);
        if(known)return runtime_construct(e,ty,v);
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

/* ---- scalar/text/token predicates --------------------------------------- */
static Value b_truep(Env*e,Value*a,int n){ return v_bool(a[0].k==V_BOOL && a[0].u.b); }
static Value b_falsep(Env*e,Value*a,int n){ return v_bool(a[0].k==V_BOOL && !a[0].u.b); }
static Value b_onep(Env*e,Value*a,int n){
    Value v=a[0];
    if(v.k==V_INT) return v_bool(v.u.i==1);
    if(v.k==V_FLOAT) return v_bool(v.u.f==1.0);
    if(v.k==V_STR) return v_bool(strlen(v.u.s)==1);
    if(v.k==V_BLOCK) return v_bool(v.u.block.b->n==1);
    if(v.k==V_DICT) return v_bool(v.u.dict->n==1);
    if(v.k==V_RANGE) return v_bool(labs(v.u.range.hi-v.u.range.lo)+1==1);
    return v_bool(0);
}
static Value b_asciip(Env*e,Value*a,int n){
    const unsigned char *s=(const unsigned char*)(a[0].k==V_CHAR ? a[0].u.c : a[0].u.s);
    for(;*s;s++) if(*s>127) return v_bool(0);
    return v_bool(1);
}
static Value b_whitespacep(Env*e,Value*a,int n){
    const unsigned char *s=(const unsigned char*)(a[0].k==V_CHAR ? a[0].u.c : a[0].u.s);
    if(!*s) return v_bool(0);
    for(;*s;s++) if(!isspace(*s)) return v_bool(0);
    return v_bool(1);
}
static Value b_lowerp(Env*e,Value*a,int n){
    const unsigned char *s=(const unsigned char*)(a[0].k==V_CHAR ? a[0].u.c : a[0].u.s);
    for(;*s;s++) if(!islower(*s)) return v_bool(0);
    return v_bool(1);
}
static Value b_upperp(Env*e,Value*a,int n){
    const unsigned char *s=(const unsigned char*)(a[0].k==V_CHAR ? a[0].u.c : a[0].u.s);
    for(;*s;s++) if(!isupper(*s)) return v_bool(0);
    return v_bool(1);
}
static Value b_numericp(Env*e,Value*a,int n){
    if(a[0].k==V_INT||a[0].k==V_FLOAT||a[0].k==V_RATIONAL||a[0].k==V_COMPLEX||a[0].k==V_QUANTITY)return v_bool(1);
    char buf[8]={0,0,0,0,0,0,0,0}; const char *s;
    if(a[0].k==V_CHAR){ memcpy(buf,a[0].u.c,5); s=buf; } else s=a[0].u.s;
    if(!s || !*s) return v_bool(0);
    char *end=NULL; (void)strtod(s,&end);
    return v_bool(end && end!=s && *end=='\0');
}
static Value b_prefixp(Env*e,Value*a,int n){
    size_t p=strlen(a[1].u.s); return v_bool(!strncmp(a[0].u.s,a[1].u.s,p));
}
static Value b_suffixp(Env*e,Value*a,int n){
    size_t s=strlen(a[0].u.s), p=strlen(a[1].u.s);
    return v_bool(p<=s && !memcmp(a[0].u.s+s-p,a[1].u.s,p));
}
static Value b_betweenp(Env*e,Value*a,int n){
    if(a[0].k==V_STR && a[1].k==V_STR && a[2].k==V_STR)
        return v_bool(strcmp(a[0].u.s,a[1].u.s)>=0 && strcmp(a[0].u.s,a[2].u.s)<=0);
    double x=as_float(a[0]), lo=as_float(a[1]), hi=as_float(a[2]);
    return v_bool(x>=lo && x<=hi);
}
static int value_same(Value a, Value b){
    if(a.k!=b.k) return 0;
    switch(a.k){
        case V_NULL: return 1;
        case V_INT: return a.u.i==b.u.i;
        case V_BIGINT: return !strcmp(a.u.s,b.u.s);
        case V_FLOAT: return a.u.f==b.u.f;
        case V_CHAR: return !strcmp(a.u.c,b.u.c);
        case V_BOOL: return a.u.b==b.u.b;
        case V_RATIONAL: return a.u.rational.num==b.u.rational.num&&a.u.rational.den==b.u.rational.den;
        case V_COMPLEX: return a.u.complex.real==b.u.complex.real&&a.u.complex.imag==b.u.complex.imag;
        case V_QUANTITY: return !strcmp(a.u.quantity.unit,b.u.quantity.unit)&&fabs(a.u.quantity.amount-b.u.quantity.amount)<=1e-12*fmax(1.0,fmax(fabs(a.u.quantity.amount),fabs(b.u.quantity.amount)));
        case V_UNIT: return !strcmp(a.u.s,b.u.s);
        case V_DATE: return a.u.epoch==b.u.epoch;
        case V_COLOR: return a.u.rgba==b.u.rgba;
        case V_BINARY: return a.u.binary.len==b.u.binary.len&&!memcmp(a.u.binary.data,b.u.binary.data,a.u.binary.len);
        case V_RANGE: return a.u.range.lo==b.u.range.lo && a.u.range.hi==b.u.range.hi && a.u.range.step==b.u.range.step && a.u.range.character==b.u.range.character && a.u.range.infinite==b.u.range.infinite;
        case V_BLOCK:
        case V_INLINE:
            if(a.u.block.b->n!=b.u.block.b->n) return 0;
            for(int i=0;i<a.u.block.b->n;i++) if(!value_same(*a.u.block.b->items[i],*b.u.block.b->items[i])) return 0;
            return 1;
        case V_DICT:
            if(a.u.dict->n!=b.u.dict->n) return 0;
            for(int i=0;i<a.u.dict->n;i++){
                int j=dict_find(b,a.u.dict->keys[i]);
                if(j<0 || !value_same(a.u.dict->vals[i],b.u.dict->vals[j])) return 0;
            }
            return 1;
        case V_PATH:
        case V_PATHLABEL:
        case V_PATHLITERAL:
            if(a.u.path.nsegs!=b.u.path.nsegs) return 0;
            for(int i=0;i<a.u.path.nsegs;i++) if(strcmp(a.u.path.segs[i],b.u.path.segs[i])) return 0;
            return 1;
        case V_FUNC: return a.u.fn.body==b.u.fn.body && a.u.fn.closure==b.u.fn.closure;
        default:
            if(IS_STRLIKE(a.k) || a.k==V_REGEX || a.k==V_ATTRIBUTE || a.k==V_ATTRIBUTELABEL || a.k==V_BUILTIN)
                return !strcmp(a.u.s?a.u.s:"",b.u.s?b.u.s:"");
            return 0;
    }
}
static Value b_samep(Env*e,Value*a,int n){ return v_bool(value_same(a[0],a[1])); }

/* ---- aggregate and number-theory helpers -------------------------------- */
static Block *numeric_block(Value v, const char *name){
    if(v.k!=V_BLOCK){ die(name); return NULL; }
    return v.u.block.b;
}
static Value b_allp(Env*e,Value*a,int n){
    Block *xs=numeric_block(a[0],"all?: expected block");
    for(int i=0;i<xs->n;i++) if(xs->items[i]->k!=V_BOOL || !xs->items[i]->u.b) return v_bool(0);
    return v_bool(1);
}
static Value b_anyp(Env*e,Value*a,int n){
    Block *xs=numeric_block(a[0],"any?: expected block");
    for(int i=0;i<xs->n;i++) if(xs->items[i]->k==V_BOOL && xs->items[i]->u.b) return v_bool(1);
    return v_bool(0);
}
static Value b_average(Env*e,Value*a,int n){
    Block *xs=numeric_block(a[0],"average: expected block");
    if(xs->n==0) die("average: empty block");
    double sum=0.0; for(int i=0;i<xs->n;i++) sum+=as_float(*xs->items[i]);
    return v_float(sum/(double)xs->n);
}
static Value b_clamp(Env*e,Value*a,int n){
    Value x=a[0], lo, hi;
    if(a[1].k==V_RANGE){ long first=a[1].u.range.lo,last=a[1].u.range.hi;lo=v_int(first<last?first:last);hi=v_int(first>last?first:last); }
    else {
        Block *bounds=numeric_block(a[1],"clamp: expected range or block");
        if(bounds->n<2) die("clamp: expected two bounds");
        lo=*bounds->items[0]; hi=*bounds->items[1];
    }
    if(as_float(x)<as_float(lo)) return lo;
    if(as_float(x)>as_float(hi)) return hi;
    return x;
}
static Value b_exp(Env*e,Value*a,int n){ return v_float(exp(as_float(a[0]))); }
static Value b_factorial(Env*e,Value*a,int n){
    long x=as_int(a[0]); if(x<0) die("factorial: expected non-negative integer");
    long r=1; for(long i=2;i<=x;i++) r*=i; return v_int(r);
}
static Value b_factors(Env*e,Value*a,int n){
    long x=labs(as_int(a[0])); if(x==0) die("factors: zero has infinitely many factors");
    if(rt_has_attr("prime")){
        long rest=x;int count=0,cap=8;Value **items=(Value**)xmalloc((size_t)cap*sizeof(Value*));
        for(long p=2;p<=rest/p;p+=(p==2?1:2))while(rest%p==0){if(count==cap){cap*=2;items=(Value**)xrealloc(items,(size_t)cap*sizeof(Value*));}items[count]=(Value*)xmalloc(sizeof(Value));*items[count++]=v_int(p);rest/=p;}
        if(rest>1){if(count==cap){cap*=2;items=(Value**)xrealloc(items,(size_t)cap*sizeof(Value*));}items[count]=(Value*)xmalloc(sizeof(Value));*items[count++]=v_int(rest);}
        return v_block(items,count);
    }
    int count=0; for(long i=1;i<=x/i;i++) if(x%i==0) count+=(i==x/i?1:2);
    Value **items=(Value**)xmalloc((size_t)count*sizeof(Value*)); int at=0;
    for(long i=1;i<=x;i++) if(x%i==0){ items[at]=(Value*)xmalloc(sizeof(Value)); *items[at++]=v_int(i); }
    return v_block(items,count);
}
static Value b_hypot(Env*e,Value*a,int n){ return v_float(hypot(as_float(a[0]),as_float(a[1]))); }
static Value b_lcm(Env*e,Value*a,int n){
    Block *xs=numeric_block(a[0],"lcm: expected block");
    if(xs->n==0) return v_int(1);
    long r=labs(as_int(*xs->items[0]));
    for(int i=1;i<xs->n;i++){ long x=labs(as_int(*xs->items[i])); r=(r==0||x==0)?0:labs((r/gcd_pair(r,x))*x); }
    return v_int(r);
}
static int value_num_cmp(const void *pa,const void *pb){
    const Value *a=(const Value*)pa,*b=(const Value*)pb;
    double x=as_float(*a),y=as_float(*b); return (x>y)-(x<y);
}
static Value b_median(Env*e,Value*a,int n){
    Block *xs=numeric_block(a[0],"median: expected block");
    if(xs->n==0) die("median: empty block");
    Value *copy=(Value*)xmalloc((size_t)xs->n*sizeof(Value));
    for(int i=0;i<xs->n;i++) copy[i]=*xs->items[i];
    qsort(copy,(size_t)xs->n,sizeof(Value),value_num_cmp);
    Value r;
    if(xs->n&1) r=copy[xs->n/2];
    else r=v_float((as_float(copy[xs->n/2-1])+as_float(copy[xs->n/2]))/2.0);
    free(copy); return r;
}
static Value b_powmod(Env*e,Value*a,int n){
    long base=as_int(a[0]), expn=as_int(a[1]), modn=as_int(a[2]);
    if(expn<0 || modn==0) die("powmod: invalid exponent or modulus");
    long result=1%modn; base%=modn;
    while(expn){ if(expn&1) result=(long)(((__int128)result*base)%modn); expn>>=1; if(expn) base=(long)(((__int128)base*base)%modn); }
    return v_int(result);
}
static Value b_primep(Env*e,Value*a,int n){
    long x=as_int(a[0]); if(x<2) return v_bool(0); if(x%2==0) return v_bool(x==2);
    for(long d=3;d<=x/d;d+=2) if(x%d==0) return v_bool(0);
    return v_bool(1);
}
static double block_variance(Value v){
    Block *xs=numeric_block(v,"variance: expected block");
    double mean=0.0; for(int i=0;i<xs->n;i++) mean+=as_float(*xs->items[i]); mean/=xs->n;
    double sum=0.0; for(int i=0;i<xs->n;i++){ double d=as_float(*xs->items[i])-mean; sum+=d*d; }
    return sum/xs->n;
}
static Value b_variance(Env*e,Value*a,int n){ return v_float(block_variance(a[0])); }
static Value b_deviation(Env*e,Value*a,int n){ return v_float(sqrt(block_variance(a[0]))); }
static Value b_skewness(Env*e,Value*a,int n){
    Block *xs=numeric_block(a[0],"skewness: expected block");
    double mean=0.0; for(int i=0;i<xs->n;i++) mean+=as_float(*xs->items[i]); mean/=xs->n;
    double m2=0.0,m3=0.0;
    for(int i=0;i<xs->n;i++){
        double d=as_float(*xs->items[i])-mean, d2=d*d;
        m2+=d2; m3+=d2*d;
    }
    m2/=xs->n; m3/=xs->n;
    return v_float(m3/pow(m2,1.5));
}
static Value b_kurtosis(Env*e,Value*a,int n){
    Block *xs=numeric_block(a[0],"kurtosis: expected block");
    double mean=0.0; for(int i=0;i<xs->n;i++) mean+=as_float(*xs->items[i]); mean/=xs->n;
    double m2=0.0,m4=0.0;
    for(int i=0;i<xs->n;i++){
        double d=as_float(*xs->items[i])-mean, d2=d*d;
        m2+=d2; m4+=d2*d2;
    }
    m2/=xs->n; m4/=xs->n;
    return v_float(m4/(m2*m2)-3.0);
}

/* ---- collection construction and relations ------------------------------ */
static Value b_digits(Env*e,Value*a,int n){
    long x=labs(as_int(a[0])); long p=1; int count=1;
    while(x/p>=10){ p*=10; count++; }
    Value **items=(Value**)xmalloc((size_t)count*sizeof(Value*));
    for(int i=0;i<count;i++){ items[i]=(Value*)xmalloc(sizeof(Value)); *items[i]=v_int((x/p)%10); p/=10; }
    return v_block(items,count);
}
static Value b_couple(Env*e,Value*a,int n){
    Block *x=numeric_block(a[0],"couple: expected block");
    Block *y=numeric_block(a[1],"couple: expected block");
    int count=x->n<y->n?x->n:y->n;
    Value **items=(Value**)xmalloc((size_t)count*sizeof(Value*));
    for(int i=0;i<count;i++){
        Value **pair=(Value**)xmalloc(2*sizeof(Value*));
        pair[0]=(Value*)xmalloc(sizeof(Value)); *pair[0]=*x->items[i];
        pair[1]=(Value*)xmalloc(sizeof(Value)); *pair[1]=*y->items[i];
        items[i]=(Value*)xmalloc(sizeof(Value)); *items[i]=v_block(pair,2);
    }
    return v_block(items,count);
}
static int ordered_pair(Value a,Value b){
    if((a.k==V_INT||a.k==V_FLOAT) && (b.k==V_INT||b.k==V_FLOAT)) return as_float(a)<=as_float(b);
    char *sa=val_str(a),*sb=val_str(b); int ok=strcmp(sa,sb)<=0; free(sa); free(sb); return ok;
}
static Value b_sortedp(Env*e,Value*a,int n){
    Block *xs=numeric_block(a[0],"sorted?: expected block");
    int descending=rt_has_attr("descending");for(int i=1;i<xs->n;i++){int order=order_values(*xs->items[i-1],*xs->items[i]);if(descending?order<0:order>0)return v_bool(0);}
    return v_bool(1);
}
static Value b_tally(Env*e,Value*a,int n){
    Value v=a[0]; int count=v.k==V_BLOCK?v.u.block.b->n:(v.k==V_STR?(int)strlen(v.u.s):-1);
    if(count<0) die("tally: expected block or string");
    char **keys=NULL; Value *vals=NULL; int used=0;
    for(int i=0;i<count;i++){
        Value item;
        if(v.k==V_BLOCK) item=*v.u.block.b->items[i]; else item=v_str((char[]){v.u.s[i],0});
        char *key=val_str(item); int found=-1;
        for(int j=0;j<used;j++) if(!strcmp(keys[j],key)){ found=j; break; }
        if(found>=0){ vals[found].u.i++; free(key); }
        else {
            keys=(char**)xrealloc(keys,(size_t)(used+1)*sizeof(char*));
            vals=(Value*)xrealloc(vals,(size_t)(used+1)*sizeof(Value));
            keys[used]=key; vals[used]=v_int(1); used++;
        }
    }
    return v_dict(keys,vals,used);
}
static int block_contains_value(Block *xs,Value needle){
    for(int i=0;i<xs->n;i++) if(value_eq(*xs->items[i],needle)) return 1;
    return 0;
}
static Value b_disjointp(Env*e,Value*a,int n){
    Block *x=numeric_block(a[0],"disjoint?: expected block"),*y=numeric_block(a[1],"disjoint?: expected block");
    for(int i=0;i<x->n;i++) if(block_contains_value(y,*x->items[i])) return v_bool(0);
    return v_bool(1);
}
static Value b_intersectp(Env*e,Value*a,int n){ Value r=b_disjointp(e,a,n); r.u.b=!r.u.b; return r; }
static Value b_subsetp(Env*e,Value*a,int n){
    Block *x=numeric_block(a[0],"subset?: expected block"),*y=numeric_block(a[1],"subset?: expected block");
    for(int i=0;i<x->n;i++) if(!block_contains_value(y,*x->items[i])) return v_bool(0);
    return v_bool(1);
}
static Value b_supersetp(Env*e,Value*a,int n){ Value rev[2]={a[1],a[0]}; return b_subsetp(e,rev,2); }

static Value make_block_slice(Block *src, const int *indices, int count){
    Value **items=(Value**)xmalloc((size_t)(count?count:1)*sizeof(Value*));
    for(int i=0;i<count;i++){
        items[i]=(Value*)xmalloc(sizeof(Value));
        *items[i]=*src->items[indices[i]];
    }
    return v_block(items,count);
}
static Value b_permutate(Env*e,Value*a,int n);
static void permutation_fill(Block *src,int width,int repeated,int depth,int *order,int *used,Value **out,int *at){
    if(depth==width){
        out[*at]=(Value*)xmalloc(sizeof(Value));
        *out[(*at)++]=make_block_slice(src,order,width);
        return;
    }
    for(int i=0;i<src->n;i++)if(repeated||!used[i]){
        if(!repeated)used[i]=1; order[depth]=i;
        permutation_fill(src,width,repeated,depth+1,order,used,out,at);
        if(!repeated)used[i]=0;
    }
}
static Value b_permutate(Env*e,Value*a,int n){
    Block *src=numeric_block(a[0],"permutate: expected block");
    int width=rt_has_attr("by")?(int)as_int(rt_attr_value("by",v_int(src->n))):src->n,repeated=rt_has_attr("repeated");if(width<0)die("permutate.by: expected nonnegative size");size_t count=1;
    if(!repeated&&width>src->n)count=0;else for(int i=0;i<width;i++)count*=repeated?(size_t)src->n:(size_t)(src->n-i);
    if(rt_has_attr("count"))return v_int((long)count);
    Value **items=(Value**)xmalloc((count?count:1)*sizeof(Value*));
    int *order=(int*)xmalloc((size_t)(width?width:1)*sizeof(int));
    int *used=(int*)xmalloc((size_t)(src->n?src->n:1)*sizeof(int));
    memset(used,0,(size_t)(src->n?src->n:1)*sizeof(int));
    int at=0;if(count)permutation_fill(src,width,repeated,0,order,used,items,&at); free(order); free(used);
    return v_block(items,at);
}
static void combine_fill(Block*source,int width,int repeated,int depth,int start,Value*current,Value out){
    if(depth==width){Value**items=xmalloc((size_t)(width+1)*sizeof(Value*));for(int i=0;i<width;i++){items[i]=xmalloc(sizeof(Value));*items[i]=clone_value(current[i]);}block_append(out,v_block(items,width));return;}
    for(int i=start;i<source->n;i++){current[depth]=*source->items[i];combine_fill(source,width,repeated,depth+1,repeated?i:i+1,current,out);}
}
static Value b_combine(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_BLOCK)die("combine: expected block");Block*source=a[0].u.block.b;long width=source->n;Value by=rt_attr_value("by",v_int(width));width=as_int(by);if(width<0)width=0;int repeated=rt_has_attr("repeated");Value out=v_block(xmalloc(sizeof(Value*)),0);
    if(width==0)block_append(out,v_block(xmalloc(sizeof(Value*)),0));else if(repeated||width<=source->n){Value*current=xmalloc((size_t)width*sizeof(Value));combine_fill(source,(int)width,repeated,0,0,current,out);free(current);}
    if(rt_has_attr("count"))return v_int(out.u.block.b->n);return out;
}

/* ---- in-place collection/string transforms ------------------------------ */
static Value mut_load(Env *e, Value receiver, MutTarget *target){
    memset(target,0,sizeof *target); target->index=-1;
    if(receiver.k!=V_PATH) return receiver;
    if(receiver.u.path.nsegs==1){
        target->kind=1; target->var=receiver.u.path.segs[0];
        if(!env_bound(e,target->var)){size_t z=strlen(target->var)+9;char *shadow=xmalloc(z);snprintf(shadow,z,"_shadow_%s",target->var);if(env_bound(e,shadow))target->var=shadow;else free(shadow);}
        return env_get(e,target->var);
    }
    target->kind=2;
    if(path_target(e,receiver,&target->container,&target->index)<0) die("mutation path not found");
    return target->container.k==V_DICT?target->container.u.dict->vals[target->index]:*target->container.u.block.b->items[target->index];
}
static void mut_store(Env *e, MutTarget *target, Value value){
    if(target->kind==1) env_set(e,target->var,value);
    else if(target->kind==2) path_store(target->container,target->index,value);
}
static Value b_escape(Env*e,Value*a,int n){MutTarget target;Value source=mut_load(e,a[0],&target),result=escape_value(e,&source,n);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
static Value b_unescape(Env*e,Value*a,int n){MutTarget target;Value source=mut_load(e,a[0],&target),result=unescape_value(e,&source,n);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
static Value b_render(Env*e,Value*a,int n){MutTarget target;Value source=mut_load(e,a[0],&target),result=render_value(e,&source,n);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
static Value b_translate(Env*e,Value*a,int n){MutTarget target;Value args[2]={mut_load(e,a[0],&target),a[1]},result=translate_value(e,args,n);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
static Value b_wordwrap(Env*e,Value*a,int n){MutTarget target;Value source=mut_load(e,a[0],&target),result=wordwrap_value(e,&source,n);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
static Value step_numeric(Value v,int direction){if(v.k==V_INT)return v_int(v.u.i+direction);if(v.k==V_FLOAT)return v_float(v.u.f+direction);if(v.k==V_RATIONAL)return v_rational(v.u.rational.num+direction*v.u.rational.den,v.u.rational.den);if(v.k==V_COMPLEX)return v_complex(v.u.complex.real+direction,v.u.complex.imag);if(v.k==V_QUANTITY){double x=v.u.quantity.amount+direction;return v.u.quantity.integral?v_quantity_int((long)x,v.u.quantity.unit):v_quantity(x,v.u.quantity.unit);}die("expected number");return v_null();}
static Value mutate_unary(Env*e,Value receiver,int direction){MutTarget target;Value value=mut_load(e,receiver,&target),result=step_numeric(value,direction);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
static Value b_inc(Env*e,Value*a,int n){(void)n;return mutate_unary(e,a[0],1);}
static Value b_dec(Env*e,Value*a,int n){(void)n;return mutate_unary(e,a[0],-1);}
static Value b_neg(Env*e,Value*a,int n){(void)n;MutTarget target;Value value=mut_load(e,a[0],&target),result;if(value.k==V_QUANTITY)result=value.u.quantity.integral?v_quantity_int((long)-value.u.quantity.amount,value.u.quantity.unit):v_quantity(-value.u.quantity.amount,value.u.quantity.unit);else if(value.k==V_COMPLEX)result=v_complex(-value.u.complex.real,-value.u.complex.imag);else if(value.k==V_FLOAT)result=v_float(-value.u.f);else if(value.k==V_RATIONAL)result=v_rational(-value.u.rational.num,value.u.rational.den);else if(value.k==V_BIGINT)result=v_bigint_text(big_negate(value.u.s));else if(value.k==V_INT)result=v_int(-value.u.i);else die("neg: expected number");if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
static Value b_normalize(Env*e,Value*a,int n){
    (void)n;MutTarget target;Value source=mut_load(e,a[0],&target);Value result=normalize_value(source);
    if(target.kind){mut_store(e,&target,result);return v_null();}return result;
}
static long attr_long(const char*name){Value v=rt_attr_value(name,v_int(0));return as_int(v);}
static int days_in_month(int year,int month){static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};if(month==2&&((year%4==0&&year%100!=0)||year%400==0))return 29;return days[month-1];}
static Value b_shift_date(Env*e,Value*a,int n,int direction){
    (void)n;MutTarget target;Value source=mut_load(e,a[0],&target);if(source.k!=V_DATE)die("date shift: expected date");time_t stamp=(time_t)source.u.epoch;struct tm tmv;localtime_r(&stamp,&tmv);
    long overflowDays=0,monthDelta=direction*(attr_long("months")+12*attr_long("years"));if(monthDelta){long total=(long)(tmv.tm_year+1900)*12+tmv.tm_mon+monthDelta;long year=total/12,month=total%12;if(month<0){month+=12;year--;}int day=tmv.tm_mday,limit=days_in_month((int)year,(int)month+1);if(day>limit){overflowDays=day-limit;day=limit;}tmv.tm_year=(int)year-1900;tmv.tm_mon=(int)month;tmv.tm_mday=day;stamp=mktime(&tmv);}
    long seconds=attr_long("seconds")+60*attr_long("minutes")+3600*attr_long("hours")+86400*(attr_long("days")+overflowDays)+604800*attr_long("weeks");stamp+=(time_t)(direction*seconds);Value out=date_epoch(stamp);if(target.kind){mut_store(e,&target,out);return v_null();}return out;
}
static Value b_after(Env*e,Value*a,int n){return b_shift_date(e,a,n,1);}static Value b_before(Env*e,Value*a,int n){return b_shift_date(e,a,n,-1);}
static Value b_divmod(Env*e,Value*a,int n){
    MutTarget target;Value left=mut_load(e,a[0],&target),quotient,remainder;
    if(left.k==V_COMPLEX||a[1].k==V_COMPLEX){die("divmod: complex unsupported");return v_null();}
    if(left.k==V_RATIONAL||a[1].k==V_RATIONAL){double divisor=as_float(a[1]);if(divisor==0.0)die("division by zero");double q=as_float(left)/divisor,rest=fmod(as_float(left),divisor);quotient=floor(q)==q?v_int((long)q):v_float(q);remainder=floor(rest)==rest?v_int((long)rest):v_float(rest);}
    else if(left.k==V_FLOAT||a[1].k==V_FLOAT){
        double divisor=as_float(a[1]);if(divisor==0.0)die("division by zero");
        quotient=v_float(as_float(left)/divisor);remainder=v_float(fmod(as_float(left),divisor));
    }else{
        long divisor=as_int(a[1]);if(divisor==0)die("division by zero");
        quotient=v_int(as_int(left)/divisor);remainder=v_int(as_int(left)%divisor);
    }
    Value **items=(Value**)xmalloc(2*sizeof(Value*));
    items[0]=(Value*)xmalloc(sizeof(Value));*items[0]=quotient;
    items[1]=(Value*)xmalloc(sizeof(Value));*items[1]=remainder;
    Value result=v_block(items,2);
    if(target.kind){mut_store(e,&target,result);return v_null();}
    return result;
}
static Value b_discard(Env*e,Value*a,int n){(void)e;(void)a;(void)n;return v_null();}
static Value b_dup(Env*e,Value*a,int n){(void)e;(void)n;return a[0];}
static Value b_attr(Env*e,Value*a,int n){
    (void)e;(void)n;char*name=val_str(a[0]);int i=rt_attr_index(name);free(name);if(i<0)return v_null();Value out=g_attrs.values[i];for(int j=i+1;j<g_attrs.n;j++){g_attrs.names[j-1]=g_attrs.names[j];g_attrs.values[j-1]=g_attrs.values[j];}g_attrs.n--;return out;
}
static Value b_attrp(Env*e,Value*a,int n){(void)e;(void)n;char*name=val_str(a[0]);int found=rt_has_attr(name);free(name);return v_bool(found);}
static Value b_attrs(Env*e,Value*a,int n){
    (void)e;(void)a;(void)n;int count=g_attrs.n;char**names=xmalloc((size_t)(count+1)*sizeof(char*));Value*values=xmalloc((size_t)(count+1)*sizeof(Value));for(int i=0;i<count;i++){names[i]=strdup(g_attrs.names[i]);values[i]=clone_value(g_attrs.values[i]);}g_attrs.n=0;return v_dict(names,values,count);
}
static Value b_sign(Env*e,Value*a,int n){
    (void)e;(void)n;double x;
    if(a[0].k==V_COMPLEX){x=a[0].u.complex.real;if(x==0.0)x=a[0].u.complex.imag;}
    else if(a[0].k==V_QUANTITY)x=a[0].u.quantity.amount;
    else x=as_float(a[0]);
    return v_int(x>0.0?1:(x<0.0?-1:0));
}
static Value b_random(Env*e,Value*a,int n){
    (void)e;(void)n;
    if(a[0].k==V_INT&&a[1].k==V_INT){long lo=a[0].u.i,hi=a[1].u.i;if(hi<lo){long t=lo;lo=hi;hi=t;}return v_int(lo+(long)(rand()%(unsigned long)(hi-lo+1)));}
    double lo=as_float(a[0]),hi=as_float(a[1]);if(hi<lo){double t=lo;lo=hi;hi=t;}return v_float(lo+(hi-lo)*((double)rand()/(double)RAND_MAX));
}
static Value b_sample(Env*e,Value*a,int n){
    (void)e;(void)n;
    if(a[0].k==V_BLOCK){Block*b=a[0].u.block.b;if(!b->n)return v_null();return *b->items[rand()%b->n];}
    if(a[0].k==V_RANGE){int count=iterator_count(a[0]);return iterator_item(a[0],rand()%count);}
    die("sample: expected block or range");return v_null();
}
static Value b_shuffle(Env*e,Value*a,int n){
    (void)n;MutTarget target;Value source=mut_load(e,a[0],&target);if(source.k!=V_BLOCK)die("shuffle: expected block");Value out=clone_value(source);Block*b=out.u.block.b;
    for(int i=b->n-1;i>0;i--){int j=rand()%(i+1);Value*t=b->items[i];b->items[i]=b->items[j];b->items[j]=t;}
    if(target.kind){mut_store(e,&target,out);return v_null();}return out;
}
static Value b_extend(Env*e,Value*a,int n){
    (void)n;MutTarget target;Value parent=mut_load(e,a[0],&target);if(parent.k!=V_DICT||a[1].k!=V_DICT)die("extend: expected dictionaries");Value out=clone_value(parent);
    for(int i=0;i<a[1].u.dict->n;i++){Value av[3]={out,v_str(a[1].u.dict->keys[i]),a[1].u.dict->vals[i]};b_set(e,av,3);}
    if(target.kind){mut_store(e,&target,out);return v_null();}return out;
}
static Value b_decouple(Env*e,Value*a,int n){
    (void)n;MutTarget target;Value source=mut_load(e,a[0],&target);if(source.k!=V_BLOCK)die("decouple: expected block");Block*rows=source.u.block.b;Value **left=xmalloc((size_t)(rows->n+1)*sizeof(Value*)),**right=xmalloc((size_t)(rows->n+1)*sizeof(Value*));
    for(int i=0;i<rows->n;i++){Value row=*rows->items[i];if(row.k!=V_BLOCK||row.u.block.b->n<2)die("decouple: expected pairs");left[i]=xmalloc(sizeof(Value));right[i]=xmalloc(sizeof(Value));*left[i]=*row.u.block.b->items[0];*right[i]=*row.u.block.b->items[1];}
    Value **cols=xmalloc(2*sizeof(Value*));cols[0]=xmalloc(sizeof(Value));cols[1]=xmalloc(sizeof(Value));*cols[0]=v_block(left,rows->n);*cols[1]=v_block(right,rows->n);Value out=v_block(cols,2);
    if(target.kind){mut_store(e,&target,out);return v_null();}return out;
}
static Value b_truncate(Env*e,Value*a,int n){
    (void)n;MutTarget target;Value source=mut_load(e,a[0],&target);if(source.k!=V_STR)die("truncate: expected string");long at=as_int(a[1]);if(at<0)at=0;
    Value fillerValue=rt_attr_value("with",v_str("..."));if(fillerValue.k!=V_STR)die("truncate.with: expected string");const char*filler=fillerValue.u.s;size_t len=strlen(source.u.s),keep=len;
    if(rt_has_attr("preserve")){
        if(len>(size_t)at){long i=at;if(i>=(long)len)i=(long)len-1;while(i>0&&!isspace((unsigned char)source.u.s[i]))i--;i--;while(i>0&&isspace((unsigned char)source.u.s[i]))i--;keep=(size_t)(i+1);}
    }else if(len>(size_t)at+strlen(filler))keep=(size_t)at+1;
    if(keep==len){if(target.kind)return v_null();return clone_value(source);}
    char*out=xmalloc(keep+strlen(filler)+1);memcpy(out,source.u.s,keep);strcpy(out+keep,filler);Value result=v_str(out);free(out);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
}
static void block_replace(Block *dst, Value **items, int n){
    dst->items=items; dst->n=n; dst->cap=n;
}
static Value b_powerset(Env*e,Value*a,int n){
    MutTarget t; Value source=mut_load(e,a[0],&t);
    if(source.k!=V_BLOCK)die("powerset: expected block");
    Block *src=source.u.block.b;
    if(src->n>62)die("powerset: input too large");
    size_t count=(size_t)1u<<src->n;
    Value **sets=(Value**)xmalloc(count*sizeof(Value*));
    int *indices=(int*)xmalloc((size_t)(src->n?src->n:1)*sizeof(int));
    for(size_t mask=0;mask<count;mask++){
        int used=0; for(int i=0;i<src->n;i++)if(mask&((size_t)1u<<i))indices[used++]=i;
        sets[mask]=(Value*)xmalloc(sizeof(Value));
        *sets[mask]=make_block_slice(src,indices,used);
    }
    free(indices);
    Value result=v_block(sets,(int)count);
    if(t.kind){mut_store(e,&t,result);return v_null();}
    return result;
}
static Value b_capitalize(Env*e,Value*a,int n){
    MutTarget t; Value v=mut_load(e,a[0],&t); if(v.k!=V_STR) die("capitalize: expected string");
    Value r=v_str(v.u.s);if(r.u.s[0])r.u.s[0]=(char)toupper((unsigned char)r.u.s[0]);if(t.kind){mut_store(e,&t,r);return v_null();}return r;
}
static Value b_chop(Env*e,Value*a,int n){
    MutTarget t; Value v=mut_load(e,a[0],&t);
    if(v.k==V_STR){ size_t len=strlen(v.u.s); char *s=(char*)xmalloc(len+1); memcpy(s,v.u.s,len+1); if(len)s[len-1]=0; Value r=v_str(s); free(s); if(t.kind){mut_store(e,&t,r);return v_null();} return r; }
    if(v.k==V_BLOCK){
        if(t.kind){ if(v.u.block.b->n)v.u.block.b->n--; return v_null(); }
        Value r=clone_value(v); if(r.u.block.b->n)r.u.block.b->n--; return r;
    }
    die("chop: expected string or block"); return v_null();
}
static Value b_empty_value(Env*e,Value*a,int n){
    MutTarget t; Value v=mut_load(e,a[0],&t);
    if(v.k==V_STR){ Value r=v_str(""); mut_store(e,&t,r); return v_null(); }
    if(v.k==V_BLOCK){ v.u.block.b->n=0; return v_null(); }
    if(v.k==V_DICT){ v.u.dict->n=0; dict_reindex(v.u.dict); return v_null(); }
    die("empty: unsupported value"); return v_null();
}
static void flatten_into(Block *src, Value ***out, int *n, int *cap, int once){
    for(int i=0;i<src->n;i++){
        Value v=*src->items[i];
        if(v.k==V_BLOCK){
            if(once){for(int j=0;j<v.u.block.b->n;j++){if(*n>=*cap){*cap=*cap?*cap*2:8;*out=(Value**)xrealloc(*out,(size_t)*cap*sizeof(Value*));}(*out)[*n]=xmalloc(sizeof(Value));*(*out)[(*n)++]=*v.u.block.b->items[j];}}
            else flatten_into(v.u.block.b,out,n,cap,0);
        }
        else { if(*n>=*cap){ *cap=*cap?*cap*2:8; *out=(Value**)xrealloc(*out,(size_t)*cap*sizeof(Value*)); } (*out)[*n]=(Value*)xmalloc(sizeof(Value)); *(*out)[(*n)++]=v; }
    }
}
static Value b_flatten(Env*e,Value*a,int n){
    MutTarget t; Value v=mut_load(e,a[0],&t); if(v.k!=V_BLOCK) die("flatten: expected block");
    Value **items=NULL; int count=0,cap=0; flatten_into(v.u.block.b,&items,&count,&cap,rt_has_attr("once"));
    Value result=v_block(items,count);if(t.kind){mut_store(e,&t,result);return v_null();}return result;
}
static char *indent_text(const char *s, int remove){
    size_t len=strlen(s),cap=len*5+8,at=0; char *out=(char*)xmalloc(cap); int line=1;
    for(size_t i=0;i<len;){
        if(line){
            if(remove){ int k=0; while(k<4 && s[i]==' '){ i++; k++; } }
            else { memcpy(out+at,"    ",4); at+=4; }
            line=0;
        }
        if(i>=len) break; out[at++]=s[i]; if(s[i++]=='\n') line=1;
    }
    out[at]=0; return out;
}
static Value b_indent(Env*e,Value*a,int n){ MutTarget t; Value v=mut_load(e,a[0],&t); if(v.k!=V_STR)die("indent: expected string"); char*s=indent_text(v.u.s,0); Value r=v_str(s);free(s);mut_store(e,&t,r);return v_null(); }
static Value b_outdent(Env*e,Value*a,int n){ MutTarget t; Value v=mut_load(e,a[0],&t); if(v.k!=V_STR)die("outdent: expected string"); char*s=indent_text(v.u.s,1); Value r=v_str(s);free(s);mut_store(e,&t,r);return v_null(); }
static Value b_pad(Env*e,Value*a,int n){
    MutTarget t; Value v=mut_load(e,a[0],&t); if(v.k!=V_STR)die("pad: expected string"); long width=as_int(a[1]); size_t len=strlen(v.u.s);
    if(width>(long)len){ char*s=(char*)xmalloc((size_t)width+1); memset(s,' ',(size_t)width-len); memcpy(s+width-len,v.u.s,len+1); Value r=v_str(s);free(s);mut_store(e,&t,r); }
    return v_null();
}
static Value b_prepend(Env*e,Value*a,int n){
    MutTarget t; Value v=mut_load(e,a[0],&t);
    if(v.k==V_BLOCK){ Block*b=v.u.block.b; block_grow(b,b->n+1); memmove(b->items+1,b->items,(size_t)b->n*sizeof(Value*)); b->items[0]=(Value*)xmalloc(sizeof(Value)); *b->items[0]=a[1]; b->n++; return t.kind?v_null():v; }
    if(v.k==V_STR||v.k==V_CHAR){ char*body=val_str(v),*prefix=val_str(a[1]); char*s=(char*)xmalloc(strlen(prefix)+strlen(body)+1); strcpy(s,prefix);strcat(s,body);Value r=v_str(s);free(body);free(prefix);free(s);if(t.kind){mut_store(e,&t,r);return v_null();}return r; }
    if(v.k==V_BINARY){size_t prefix=a[1].k==V_BINARY?a[1].u.binary.len:1;Value r;r.k=V_BINARY;r.u.binary.len=prefix+v.u.binary.len;r.u.binary.data=xmalloc(r.u.binary.len);if(a[1].k==V_BINARY)memcpy(r.u.binary.data,a[1].u.binary.data,prefix);else r.u.binary.data[0]=(unsigned char)as_int(a[1]);memcpy(r.u.binary.data+prefix,v.u.binary.data,v.u.binary.len);if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
    die("prepend: unsupported value"); return v_null();
}
static Value b_remove(Env*e,Value*a,int n){
    MutTarget t; Value v=mut_load(e,a[0],&t);
    if(v.k==V_BLOCK){Block*b=v.u.block.b;int at=0,once=rt_has_attr("once"),removed=0;if(rt_has_attr("index")){long index=as_int(a[1]);for(int i=0;i<b->n;i++)if(i!=index)b->items[at++]=b->items[i];}else if(a[1].k==V_BLOCK&&!rt_has_attr("instance")){Block*needle=a[1].u.block.b;if(needle->n>0){for(int i=0;i<b->n;){int match=i+needle->n<=b->n;for(int j=0;match&&j<needle->n;j++)if(!value_eq(*b->items[i+j],*needle->items[j]))match=0;if(match&&(!once||!removed)){i+=needle->n;removed=1;}else b->items[at++]=b->items[i++];}}else for(int i=0;i<b->n;i++)b->items[at++]=b->items[i];}else for(int i=0;i<b->n;i++){int match=value_eq(*b->items[i],a[1]);if(match&&(!once||!removed))removed=1;else b->items[at++]=b->items[i];}b->n=at;return t.kind?v_null():v;}
    if(v.k==V_DICT){int at=0,keyMode=rt_has_attr("key");for(int i=0;i<v.u.dict->n;i++){int match=keyMode?!strcmp(v.u.dict->keys[i],key_text(a[1])):value_eq(v.u.dict->vals[i],a[1]);if(!match){v.u.dict->keys[at]=v.u.dict->keys[i];v.u.dict->vals[at++]=v.u.dict->vals[i];}}v.u.dict->n=at;dict_reindex(v.u.dict);return t.kind?v_null():v;}
    if(v.k==V_STR){ char*needle=(a[1].k==V_CHAR&&a[1].u.c[0]==39&&strchr(v.u.s,'+'))?strdup("+"):val_str(a[1]); size_t nl=strlen(needle); if(!nl){free(needle);return t.kind?v_null():v;} const char*p=v.u.s;size_t cap=strlen(p)+1,at=0;char*out=(char*)xmalloc(cap);int once=rt_has_attr("once"),prefix=rt_has_attr("prefix"),suffix=rt_has_attr("suffix"),removed=0;while(*p){size_t remaining=strlen(p);int match=!strncmp(p,needle,nl)&&(!prefix||p==v.u.s)&&(!suffix||remaining==nl);if(match&&(!once||!removed)){p+=nl;removed=1;}else out[at++]=*p++;}out[at]=0;Value r=v_str(out);free(out);free(needle);if(t.kind){mut_store(e,&t,r);return v_null();}return r; }
    die("remove: unsupported value"); return v_null();
}
static Value b_repeat(Env*e,Value*a,int n){
    MutTarget t; Value v=mut_load(e,a[0],&t); long times=as_int(a[1]); if(times<0)times=0;
    if(v.k==V_STR){size_t len=strlen(v.u.s),total=checked_mul_size(len,(size_t)times,"repeat: result too large");char*s=(char*)xmalloc(checked_add_size(total,1,"repeat: result too large"));for(long i=0;i<times;i++)memcpy(s+(size_t)i*len,v.u.s,len);s[total]=0;Value r=v_str(s);free(s);if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
    if(v.k==V_BLOCK){Block*b=v.u.block.b;size_t total_size=checked_mul_size((size_t)b->n,(size_t)times,"repeat: result too large");if(total_size>INT_MAX)die("repeat: result too large");int old=b->n,total=(int)total_size;Value**items=(Value**)xmalloc((size_t)(total?total:1)*sizeof(Value*));for(int i=0;i<total;i++){items[i]=(Value*)xmalloc(sizeof(Value));*items[i]=*b->items[i%old];}Value r=v_block(items,total);if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
    if((unsigned long)times>(unsigned long)INT_MAX)die("repeat: result too large");
    {Value**items=xmalloc((size_t)(times?times:1)*sizeof(Value*));for(int i=0;i<times;i++){items[i]=xmalloc(sizeof(Value));*items[i]=clone_value(v);}Value r=v_block(items,(int)times);if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
}
static Value b_rotate(Env*e,Value*a,int n){
    MutTarget t; Value v=mut_load(e,a[0],&t); long by=as_int(a[1]);if(rt_has_attr("left"))by=-by;
    if(v.k==V_BLOCK){Block*b=v.u.block.b;if(!b->n)return t.kind?v_null():v;int k=(int)(by%b->n);if(k<0)k+=b->n;Value**items=(Value**)xmalloc((size_t)b->n*sizeof(Value*));for(int i=0;i<b->n;i++)items[(i+k)%b->n]=b->items[i];Value r=v_block(items,b->n);if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
    if(v.k==V_STR){int len=(int)strlen(v.u.s);if(!len)return t.kind?v_null():v;int k=(int)(by%len);if(k<0)k+=len;char*s=(char*)xmalloc((size_t)len+1);for(int i=0;i<len;i++)s[(i+k)%len]=v.u.s[i];s[len]=0;Value r=v_str(s);free(s);if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
    die("rotate: unsupported value"); return v_null();
}
static Env *g_sort_env=NULL;static int g_sort_descending=0,g_sort_sensitive=0,g_sort_locale=0;
static void collation_key(const char*source,char*out,size_t cap){size_t used=0;for(size_t i=0;source[i]&&used+1<cap;i++){unsigned char c=(unsigned char)source[i];if(c==0xc3&&source[i+1]){unsigned char next=(unsigned char)source[++i];if(next==0xa1||next==0x81)c='a';else if(next==0xa9||next==0x89)c='e';else if(next==0xad||next==0x8d)c='i';else if(next==0xb3||next==0x93)c='o';else if(next==0xba||next==0x9a)c='u';else c=next;}out[used++]=(char)tolower(c);}out[used]=0;}
static int sort_order(Value left,Value right){if(IS_STRLIKE(left.k)&&IS_STRLIKE(right.k)){int result;if(g_sort_locale){char a[512],b[512];collation_key(left.u.s,a,sizeof a);collation_key(right.u.s,b,sizeof b);result=strcmp(a,b);}else if(!g_sort_sensitive)result=strcasecmp(left.u.s,right.u.s);else result=strcmp(left.u.s,right.u.s);return (result>0)-(result<0);}return order_values(left,right);}
static int value_ptr_cmp(const void*pa,const void*pb){Value*a=*(Value*const*)pa,*b=*(Value*const*)pb;int result;if(g_sort_env&&object_member_index(*a,"compare")>=0){Value compared=object_magic(g_sort_env,*a,"compare",b,1);result=(as_int(compared)>0)-(as_int(compared)<0);}else result=sort_order(*a,*b);return g_sort_descending?-result:result;}
/* Stable merge sort, replacing the insertion sorts these used to be — sorting
 * a 100k-element block was quadratic. Merging takes from the left run whenever
 * the left element does not compare greater, which is exactly the order the
 * insertion sort produced.
 *
 * A block whose elements carry a user-defined `compare` method keeps the
 * insertion sort: its comparator runs Arturo code, and merge sort would call
 * it a different number of times in a different order. */
static int sort_uses_object_compare(Block *b){
    if(!g_sort_env)return 0;
    for(int i=0;i<b->n;i++)if(object_member_index(*b->items[i],"compare")>=0)return 1;
    return 0;
}
static void sort_block_items(Block *b){
    int n=b->n;
    if(n<2)return;
    if(sort_uses_object_compare(b)){
        for(int i=1;i<n;i++){
            Value*item=b->items[i];int j=i-1;
            while(j>=0&&value_ptr_cmp(&b->items[j],&item)>0){b->items[j+1]=b->items[j];j--;}
            b->items[j+1]=item;
        }
        return;
    }
    Value **scratch=(Value**)xmalloc((size_t)n*sizeof(Value*));
    Value **src=b->items,**dst=scratch;
    for(int width=1;width<n;){
        for(int lo=0;lo<n;lo+=2*width){
            int mid=lo+width<n?lo+width:n,hi=lo+2*width<n?lo+2*width:n;
            int i=lo,j=mid,at=lo;
            while(i<mid&&j<hi)dst[at++]=value_ptr_cmp(&src[i],&src[j])>0?src[j++]:src[i++];
            while(i<mid)dst[at++]=src[i++];
            while(j<hi)dst[at++]=src[j++];
        }
        Value **swap=src;src=dst;dst=swap;
        if(width>n/2)break;width*=2;
    }
    if(src!=b->items)memcpy(b->items,src,(size_t)n*sizeof(Value*));
    free(scratch);
}
static void sort_dict_entries(Dict *d){
    int n=d->n;
    if(n<2)return;
    char **ks=(char**)xmalloc((size_t)n*sizeof(char*));
    Value *vs=(Value*)xmalloc((size_t)n*sizeof(Value));
    char **ksrc=d->keys,**kdst=ks;
    Value *vsrc=d->vals,*vdst=vs;
    for(int width=1;width<n;){
        for(int lo=0;lo<n;lo+=2*width){
            int mid=lo+width<n?lo+width:n,hi=lo+2*width<n?lo+2*width:n;
            int i=lo,j=mid,at=lo;
            while(i<mid&&j<hi){
                int comparison=strcasecmp(ksrc[i],ksrc[j]);
                int takeRight=g_sort_descending?comparison<0:comparison>0;
                if(takeRight){kdst[at]=ksrc[j];vdst[at++]=vsrc[j++];}
                else {kdst[at]=ksrc[i];vdst[at++]=vsrc[i++];}
            }
            while(i<mid){kdst[at]=ksrc[i];vdst[at++]=vsrc[i++];}
            while(j<hi){kdst[at]=ksrc[j];vdst[at++]=vsrc[j++];}
        }
        char **kswap=ksrc;ksrc=kdst;kdst=kswap;
        Value *vswap=vsrc;vsrc=vdst;vdst=vswap;
        if(width>n/2)break;width*=2;
    }
    if(ksrc!=d->keys){memcpy(d->keys,ksrc,(size_t)n*sizeof(char*));memcpy(d->vals,vsrc,(size_t)n*sizeof(Value));}
    free(ks);free(vs);
    dict_reindex(d);
}
static Value b_sort(Env*e,Value*a,int n){
    (void)n;MutTarget t;Value source=mut_load(e,a[0],&t);Env *previousEnv=g_sort_env;int previousDescending=g_sort_descending,previousSensitive=g_sort_sensitive,previousLocale=g_sort_locale;g_sort_env=e;g_sort_descending=rt_has_attr("descending");g_sort_sensitive=rt_has_attr("sensitive");g_sort_locale=rt_has_attr("as");Value out;
    if(source.k==V_BLOCK){out=t.kind?source:clone_value(source);sort_block_items(out.u.block.b);}
    else if(source.k==V_DICT){out=t.kind?source:clone_value(source);sort_dict_entries(out.u.dict);}
    else die("sort: expected collection");g_sort_env=previousEnv;g_sort_descending=previousDescending;g_sort_sensitive=previousSensitive;g_sort_locale=previousLocale;if(t.kind){mut_store(e,&t,out);return v_null();}return out;
}
static Value b_squeeze(Env*e,Value*a,int n){
    MutTarget t;Value v=mut_load(e,a[0],&t);
    if(v.k==V_STR){size_t len=strlen(v.u.s),at=0;char*s=(char*)xmalloc(len+1);for(size_t i=0;i<len;i++)if(i==0||v.u.s[i]!=v.u.s[i-1])s[at++]=v.u.s[i];s[at]=0;Value r=v_str(s);free(s);if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
    if(v.k==V_BLOCK){Value r=t.kind?v:clone_value(v);Block*b=r.u.block.b;int at=0;for(int i=0;i<b->n;i++)if(i==0||!value_eq(*b->items[i],*b->items[i-1]))b->items[at++]=b->items[i];b->n=at;if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
    die("squeeze: unsupported value");return v_null();
}
static Value b_strip(Env*e,Value*a,int n){MutTarget t;Value v=mut_load(e,a[0],&t);if(v.k!=V_STR)die("strip: expected string");const char*s=v.u.s;while(*s&&isspace((unsigned char)*s))s++;size_t len=strlen(s);while(len&&isspace((unsigned char)s[len-1]))len--;char*out=(char*)xmalloc(len+1);memcpy(out,s,len);out[len]=0;Value r=v_str(out);free(out);mut_store(e,&t,r);return r;}
static Value b_unique(Env*e,Value*a,int n){
    MutTarget t;Value v=mut_load(e,a[0],&t);
    if(rt_has_attr("id")){static unsigned long serial=0;char suffix[64];snprintf(suffix,sizeof suffix,"%llx-%lx",(unsigned long long)time(NULL),++serial);char *prefix=val_str(v);size_t length=strlen(prefix)+strlen(suffix)+1;char *text=xmalloc(length);snprintf(text,length,"%s%s",prefix,suffix);Value result=v_str(text);free(text);free(prefix);return result;}
    if(v.k==V_BLOCK){Value r=t.kind?v:clone_value(v);Block*b=r.u.block.b;int at=0;for(int i=0;i<b->n;i++){int seen=0;for(int j=0;j<at;j++)if(value_eq(*b->items[j],*b->items[i])){seen=1;break;}if(!seen)b->items[at++]=b->items[i];}b->n=at;if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
    if(v.k==V_STR){size_t len=strlen(v.u.s),at=0;char*out=(char*)xmalloc(len+1);for(size_t i=0;i<len;i++){int seen=0;for(size_t j=0;j<at;j++)if(out[j]==v.u.s[i]){seen=1;break;}if(!seen)out[at++]=v.u.s[i];}out[at]=0;Value r=v_str(out);free(out);if(t.kind){mut_store(e,&t,r);return v_null();}return r;}
    die("unique: unsupported value");return v_null();
}

/* ---- reciprocal trigonometry and bit operations ------------------------- */
static Value b_sec(Env*e,Value*a,int n){return v_float(1.0/cos(as_float(a[0])));}
static Value b_sech(Env*e,Value*a,int n){return v_float(1.0/cosh(as_float(a[0])));}
static Value b_csec(Env*e,Value*a,int n){return v_float(1.0/sin(as_float(a[0])));}
static Value b_csech(Env*e,Value*a,int n){return v_float(1.0/sinh(as_float(a[0])));}
static Value b_ctan(Env*e,Value*a,int n){return v_float(1.0/tan(as_float(a[0])));}
static Value b_ctanh(Env*e,Value*a,int n){return v_float(1.0/tanh(as_float(a[0])));}
static Value b_asec(Env*e,Value*a,int n){return v_float(acos(1.0/as_float(a[0])));}
static Value b_asech(Env*e,Value*a,int n){return v_float(acosh(1.0/as_float(a[0])));}
static Value b_acsec(Env*e,Value*a,int n){return v_float(asin(1.0/as_float(a[0])));}
static Value b_acsech(Env*e,Value*a,int n){return v_float(asinh(1.0/as_float(a[0])));}
static Value b_actan(Env*e,Value*a,int n){return v_float(atan(1.0/as_float(a[0])));}
static Value b_actanh(Env*e,Value*a,int n){return v_float(atanh(1.0/as_float(a[0])));}
static Value b_atan2(Env*e,Value*a,int n){return v_float(atan2(as_float(a[0]),as_float(a[1])));}
static Value b_gamma(Env*e,Value*a,int n){return v_float(tgamma(as_float(a[0])));}
static Value bit_result(Env*e,Value receiver,long result){
    if(receiver.k==V_PATH){MutTarget t; (void)mut_load(e,receiver,&t);mut_store(e,&t,v_int(result));return v_null();}
    return v_int(result);
}
static Value b_bit_and(Env*e,Value*a,int n){return bit_result(e,a[0],as_int(a[0].k==V_PATH?mut_load(e,a[0],&(MutTarget){0}):a[0])&as_int(a[1]));}
static Value b_bit_or(Env*e,Value*a,int n){return bit_result(e,a[0],as_int(a[0].k==V_PATH?mut_load(e,a[0],&(MutTarget){0}):a[0])|as_int(a[1]));}
static Value b_bit_xor(Env*e,Value*a,int n){return bit_result(e,a[0],as_int(a[0].k==V_PATH?mut_load(e,a[0],&(MutTarget){0}):a[0])^as_int(a[1]));}
static Value b_bit_not(Env*e,Value*a,int n){return bit_result(e,a[0],~as_int(a[0].k==V_PATH?mut_load(e,a[0],&(MutTarget){0}):a[0]));}
static Value b_bit_nand(Env*e,Value*a,int n){return bit_result(e,a[0],~(as_int(a[0].k==V_PATH?mut_load(e,a[0],&(MutTarget){0}):a[0])&as_int(a[1])));}
static Value b_bit_nor(Env*e,Value*a,int n){return bit_result(e,a[0],~(as_int(a[0].k==V_PATH?mut_load(e,a[0],&(MutTarget){0}):a[0])|as_int(a[1])));}
static Value b_bit_xnor(Env*e,Value*a,int n){return bit_result(e,a[0],~(as_int(a[0].k==V_PATH?mut_load(e,a[0],&(MutTarget){0}):a[0])^as_int(a[1])));}
static Value b_shl(Env*e,Value*a,int n){return bit_result(e,a[0],as_int(a[0].k==V_PATH?mut_load(e,a[0],&(MutTarget){0}):a[0])<<as_int(a[1]));}
static Value b_shr(Env*e,Value*a,int n){return bit_result(e,a[0],as_int(a[0].k==V_PATH?mut_load(e,a[0],&(MutTarget){0}):a[0])>>as_int(a[1]));}
static Value b_nandp(Env*e,Value*a,int n){return v_bool(!(v_truthy(a[0])&&v_truthy(a[1])));}
static Value b_norp(Env*e,Value*a,int n){return v_bool(!(v_truthy(a[0])||v_truthy(a[1])));}
static Value b_xorp(Env*e,Value*a,int n){return v_bool(v_truthy(a[0])!=v_truthy(a[1]));}
static Value b_xnorp(Env*e,Value*a,int n){return v_bool(v_truthy(a[0])==v_truthy(a[1]));}

/* ---- comparison, membership, and string distance ------------------------ */
static Value b_coalesce(Env*e,Value*a,int n){return a[0].k==V_NULL?a[1]:a[0];}
static Value b_compare(Env*e,Value*a,int n){
    int cmp;
    if(object_member_index(a[0],"compare")>=0)return object_magic(e,a[0],"compare",&a[1],1);
    cmp=order_values(a[0],a[1]);
    return v_int(cmp);
}
static Value b_sortable_compare(Env*e,Value*a,int n){
    (void)n;if(a[0].k!=V_DICT||a[1].k!=V_DICT)die("sortable: expected objects");const char *field=key_text(a[2]);
    int left=dict_find(a[0],field),right=dict_find(a[1],field);if(left<0||right<0)die("sortable: missing field");
    Value pair[2]={a[0].u.dict->vals[left],a[1].u.dict->vals[right]};return b_compare(e,pair,2);
}
static Value b_sortable(Env*e,Value*a,int n){
    (void)n;const char *field=key_text(a[0]);
    IR **paramsItems=xmalloc(sizeof(IR*));paramsItems[0]=ir_load("that");IR *params=ir_block(paramsItems,1);
    IR **callArgs=xmalloc(3*sizeof(IR*));callArgs[0]=ir_load("this");callArgs[1]=ir_load("that");callArgs[2]=ir_const(v_str(field));
    IR **body=xmalloc(sizeof(IR*));body[0]=ir_call(ir_intrinsic("__sortableCompare"),callArgs,3);
    return v_func(params,body,1,e);
}
static Value b_inp(Env*e,Value*a,int n){
    if(rt_has_attr("deep")&&a[1].k==V_BLOCK){
        for(int i=0;i<a[1].u.block.b->n;i++){Value item=*a[1].u.block.b->items[i];if(value_eq(item,a[0]))return v_bool(1);if(item.k==V_BLOCK){Value nested[2]={a[0],item};if(v_truthy(b_inp(e,nested,2)))return v_bool(1);}}return v_bool(0);
    }
    if(rt_has_attr("deep")&&a[1].k==V_DICT){for(int i=0;i<a[1].u.dict->n;i++){Value item=a[1].u.dict->vals[i];if(value_eq(item,a[0]))return v_bool(1);if(item.k==V_BLOCK||item.k==V_DICT){Value nested[2]={a[0],item};if(v_truthy(b_inp(e,nested,2)))return v_bool(1);}}return v_bool(0);}
    long requested=rt_has_attr("at")?as_int(rt_attr_value("at",v_int(0))):-1,foundAt=-1;
    if(a[1].k==V_BLOCK){for(int i=0;i<a[1].u.block.b->n;i++)if(value_eq(*a[1].u.block.b->items[i],a[0])){foundAt=i;break;}}
    else if(a[1].k==V_STR){if(a[0].k==V_REGEX){regex_t rx;if(regcomp(&rx,a[0].u.s,REG_EXTENDED)==0){regmatch_t match;if(regexec(&rx,a[1].u.s,1,&match,0)==0)foundAt=match.rm_so;regfree(&rx);}}else{char*needle=val_str(a[0]);char *at=strstr(a[1].u.s,needle);if(at)foundAt=at-a[1].u.s;free(needle);}}
    else if(a[1].k==V_RANGE){int count=iterator_count(a[1]);for(int i=0;i<count;i++)if(value_eq(iterator_item(a[1],i),a[0])){foundAt=i;break;}}
    else if(a[1].k==V_DICT){for(int i=0;i<a[1].u.dict->n;i++)if(value_eq(a[1].u.dict->vals[i],a[0])){foundAt=i;break;}}
    else die("in?: unsupported collection");
    return v_bool(requested>=0?foundAt==requested:foundAt>=0);
}
static Value b_infinitep(Env*e,Value*a,int n){return v_bool(a[0].k==V_FLOAT&&isinf(a[0].u.f));}
static Value b_levenshtein(Env*e,Value*a,int n){
    const unsigned char*s=(unsigned char*)a[0].u.s,*t=(unsigned char*)a[1].u.s;size_t ns=strlen((char*)s),nt=strlen((char*)t);
    int*row=(int*)xmalloc((nt+1)*sizeof(int));for(size_t j=0;j<=nt;j++)row[j]=(int)j;
    for(size_t i=1;i<=ns;i++){int prev=row[0];row[0]=(int)i;for(size_t j=1;j<=nt;j++){int old=row[j],cost=s[i-1]==t[j-1]?0:1;int ins=row[j-1]+1,del=old+1,sub=prev+cost;row[j]=ins<del?(ins<sub?ins:sub):(del<sub?del:sub);prev=old;}}
    int result=row[nt];free(row);return v_int(result);
}
static Value b_jaro(Env*e,Value*a,int n){
    const char*s=a[0].u.s,*t=a[1].u.s;int ls=(int)strlen(s),lt=(int)strlen(t);if(!ls&&!lt)return v_float(1.0);if(!ls||!lt)return v_float(0.0);
    int range=(ls>lt?ls:lt)/2-1;if(range<0)range=0;unsigned char*sm=(unsigned char*)calloc((size_t)ls,1),*tm=(unsigned char*)calloc((size_t)lt,1);if(!sm||!tm)die("out of memory");
    int matches=0;for(int i=0;i<ls;i++){int lo=i-range;if(lo<0)lo=0;int hi=i+range+1;if(hi>lt)hi=lt;for(int j=lo;j<hi;j++)if(!tm[j]&&s[i]==t[j]){sm[i]=1;tm[j]=1;matches++;break;}}
    if(!matches){libc_free(sm);libc_free(tm);return v_float(0.0);}int trans=0,j=0;for(int i=0;i<ls;i++)if(sm[i]){while(!tm[j])j++;if(s[i]!=t[j])trans++;j++;}
    libc_free(sm);libc_free(tm);double m=matches;return v_float((m/ls+m/lt+(m-trans/2.0)/m)/3.0);
}
static int type_matches(Value spec,Value value){
    /* Arturo accepts a block in the signature for other predicate forms, but
     * it is not a union of type values (`is? [:integer :floating] 1.5` is
     * false). Only a scalar :type performs this direct kind check. */
    if(spec.k==V_BLOCK)return 0;
    const char *custom=object_type_name(value);if(custom)return spec.u.s&&!strcmp(spec.u.s,custom);
    const char*name=spec.u.s?spec.u.s:"";if(*name==':')name++;return !strcmp(name,type_name(value));
}
static Value b_isp(Env*e,Value*a,int n){return v_bool(type_matches(a[0],a[1]));}

static Value b_difference(Env*e,Value*a,int n){
    MutTarget t;Value left=mut_load(e,a[0],&t);if(left.k!=V_BLOCK||a[1].k!=V_BLOCK)die("difference: expected blocks");Block*x=left.u.block.b,*y=a[1].u.block.b;int at=0;for(int i=0;i<x->n;i++)if(!block_contains_value(y,*x->items[i]))x->items[at++]=x->items[i];x->n=at;return v_null();
}
static Value b_intersection(Env*e,Value*a,int n){
    MutTarget t;Value left=mut_load(e,a[0],&t);if(left.k!=V_BLOCK||a[1].k!=V_BLOCK)die("intersection: expected blocks");Block*x=left.u.block.b,*y=a[1].u.block.b;int at=0;for(int i=0;i<x->n;i++){int duplicate=0;for(int j=0;j<at;j++)if(value_eq(*x->items[j],*x->items[i])){duplicate=1;break;}if(!duplicate&&block_contains_value(y,*x->items[i]))x->items[at++]=x->items[i];}x->n=at;return v_null();
}
static Value b_union(Env*e,Value*a,int n){
    MutTarget t;Value left=mut_load(e,a[0],&t);if(left.k!=V_BLOCK||a[1].k!=V_BLOCK)die("union: expected blocks");Block*x=left.u.block.b,*y=a[1].u.block.b;for(int i=0;i<y->n;i++)if(!block_contains_value(x,*y->items[i]))block_append(left,*y->items[i]);return v_null();
}

/* `replace s from to` — replace all occurrences of `from` with `to`. */
static Value b_replace(Env*e,Value*a,int n){
    MutTarget target;Value source=mut_load(e,a[0],&target);
    if(a[1].k==V_BLOCK&&a[2].k==V_BLOCK){if(source.k!=V_STR||a[1].u.block.b->n!=a[2].u.block.b->n)die("replace: expected matching replacement blocks");Value current=source;for(int i=0;i<a[1].u.block.b->n;i++){Value args[3]={current,*a[1].u.block.b->items[i],*a[2].u.block.b->items[i]};current=b_replace(e,args,3);}if(target.kind){mut_store(e,&target,current);return v_null();}return current;}
    if(source.k!=V_STR||a[1].k!=V_STR||a[2].k!=V_STR)die("replace: expected strings");
    const char *s=source.u.s, *from=a[1].u.s, *to=a[2].u.s;
    if(!*from){Value result=v_str(s);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
    size_t cnt=0; for(const char*p=s;(p=strstr(p,from));p+=strlen(from)) cnt++;
    if(!cnt){Value result=v_str(s);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
    size_t fl=strlen(from), tl=strlen(to), sl=strlen(s);
    size_t replaced=checked_mul_size(cnt,fl,"replace: result too large");
    size_t inserted=checked_mul_size(cnt,tl,"replace: result too large");
    size_t out_length=checked_add_size(sl-replaced,inserted,"replace: result too large");
    char *out=xmalloc(checked_add_size(out_length,1,"replace: result too large"));
    char *o=out;
    const char *p;
    while((p=strstr(s,from))){
        size_t pre=p-s; memcpy(o,s,pre); o+=pre; memcpy(o,to,tl); o+=tl;
        s=p+fl;
    }
    strcpy(o,s);
    Value r=v_str(out);free(out);if(target.kind){mut_store(e,&target,r);return v_null();}return r;
}

/* `joinWith coll sep` — render coll's elements joined by sep. */
static Value b_joinWith(Env*e,Value*a,int n){
    Value coll=a[0]; const char *sep=(n>1 && a[1].k==V_STR)?a[1].u.s:"";
    if(coll.k==V_STR) return coll;
    if(coll.k!=V_BLOCK) die("joinWith: expected block");
    size_t total=1;
    for(int i=0;i<coll.u.block.b->n;i++){ char*s=val_str(*coll.u.block.b->items[i]); total+=strlen(s); free(s); if(i) total+=strlen(sep); }
    char *out=xmalloc(total+1); size_t len=0; out[0]=0;
    for(int i=0;i<coll.u.block.b->n;i++){
        char*s=val_str(*coll.u.block.b->items[i]);
        if(i){ strcpy(out+len,sep); len+=strlen(sep); }
        strcpy(out+len,s); len+=strlen(s); free(s);
    }
    out[len]=0; Value r=v_str(out); free(out); return r;
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
        Value **it=a[0].u.block.b->items; int m=a[0].u.block.b->n;
        for(int i=0;i<m;i++){ /* compare by value-str for simplicity */
            char *sv=val_str(*it[i]), *nv=val_str(a[1]);
            int eq=!strcmp(sv,nv); free(sv); free(nv);
            if(eq) return v_int(i);
        }
        return v_int(-1);
    }
    if(a[0].k==V_DICT){for(int i=0;i<a[0].u.dict->n;i++)if(value_eq(a[0].u.dict->vals[i],a[1]))return v_str(a[0].u.dict->keys[i]);return v_null();}
    if(a[0].k==V_RANGE){int count=iterator_count(a[0]);for(int i=0;i<count;i++)if(value_eq(iterator_item(a[0],i),a[1]))return v_int(i);return v_int(-1);}
    die("index"); return v_null();
}
/* `slice coll from to` — inclusive slice (Arturo semantics). */
static Value b_slice(Env*e,Value*a,int n){
    MutTarget target;Value source=mut_load(e,a[0],&target);
    long from=as_int(a[1]), to=as_int(a[2]);
    if(source.k==V_BLOCK){
        int m=source.u.block.b->n;
        if(from>=0 && to<=m-1){
            long cnt=to-from+1; if(cnt<0)cnt=0; if(cnt>m-from)cnt=m-from;
            Value **items=(Value**)xmalloc((size_t)(cnt?cnt:1)*sizeof(Value*));
            for(long i=0;i<cnt;i++) items[i]=source.u.block.b->items[from+i];
            Value result=v_block(items,(int)cnt);
            if(target.kind){mut_store(e,&target,result);return v_null();}return result;
        }
        Value result=v_block(NULL,0);
        if(target.kind){mut_store(e,&target,result);return v_null();}return result;
    }
    if(source.k==V_STR){
        int total=rune_count(source.u.s);
        if(from>=0 && to<=total){
            long len=to-from+1; if(len<0)len=0;
            int avail=total-(int)from; if(len>avail)len=avail; if(len<0)len=0;
            const char*start=rune_offset(source.u.s,(int)from);
            const char*p=start; int bytes=0;
            for(int k=0;k<(int)len;k++){bytes+=utf8_rune_len(p);p+=utf8_rune_len(p);}
            char *out=xmalloc((size_t)bytes+1); memcpy(out,start,(size_t)bytes); out[bytes]=0;
            Value r=v_str(out); free(out);
            if(target.kind){mut_store(e,&target,r);return v_null();}return r;
        }
        Value r=v_str("");
        if(target.kind){mut_store(e,&target,r);return v_null();}return r;
    }
    die("slice"); return v_null();
}
/* `split s sep` — split a string into a block of substrings. */
static Value b_split(Env*e,Value*a,int n){
    MutTarget target;Value source=mut_load(e,a[0],&target);long width=rt_has_attr("every")?as_int(rt_attr_value("every",v_int(1))):1;if(rt_has_attr("every")&&(width<=0||width>INT_MAX))die("split.every: expected positive width");if(source.k==V_BLOCK&&rt_has_attr("every")){int count=(source.u.block.b->n+(int)width-1)/(int)width;Value **groups=xmalloc((size_t)(count?count:1)*sizeof(Value*));for(int g=0;g<count;g++){int start=g*(int)width,items=source.u.block.b->n-start;if(items>width)items=(int)width;Value **part=xmalloc((size_t)(items?items:1)*sizeof(Value*));for(int i=0;i<items;i++){part[i]=xmalloc(sizeof(Value));*part[i]=*source.u.block.b->items[start+i];}groups[g]=xmalloc(sizeof(Value));*groups[g]=v_block(part,items);}Value result=v_block(groups,count);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}if(source.k==V_BLOCK&&!rt_has_attr("at"))return target.kind?v_null():source;if(source.k!=V_STR&&source.k!=V_BLOCK)die("split: expected string or block");const char *s=source.k==V_STR?source.u.s:"";
    if(rt_has_attr("at")){
        long cut=as_int(rt_attr_value("at",v_int(0)));if(cut<0)cut=0;
        Value **halves=xmalloc(2*sizeof(Value*));
        if(source.k==V_BLOCK){int total=source.u.block.b->n;if(cut>total)cut=total;for(int half=0;half<2;half++){int begin=half?(int)cut:0,end=half?total:(int)cut,count=end-begin;Value **part=xmalloc((size_t)(count?count:1)*sizeof(Value*));for(int i=0;i<count;i++)part[i]=source.u.block.b->items[begin+i];halves[half]=xmalloc(sizeof(Value));*halves[half]=v_block(part,count);}}
        else {size_t total=strlen(s);if((size_t)cut>total)cut=(long)total;for(int half=0;half<2;half++){size_t begin=half?(size_t)cut:0,end=half?total:(size_t)cut,length=end-begin;char *part=xmalloc(length+1);memcpy(part,s+begin,length);part[length]=0;halves[half]=xmalloc(sizeof(Value));*halves[half]=v_str(part);free(part);}}
        Value result=v_block(halves,2);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
    }
    if(rt_has_attr("path")){
        int capacity=8,count=0;Value **items=xmalloc((size_t)capacity*sizeof(Value*));const char *start=s,*cursor=s;
        for(;;cursor++)if(*cursor=='/'||*cursor=='\\'||!*cursor){size_t length=(size_t)(cursor-start);if(length){char *part=xmalloc(length+1);memcpy(part,start,length);part[length]=0;if(count==capacity){capacity*=2;items=xrealloc(items,(size_t)capacity*sizeof(Value*));}items[count]=xmalloc(sizeof(Value));*items[count++]=v_str(part);free(part);}if(!*cursor)break;start=cursor+1;}
        Value result=v_block(items,count);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
    }
    if(rt_has_attr("every")&&!rt_has_attr("words")&&!rt_has_attr("lines")){int length=(int)strlen(s),count=(length+(int)width-1)/(int)width;Value **items=xmalloc((size_t)(count?count:1)*sizeof(Value*));for(int i=0;i<count;i++){int start=i*(int)width,size=length-start;if(size>width)size=(int)width;char*part=xmalloc((size_t)size+1);memcpy(part,s+start,(size_t)size);part[size]=0;items[i]=xmalloc(sizeof(Value));*items[i]=v_str(part);free(part);}Value result=v_block(items,count);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
    if(rt_has_attr("by")){
        Value delimiters=rt_attr_value("by",v_str(""));int capacity=8,count=0;Value **items=xmalloc((size_t)capacity*sizeof(Value*));const char *start=s,*cursor=s;
        while(*cursor){size_t matched=0;
            int choices=delimiters.k==V_BLOCK?delimiters.u.block.b->n:1;
            for(int choice=0;choice<choices;choice++){
                Value delimiter=delimiters.k==V_BLOCK?*delimiters.u.block.b->items[choice]:delimiters;size_t length=0;
                if(delimiter.k==V_REGEX){regex_t rx;if(regcomp(&rx,delimiter.u.s,REG_EXTENDED)==0){regmatch_t match;if(regexec(&rx,cursor,1,&match,0)==0&&match.rm_so==0&&match.rm_eo>0)length=(size_t)match.rm_eo;regfree(&rx);}}
                else if(delimiter.k==V_CHAR){if(!strncmp(cursor,delimiter.u.c,strlen(delimiter.u.c)))length=strlen(delimiter.u.c);}
                else {char *text=val_str(delimiter);size_t candidate=strlen(text);if(candidate&&strncmp(cursor,text,candidate)==0)length=candidate;free(text);}
                if(length>matched)matched=length;
            }
            if(!matched){cursor++;continue;}
            size_t length=(size_t)(cursor-start);if(length){char *part=xmalloc(length+1);memcpy(part,start,length);part[length]=0;if(count==capacity){capacity*=2;items=xrealloc(items,(size_t)capacity*sizeof(Value*));}items[count]=xmalloc(sizeof(Value));*items[count++]=v_str(part);free(part);}cursor+=matched;start=cursor;
        }
        if(cursor>start){size_t length=(size_t)(cursor-start);char *part=xmalloc(length+1);memcpy(part,start,length);part[length]=0;if(count==capacity){capacity*=2;items=xrealloc(items,(size_t)capacity*sizeof(Value*));}items[count]=xmalloc(sizeof(Value));*items[count++]=v_str(part);free(part);}
        Value result=v_block(items,count);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
    }
    const char *sep=rt_has_attr("words")?" ":rt_has_attr("lines")?"\n":"";
    if(!*sep){ /* split into chars */
        int m=(int)strlen(s); Value **items=(Value**)xmalloc((m+1)*sizeof(Value*));
        for(int i=0;i<m;i++){ char c[2]={s[i],0}; items[i]=(Value*)xmalloc(sizeof(Value)); items[i][0]=v_str(c); }
        Value result=v_block(items,m);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
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
    Value result=v_block(items,m);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
}
/* `take coll n` / `drop coll n` */
static Value b_take(Env*e,Value*a,int n){
    MutTarget target;Value source=mut_load(e,a[0],&target);long c=as_int(a[1]);
    if(source.k==V_BLOCK){
        int m=source.u.block.b->n,start=0,count;if(c>=0){if(c>m)c=m;count=(int)c;}else{long wanted=-c;if(wanted>m)wanted=m;start=m-(int)wanted;count=(int)wanted;}
        Value **items=(Value**)xmalloc((size_t)(count?count:1)*sizeof(Value*));
        for(int i=0;i<count;i++) items[i]=source.u.block.b->items[start+i];
        Value result=v_block(items,count);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
    }
    if(source.k==V_STR){
        const char*s=source.u.s; long m=(long)strlen(s),start=0,count;if(c>=0){if(c>m)c=m;count=c;}else{long wanted=-c;if(wanted>m)wanted=m;start=m-wanted;count=wanted;}
        char *out=xmalloc((size_t)count+1); memcpy(out,s+start,(size_t)count); out[count]=0;
        Value result=v_str(out); free(out);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
    }
    if(source.k==V_RANGE){int m=iterator_count(source),start=0,count;if(c>=0){if(c>m)c=m;count=(int)c;}else{long wanted=-c;if(wanted>m)wanted=m;start=m-(int)wanted;count=(int)wanted;}Value **items=xmalloc((size_t)(count?count:1)*sizeof(Value*));for(int i=0;i<count;i++){items[i]=xmalloc(sizeof(Value));*items[i]=iterator_item(source,start+i);}Value result=v_block(items,count);if(target.kind){mut_store(e,&target,result);return v_null();}return result;}
    die("take: expected string, block, or range"); return v_null();
}
static Value b_drop(Env*e,Value*a,int n){
    MutTarget target;Value source=mut_load(e,a[0],&target);long c=as_int(rt_attr_value("times",v_int(1)));
    if(source.k==V_BLOCK){
        int m=source.u.block.b->n,start=0,count=m;if(c>=0){if(c>m)c=m;start=(int)c;count=m-start;}else{long remove=-c;if(remove>m)remove=m;count=m-(int)remove;}
        Value **items=(Value**)xmalloc((count+1)*sizeof(Value*));for(int i=0;i<count;i++)items[i]=source.u.block.b->items[start+i];Value result=v_block(items,count);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
    }
    if(source.k==V_STR){
        const char*s=source.u.s;long m=(long)strlen(s),start=0,count=m;if(c>=0){if(c>m)c=m;start=c;count=m-c;}else{long remove=-c;if(remove>m)remove=m;count=m-remove;}
        char*out=xmalloc((size_t)count+1);memcpy(out,s+start,(size_t)count);out[count]=0;Value result=v_str(out);free(out);if(target.kind){mut_store(e,&target,result);return v_null();}return result;
    }
    die("drop"); return v_null();
}
static Value b_reverse(Env*e,Value*a,int n){
    MutTarget target;Value source=mut_load(e,a[0],&target);
    if(source.k==V_STR){
        const char*s=source.u.s; long m=(long)strlen(s);
        char *out=xmalloc(m+1); for(long i=0;i<m;i++) out[i]=s[m-1-i]; out[m]=0;
        Value r=v_str(out); free(out);if(target.kind){mut_store(e,&target,r);return v_null();}return r;
    }
    if(source.k==V_BLOCK){
        int m=source.u.block.b->n; Value **items=(Value**)xmalloc((m+1)*sizeof(Value*));
        for(int i=0;i<m;i++) items[i]=source.u.block.b->items[m-1-i];Value r=v_block(items,m);if(target.kind){mut_store(e,&target,r);return v_null();}return r;
    }
    if(source.k==V_RANGE){Value r=v_range(source.u.range.hi,source.u.range.lo);r.u.range.step=-source.u.range.step;r.u.range.character=source.u.range.character;r.u.range.infinite=source.u.range.infinite;if(target.kind){mut_store(e,&target,r);return v_null();}return r;}
    die("reverse"); return v_null();
}
/* `ensure pred msg` — assert; die on failure. */
static Value b_ensure(Env*e,Value*a,int n){
    Value condition=a[0];
    if(condition.k==V_BLOCK)condition=evalSeq(e,condition.u.block.b->items,condition.u.block.b->n);
    if(!v_truthy(condition))die("assertion failed");
    return v_null();
}
/* `array coll` — block to an array (block-like here). */
static Value b_array(Env*e,Value*a,int n){
    Value v=a[0];
    if(v.k==V_BLOCK) return v;
    if(v.k==V_STR){FILE*f=fopen(v.u.s,"rb");if(!f){char msg[512];snprintf(msg,sizeof msg,"array: file not found '%s'",v.u.s);die(msg);}fclose(f);}
    return one_elt(v);
}
static Value b_dictionary_value(Env*e,Value*a,int n){
    (void)e;(void)n;Value v=a[0];if(v.k==V_DICT)return v;
    if(v.k==V_STR){FILE*f=fopen(v.u.s,"rb");if(!f){char msg[512];snprintf(msg,sizeof msg,"dictionary: file not found '%s'",v.u.s);die(msg);}fclose(f);}
    die("dictionary: expected dictionary source");return v_null();
}
/* `case key [match1 -> result1, match2 -> result2, ...]` — the arms form a
 * FLAT block: each arm is `match "->" resultExpr...`, where resultExpr is the
 * tokens from after "->" until the next arm starts. The emitter wraps
 * multi-token results in a nested block, so a top-level token that is a
 * ":type" word or "else" begins a new arm (results never contain those). */
static Value b_case(Env*e,Value*a,int n){
    Value key=a[0]; Value pairs=a[1];
    if(pairs.k!=V_BLOCK) die("case: expected arms block");
    Value **it=pairs.u.block.b->items; int pn=pairs.u.block.b->n;
    /* An arm's result runs until the next MATCH token. A match is a string
     * immediately followed by `->` (`:integer -> ...` in cValue, but cNode's
     * `case op [ "load" -> ... ]` uses PLAIN words as matches — the old scan
     * only stopped at ":"-prefixed/else tokens, so a plain-word arm's result
     * swallowed every later arm and `case` fell off the end returning null,
     * which made cNode emit "null" for every corpus statement). */
    int i=0;
    while(i<pn){
        if(!(i+1<pn && IS_STRLIKE(it[i+1]->k) && !strcmp(it[i+1]->u.s,"->"))){ i++; continue; }
        Value mv=*it[i];
        int rs=i+2;               /* result start */
        int re=rs;                /* result end (exclusive) */
        while(re<pn){
            Value t=*it[re];
            int armStart = (IS_STRLIKE(t.k) && (t.u.s[0]==':' || !strcmp(t.u.s,"else") || !strcmp(t.u.s,"any")))
                        || (re+1<pn && IS_STRLIKE(it[re+1]->k) && !strcmp(it[re+1]->u.s,"->"));
            if(armStart) break;
            re++;
        }
        int hit;
        if(IS_STRLIKE(mv.k) && (!strcmp(mv.u.s,"else")||!strcmp(mv.u.s,"any"))) hit=1;
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
static Value b_directoryp(Env*e,Value*a,int n){
    (void)e;(void)n;
    struct stat st;
    return v_bool(a[0].k==V_STR && stat(a[0].u.s,&st)==0 && S_ISDIR(st.st_mode));
}
static Value b_existsp(Env*e,Value*a,int n){
    (void)e;(void)n; struct stat st;
    return v_bool(a[0].k==V_STR && lstat(a[0].u.s,&st)==0);
}
static Value b_filep(Env*e,Value*a,int n){
    (void)e;(void)n; struct stat st;
    return v_bool(a[0].k==V_STR && stat(a[0].u.s,&st)==0 && S_ISREG(st.st_mode));
}
static Value b_symlinkp(Env*e,Value*a,int n){
    (void)e;(void)n; struct stat st;
    return v_bool(a[0].k==V_STR && lstat(a[0].u.s,&st)==0 && S_ISLNK(st.st_mode));
}
static Value b_hiddenp(Env*e,Value*a,int n){
    (void)e;(void)n;
    if(a[0].k!=V_STR)return v_bool(0);
    const char *base=strrchr(a[0].u.s,'/'); base=base?base+1:a[0].u.s;
    return v_bool(base[0]=='.' && base[1]!='\0');
}
static Value b_absolutep(Env*e,Value*a,int n){
    (void)e;(void)n; return v_bool(a[0].k==V_STR && a[0].u.s[0]=='/');
}
static Value b_copy_file(Env*e,Value*a,int n){
    (void)e;(void)n;
    if(a[0].k!=V_STR||a[1].k!=V_STR)return v_null();
    FILE *in=fopen(a[0].u.s,"rb"); if(!in)return v_null();
    FILE *out=fopen(a[1].u.s,"wb"); if(!out){fclose(in);return v_null();}
    char buf[8192]; size_t got;
    while((got=fread(buf,1,sizeof buf,in))>0)fwrite(buf,1,got,out);
    fclose(out);fclose(in);return v_null();
}
static Value b_delete_file(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k==V_STR)(void)unlink(a[0].u.s);return v_null();
}
static Value b_move_file(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k==V_STR&&a[1].k==V_STR)(void)rename(a[0].u.s,a[1].u.s);return v_null();
}
static Value b_symlink_file(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k==V_STR&&a[1].k==V_STR)(void)symlink(a[0].u.s,a[1].u.s);return v_null();
}
static int compile_regex(Value pattern,regex_t *compiled){
    const char *text=(pattern.k==V_REGEX||pattern.k==V_STR)?pattern.u.s:"";
    return regcomp(compiled,text,REG_EXTENDED);
}
static Value b_matchp(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_STR)return v_bool(0);regex_t rx;if(compile_regex(a[1],&rx)!=0)return v_bool(0);
    regmatch_t m;int ok=regexec(&rx,a[0].u.s,1,&m,0)==0;regfree(&rx);return v_bool(ok);
}
static Value b_match(Env*e,Value*a,int n){
    (void)e;(void)n;if(a[0].k!=V_STR)return v_block(NULL,0);regex_t rx;if(compile_regex(a[1],&rx)!=0)return v_block(NULL,0);
    const char *cursor=a[0].u.s;int count=0,cap=4;Value **items=(Value**)xmalloc((size_t)cap*sizeof(Value*));regmatch_t m;
    while(regexec(&rx,cursor,1,&m,0)==0){
        if(count==cap){cap*=2;items=(Value**)xrealloc(items,(size_t)cap*sizeof(Value*));}
        size_t len=(size_t)(m.rm_eo-m.rm_so);char *s=(char*)xmalloc(len+1);memcpy(s,cursor+m.rm_so,len);s[len]=0;
        items[count]=(Value*)xmalloc(sizeof(Value));*items[count++]=v_str(s);free(s);
        if(m.rm_eo==0){if(!*cursor)break;cursor++;}else cursor+=m.rm_eo;
    }
    regfree(&rx);return v_block(items,count);
}
static Value b_new(Env*e,Value*a,int n){(void)e;(void)n;return clone_value(a[0]);}
static Value b_setp(Env*e,Value*a,int n){
    (void)n;
    const char *name=(a[0].k==V_STR||a[0].k==V_LITERAL||a[0].k==V_WORD)?a[0].u.s:"";
    return v_bool(e && env_bound(e,name));
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
    buf[len]=0;int raw=pclose(p),code=WIFEXITED(raw)?WEXITSTATUS(raw):raw;
    if(rt_has_attr("code")){
        if(rt_has_attr("directly")){free(buf);return v_int(code);}
        char **keys=xmalloc(2*sizeof(char*));Value *vals=xmalloc(2*sizeof(Value));keys[0]=strdup("output");keys[1]=strdup("code");vals[0]=v_str(buf);vals[1]=v_int(code);free(buf);return v_dict(keys,vals,2);
    }
    Value r=v_str(buf); free(buf); return r;
}
/* `args` — the command-line arguments as a block of strings. The generated
 * main() calls runtime_set_args() before running the program. */
static int  g_argc = 0;
static char **g_argv = NULL;
void runtime_set_args(int argc, char **argv){ g_argc = argc; g_argv = argv; }
void runtime_set_source(const char *path){g_source_path=path;}

static Value dict_from_pairs(const char **names,Value *vals,int n){char **keys=xmalloc((size_t)n*sizeof(char*));Value *out=xmalloc((size_t)n*sizeof(Value));for(int i=0;i<n;i++){keys[i]=strdup(names[i]);out[i]=vals[i];}return v_dict(keys,out,n);}
static Value b_prints(Env*e,Value*a,int n){(void)e;(void)n;print_scalar(a[0]);fflush(stdout);return v_null();}
static Value b_input(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k==V_STR){fputs(a[0].u.s,stdout);fflush(stdout);}char buf[4096];if(!fgets(buf,sizeof buf,stdin))return v_str("");size_t z=strlen(buf);while(z&&(buf[z-1]=='\n'||buf[z-1]=='\r'))buf[--z]=0;return v_str(buf);}
static Value b_pause(Env*e,Value*a,int n){(void)e;(void)n;double ms=a[0].k==V_QUANTITY?quantity_convert_amount(a[0],"ms"):as_float(a[0]);if(ms>0)usleep((useconds_t)(ms*1000.0));return v_null();}
static Value b_exit0(Env*e,Value*a,int n){(void)e;(void)a;(void)n;exit(0);return v_null();}
static Value b_clear(Env*e,Value*a,int n){(void)e;(void)a;(void)n;fputs("\033[2J\033[H",stdout);fflush(stdout);return v_null();}
static Value b_env(Env*e,Value*a,int n){(void)e;(void)a;(void)n;int count=0;while(environ&&environ[count])count++;char **keys=xmalloc((size_t)count*sizeof(char*));Value *vals=xmalloc((size_t)count*sizeof(Value));int used=0;for(int i=0;i<count;i++){char *eq=strchr(environ[i],'=');if(!eq)continue;size_t z=(size_t)(eq-environ[i]);keys[used]=xmalloc(z+1);memcpy(keys[used],environ[i],z);keys[used][z]=0;vals[used]=v_str(eq+1);used++;}return v_dict(keys,vals,used);}
static Value b_path_info(Env*e,Value*a,int n){(void)e;(void)a;(void)n;char cwd[PATH_MAX];if(!getcwd(cwd,sizeof cwd))strcpy(cwd,".");const char *home=getenv("HOME");const char *temp=getenv("TMPDIR");Value vals[]={v_str(cwd),v_str(home?home:""),v_str(temp?temp:"/tmp")};const char *keys[]={"current","home","temp"};return dict_from_pairs(keys,vals,3);}
static Value path_join_cwd(Value v){if(v.k!=V_STR)die("path: expected string");if(v.u.s[0]=='/')return v_str(v.u.s);char cwd[PATH_MAX],out[PATH_MAX];if(!getcwd(cwd,sizeof cwd))strcpy(cwd,".");snprintf(out,sizeof out,"%s/%s",cwd,v.u.s);char normalized[PATH_MAX];if(realpath(out,normalized))return v_str(normalized);return v_str(out);}
static Value b_absolute_path(Env*e,Value*a,int n){(void)e;(void)n;return path_join_cwd(a[0]);}
static Value b_relative_path(Env*e,Value*a,int n){(void)e;(void)n;return path_join_cwd(a[0]);}
static Value normalize_value(Value source){
    if(source.k!=V_STR)die("normalize: expected string");const char *input=source.u.s;char *expanded=NULL;
    if(rt_has_attr("tilde")&&input[0]=='~'&&(input[1]=='/'||input[1]==0)){const char *home=getenv("HOME");size_t z=strlen(home?home:"")+strlen(input)+1;expanded=xmalloc(z);snprintf(expanded,z,"%s%s",home?home:"",input+1);input=expanded;}
    int absolute=input[0]=='/';char *copy=strdup(input),*parts[PATH_MAX/2];int used=0;char *save=NULL;
    for(char *p=strtok_r(copy,"/",&save);p;p=strtok_r(NULL,"/",&save)){if(!strcmp(p,".")||!*p)continue;if(!strcmp(p,"..")){if(used>0&&strcmp(parts[used-1],".."))used--;else if(!absolute)parts[used++]=p;}else parts[used++]=p;}
    size_t cap=strlen(input)+4;char *out=xmalloc(cap);size_t at=0;if(absolute)out[at++]='/';for(int i=0;i<used;i++){if(at&&out[at-1]!='/')out[at++]='/';size_t z=strlen(parts[i]);memcpy(out+at,parts[i],z);at+=z;}if(at==0)out[at++]='.';out[at]=0;
    if(rt_has_attr("executable")&&!absolute&&!strchr(out,'/')&&strcmp(out,".")){size_t z=strlen(out)+3;char *pref=xmalloc(z);snprintf(pref,z,"./%s",out);free(out);out=pref;}
    Value result=v_str(out);free(out);free(copy);free(expanded);return result;
}
typedef struct {Value **items;int n,cap;} ValueList;
static void value_list_add(ValueList *v,const char *s){if(v->n>=v->cap){v->cap=v->cap?v->cap*2:16;v->items=xrealloc(v->items,(size_t)v->cap*sizeof(Value*));}v->items[v->n]=xmalloc(sizeof(Value));*v->items[v->n++]=v_str(s);}
static void list_walk(ValueList *out,const char *root,const char *relativeBase,int recursive,int relative){DIR *d=opendir(root);if(!d)return;struct dirent *ent;while((ent=readdir(d))){if(!strcmp(ent->d_name,".")||!strcmp(ent->d_name,".."))continue;char full[PATH_MAX];snprintf(full,sizeof full,"%s%s%s",root,!strcmp(root,"/")?"":"/",ent->d_name);const char *shown=relative?full+strlen(relativeBase)+(full[strlen(relativeBase)]=='/'):full;value_list_add(out,shown);if(recursive){struct stat st;if(!lstat(full,&st)&&S_ISDIR(st.st_mode)&&!S_ISLNK(st.st_mode))list_walk(out,full,relativeBase,1,relative);}}closedir(d);}
static Value b_list(Env*e,Value*a,int n){(void)e;(void)n;if(a[0].k!=V_STR)die("list: expected string");ValueList out={0};list_walk(&out,a[0].u.s,a[0].u.s,rt_has_attr("recursive"),rt_has_attr("relative"));return v_block(out.items,out.n);}
static Value b_process_info(Env*e,Value*a,int n){(void)e;(void)a;(void)n;Value vals[]={v_int((long)getpid())};const char *keys[]={"id"};return dict_from_pairs(keys,vals,1);}
static Value b_script_info(Env*e,Value*a,int n){(void)e;(void)a;(void)n;const char *file=(g_argc>0&&g_argv)?g_argv[0]:"";Value vals[]={v_str(file)};const char *keys[]={"file"};return dict_from_pairs(keys,vals,1);}
static Value b_sys_info(Env*e,Value*a,int n){
    (void)e;(void)a;(void)n;struct utsname u;memset(&u,0,sizeof u);uname(&u);
    const char *os=!strcmp(u.sysname,"Darwin")?"macos":!strcmp(u.sysname,"Linux")?"linux":u.sysname;
    const char *arch=!strcmp(u.machine,"x86_64")?"amd64":u.machine;uint16_t endianProbe=1;const char *endian=*(unsigned char*)&endianProbe?"little":"big";
    Value cpuVals[]={v_token(V_LITERAL,arch),v_token(V_LITERAL,endian)};const char *cpuKeys[]={"arch","endian"};Value cpu=dict_from_pairs(cpuKeys,cpuVals,2);
    Value deps=v_dict(NULL,NULL,0);Value vals[]={v_str("Arturo Programming Language"),v_str("Copyright Arturo contributors"),v_version("0.10.0"),v_date_iso("2026-01-01T00:00:00+00:00"),deps,v_str("arturo-native"),v_token(V_LITERAL,"standalone"),cpu,v_token(V_LITERAL,os)};
    const char *keys[]={"author","copyright","version","built","deps","binary","release","cpu","os"};return dict_from_pairs(keys,vals,9);
}
static Value b_wait(Env*e,Value*a,int n){(void)n;if(a[0].k==V_BLOCK)return runBlockValue(e,a[0]);return a[0];}

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

static Value b_arg(Env*e,Value*a,int n){
    (void)e;(void)a;(void)n;
    int c=g_argc>0?g_argc-1:0;
    Value **items=(Value**)xmalloc((size_t)(c+1)*sizeof(Value*));
    for(int i=0;i<c;i++){items[i]=(Value*)xmalloc(sizeof(Value));*items[i]=v_str(g_argv[i+1]);}
    return v_block(items,c);
}

static struct { const char *name; Value (*fn)(Env*,Value*,int); } BUILTINS[] = {
    {"print",b_print},{"add",b_add},{"sub",b_sub},{"mul",b_mul},{"div",b_div},
    {"fdiv",b_fdiv},{"mod",b_mod},{"divmod",b_divmod},{"pow",b_pow},{"neg",b_neg},{"inc",b_inc},
    {"numerator",b_numerator},{"denominator",b_denominator},
    {"angle",b_angle},{"reciprocal",b_reciprocal},
    {"dec",b_dec},{"equal?",b_equal},{"notEqual?",b_notEqual},{"greater?",b_greater},{"less?",b_less},
    {"and?",b_and},{"or?",b_or},{"not?",b_not},{"size",b_size},{"first",b_first},
    {"last",b_last},{"append",b_append},{"pop",b_pop},{"makeDict",b_makeDict},{"set",b_set},
    {"get",b_get},{"pathGet",b_path_get},{"empty?",b_empty},{"call",b_call},{"type",b_type},
    {"attr",b_attr},{"attr?",b_attrp},{"attrs",b_attrs},
    {"concat",b_concat},{"range",b_range},{"key?",b_key},
    {"map",b_map},{"select",b_select},{"filter",b_filter},{"fold",b_fold},{"every?",b_every},{"some?",b_some},{"collect",b_collect},
    {"chunk",b_chunk},{"cluster",b_cluster},{"gather",b_gather},{"arrange",b_arrange},
    {"enumerate",b_enumerate},{"maximum",b_maximum},{"minimum",b_minimum},{"loop",b_loop},
    {"to",b_to},{"replace",b_replace},{"joinWith",b_joinWith},
    {"convert",b_convert},{"in",b_in},{"units",b_units},{"scalar",b_scalar},
    {"lower",b_lower},{"upper",b_upper},{"index",b_index},{"slice",b_slice},
    {"split",b_split},{"take",b_take},{"drop",b_drop},{"reverse",b_reverse},
    {"ensure",b_ensure},{"array",b_array},{"dictionary",b_dictionary_value},{"case",b_case},
    {"read",b_read},{"write",b_write},{"execute",b_execute},{"arg",b_arg},{"args",b_args},
    {"prints",b_prints},{"input",b_input},{"pause",b_pause},{"wait",b_wait},{"exit",b_exit0},{"clear",b_clear},
    {"env",b_env},{"path",b_path_info},{"process",b_process_info},{"script",b_script_info},{"sys",b_sys_info},
    {"absolute",b_absolute_path},{"relative",b_relative_path},{"normalize",b_normalize},{"list",b_list},
    {"timestamp",b_timestamp},
    {"directory?",b_directoryp},
    {"exists?",b_existsp},{"file?",b_filep},{"hidden?",b_hiddenp},{"symlink?",b_symlinkp},
    {"absolute?",b_absolutep},{"copy",b_copy_file},{"delete",b_delete_file},
    {"move",b_move_file},{"rename",b_move_file},{"symlink",b_symlink_file},
    {"match",b_match},{"match?",b_matchp},
    {"crc",b_crc},{"encode",b_encode},{"decode",b_decode},{"color",b_color},{"escape",b_escape},{"unescape",b_unescape},{"wordwrap",b_wordwrap},{"arity",b_arity},{"symbols",b_symbols},{"var",b_var},{"unset",b_unset},{"property",b_property},{"conforms?",b_conformsp},{"parse",b_parse},{"translate",b_translate},{"extract",b_extract},{"render",b_render},{"express",b_express},{"inspect",b_inspect},{"benchmark",b_benchmark},{"export",b_export},{"alphabet",b_alphabet},{"digest",b_digest},{"hash",b_hash},{"with",b_with},{"specify",b_specify},{"alias",b_alias},
    {"blend",b_blend},{"darken",b_darken},{"lighten",b_lighten},{"saturate",b_saturate},{"desaturate",b_desaturate},{"grayscale",b_grayscale},{"invert",b_invert},{"palette",b_palette},
    {"new",b_new},{"set?",b_setp},
    {"insert",b_insert},{"break",b_break},{"continue",b_continue},{"do",b_do},{"null?",b_isNull},{"contains?",b_contains},
    {"greaterOrEqual?",b_ge},{"lessOrEqual?",b_le},{"join",b_join},{"try",b_try},
    {"throw",b_throw},{"throws?",b_throwsp},{"error?",b_errorp},
    {"panic",b_panic},{"defined?",b_definedp},
    {"abs",b_abs},{"ceil",b_ceil},{"floor",b_floor},{"even?",b_even},{"odd?",b_odd},
    {"positive?",b_positive},{"negative?",b_negative},{"zero?",b_zero},
    {"leap?",b_leapp},
    {"round",b_round},{"sqrt",b_sqrt},{"ln",b_ln},{"log",b_log},
    {"sin",b_sin},{"cos",b_cos},{"tan",b_tan},
    {"asin",b_asin},{"acos",b_acos},{"atan",b_atan},
    {"sinh",b_sinh},{"cosh",b_cosh},{"tanh",b_tanh},
    {"asinh",b_asinh},{"acosh",b_acosh},{"atanh",b_atanh},{"gcd",b_gcd},
    {"min",b_min},{"max",b_max},{"sum",b_sum},{"product",b_product},
    {"keys",b_keys},{"values",b_values},{"methods",b_methods},{"block?",b_blockp},{"dictionary?",b_dictp},
    {"integer?",b_intp},{"floating?",b_floatp},{"rational?",b_rationalp},{"complex?",b_complexp},{"quantity?",b_quantityp},{"unit?",b_unitp},{"date?",b_datep},{"color?",b_colorp},{"binary?",b_binaryp},{"string?",b_stringp},{"logical?",b_logicalp},
    {"char?",b_charp},{"function?",b_functionp},{"object?",b_objectp},{"method?",b_methodp},{"range?",b_rangep},
    {"word?",b_wordp},{"label?",b_labelp},{"literal?",b_literalp},{"symbol?",b_symbolp},{"symbolLiteral?",b_symbolliteralp},
    {"type?",b_typep},{"version?",b_versionp},{"errorKind?",b_errorkindp},{"inline?",b_inlinep},{"path?",b_pathp},{"pathLabel?",b_pathlabelp},
    {"pathLiteral?",b_pathliteralp},{"regex?",b_regexp},{"attribute?",b_attributep},
    {"attributeLabel?",b_attributelabelp},{"true?",b_truep},{"false?",b_falsep},
    {"one?",b_onep},{"ascii?",b_asciip},{"whitespace?",b_whitespacep},
    {"lower?",b_lowerp},{"upper?",b_upperp},{"numeric?",b_numericp},
    {"prefix?",b_prefixp},{"suffix?",b_suffixp},{"between?",b_betweenp},{"same?",b_samep},
    {"all?",b_allp},{"any?",b_anyp},
    {"average",b_average},{"clamp",b_clamp},{"exp",b_exp},{"factorial",b_factorial},
    {"factors",b_factors},{"hypot",b_hypot},{"lcm",b_lcm},{"median",b_median},
    {"powmod",b_powmod},{"prime?",b_primep},{"variance",b_variance},{"deviation",b_deviation},
    {"skewness",b_skewness},{"kurtosis",b_kurtosis},
    {"digits",b_digits},{"couple",b_couple},{"decouple",b_decouple},{"sorted?",b_sortedp},{"tally",b_tally},
    {"combine",b_combine},{"permutate",b_permutate},{"powerset",b_powerset},
    {"disjoint?",b_disjointp},{"intersect?",b_intersectp},{"subset?",b_subsetp},{"superset?",b_supersetp},
    {"capitalize",b_capitalize},{"chop",b_chop},{"empty",b_empty_value},{"flatten",b_flatten},
    {"indent",b_indent},{"outdent",b_outdent},{"pad",b_pad},{"prepend",b_prepend},
    {"remove",b_remove},{"repeat",b_repeat},{"rotate",b_rotate},{"sort",b_sort},{"shuffle",b_shuffle},{"truncate",b_truncate},
    {"squeeze",b_squeeze},{"strip",b_strip},{"unique",b_unique},
    {"sec",b_sec},{"sech",b_sech},{"csec",b_csec},{"csech",b_csech},{"ctan",b_ctan},{"ctanh",b_ctanh},
    {"asec",b_asec},{"asech",b_asech},{"acsec",b_acsec},{"acsech",b_acsech},{"actan",b_actan},{"actanh",b_actanh},
    {"atan2",b_atan2},{"gamma",b_gamma},{"and",b_bit_and},{"or",b_bit_or},{"not",b_bit_not},
    {"nand",b_bit_nand},{"nor",b_bit_nor},{"xor",b_bit_xor},{"xnor",b_bit_xnor},{"shl",b_shl},{"shr",b_shr},
    {"nand?",b_nandp},{"nor?",b_norp},{"xor?",b_xorp},{"xnor?",b_xnorp},
    {"coalesce",b_coalesce},{"compare",b_compare},{"sortable",b_sortable},{"__sortableCompare",b_sortable_compare},{"in?",b_inp},{"infinite?",b_infinitep},
    {"discard",b_discard},{"dup",b_dup},{"extend",b_extend},{"random",b_random},{"sample",b_sample},{"sign",b_sign},
    {"conj",b_conj},
    {"now",b_now},{"after",b_after},{"before",b_before},{"past?",b_pastp},{"today?",b_todayp},
    {"sunday?",b_sundayp},{"monday?",b_mondayp},{"tuesday?",b_tuesdayp},{"wednesday?",b_wednesdayp},
    {"thursday?",b_thursdayp},{"friday?",b_fridayp},{"saturday?",b_saturdayp},
    {"levenshtein",b_levenshtein},{"jaro",b_jaro},{"is?",b_isp},
    {"difference",b_difference},{"intersection",b_intersection},{"union",b_union},
    {NULL,NULL}
};

static NameIndex g_builtin_index;
static const char *builtin_name_at(int i){return BUILTINS[i].name;}
static int builtin_index_of(const char *name){
    if(!g_builtin_index.built){int count=0;while(BUILTINS[count].name)count++;name_index_build(&g_builtin_index,count,builtin_name_at);}
    return name_index_find(&g_builtin_index,name);
}
int rt_builtin(const char *name, Env *e, Value *args, int n, Value *out) {
    int i=builtin_index_of(name);
    if(i<0)return 0;
    int expected=declared_arity_of(name);
    if(expected>=0&&n<expected){char msg[256];snprintf(msg,sizeof msg,"%s: not enough arguments (expected %d, got %d)",name,expected,n);die(msg);}
    *out = BUILTINS[i].fn(e,args,n); return 1;
}
/* zero-arity builtins: when a bare word names one and it is NOT bound as a
 * variable, the host CALLS it (`args`, `break`) rather than yielding a value. */
static int rt_zero_arity(const char *name){
    static const char *const z[] = {"arg","args","arity","attrs","symbols","break","continue","now","env","exit","clear","path","process","script","sys",NULL};
    static NameList nl={z,{0}};
    return name_list_find(&nl,name)>=0;
}
int rt_builtin_known(const char *name) { return builtin_index_of(name)>=0; }

/* ---- evaluation --------------------------------------------------------- */
static Value runNode0(Env *e, IR *node);  /* forward */

/* map an infix operator symbol (a block-data word) to a binary builtin name */
static const char *binop_name(const char *s) {
    static const char *const syms[]={"*","+","-","/","%","^",">","<","=","==","<>",">=","=<","--","//","-->","/%","<=>","??","..","∧",">>","++","&&","||",NULL};
    static const char *const names[]={"mul","add","sub","div","mod","pow","greater?","less?","equal?","equal?","notEqual?","greaterOrEqual?","lessOrEqual?","remove","fdiv","convert","divmod","between?","coalesce","range","and?","write","concat","and?","or?"};
    static NameList nl={syms,{0}};
    int i=name_list_find(&nl,s);
    return i<0?NULL:names[i];
}
static int is_binop(Value v) {
    return (v.k==V_STR||v.k==V_SYMBOL) && binop_name(v.u.s) != NULL;
}
static Value apply_binop(Env *e, Value a, const char *sym, Value b) {
    const char *nm = binop_name(sym);
    Value argv[2] = {a, b}, out;
    if (rt_builtin(nm, e, argv, 2, &out)) return out;
    return v_null();
}

/* structural skip: advance the pointer over an expression WITHOUT evaluating it,
 * so a conditionally-skipped `if cond -> body` body doesn't trigger builtin
 * side effects (print). Mirrors evalExpr/parsePrimary traversal only. */
static int  starts_stmt(Value v);
static int  arg_boundary(Value **it, int n, int ip);
static void skipExpr(Env *e, Value **it, int n, int *ip);
static void skipPrimary(Env *e, Value **it, int n, int *ip) {
    Value v = *it[*ip];
    if (v.k == V_BLOCK) { (*ip)++; return; }            /* block value: one token */
    if (v.k == V_STR && rt_builtin_known(v.u.s) && !env_bound(e, v.u.s)) {
        (*ip)++;                                        /* function head */
        while (*ip<n && !is_binop(*it[*ip]) && !arg_boundary(it,n,*ip))
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

/* fixed arities for the heads that appear in stored action blocks. The greedy
 * boundary scan can't delimit these by itself: `not? key? NOT_MUTATING nm [b]`
 * must give `not?` one arg (key? eats NOT_MUTATING nm) and leave [b] as the
 * `if` branch, while `loop spec\muts [m][body]` must feed `loop` its two block
 * args. -1 means "scan to the boundary" (the old behaviour). */
static int fn_arity(const char *name){
    static const char *const three[]={"loop","map","select","filter","fold","every?","some?","arrange","enumerate","maximum","minimum",NULL};
    static const char *const one[]={"not?","first","last","do","size","type","pop","keys","values",NULL};
    static const char *const two[]={"key?","get","set","equal?","add","sub","mul","div","mod","to","append","contains?",NULL};
    static NameList l3={three,{0}},l1={one,{0}},l2={two,{0}};
    if(name_list_find(&l3,name)>=0)return 3;
    if(name_list_find(&l1,name)>=0)return 1;
    if(name_list_find(&l2,name)>=0)return 2;
    return -1;
}
/* is the idx-th (0-based) arg of this head a BLOCK handed to the function as-is
 * (loop/map/select params+action), never evaluated here? */
static int block_arg(const char *name, int idx){
    if (!strcmp(name,"do") && idx==0) return 1;
    if ((!strcmp(name,"loop")||!strcmp(name,"map")||!strcmp(name,"select")
         ||!strcmp(name,"filter")||!strcmp(name,"fold")||!strcmp(name,"every?")||!strcmp(name,"some?")
         ||!strcmp(name,"arrange")||!strcmp(name,"enumerate")
         ||!strcmp(name,"maximum")||!strcmp(name,"minimum")) && idx>=1) return 1;
    return 0;
}

/* collect `call.attr: value` attributes that immediately follow a call head in
 * block-as-code evaluation (`join.with: " " lst` inside a `case` arm or loop
 * body). Installs them into g_attrs and returns 1 when any were found. */
static int collect_block_attrs(Env *e, Value **it, int n, int *ip){
    const char **names=NULL; Value *vals=NULL; int cnt=0;
    while(*ip<n){
        Value t=*it[*ip];
        if(t.k==V_ATTRIBUTELABEL){
            const char *nm=t.u.s; (*ip)++;
            Value v=evalExpr(e,it,n,ip);
            names=(const char**)xrealloc(names,(size_t)(cnt+1)*sizeof(char*));
            vals=(Value*)xrealloc(vals,(size_t)(cnt+1)*sizeof(Value));
            names[cnt]=strdup(nm); vals[cnt]=v; cnt++;
            continue;
        }
        if(t.k==V_ATTRIBUTE){
            names=(const char**)xrealloc(names,(size_t)(cnt+1)*sizeof(char*));
            vals=(Value*)xrealloc(vals,(size_t)(cnt+1)*sizeof(Value));
            names[cnt]=strdup(t.u.s); vals[cnt]=v_bool(1); cnt++;
            (*ip)++;
            continue;
        }
        break;
    }
    if(cnt>0){ g_attrs.names=names; g_attrs.values=vals; g_attrs.n=cnt; return 1; }
    free(names); free(vals);
    return 0;
}

static Value parsePrimary(Env *e, Value **it, int n, int *ip) {
    Value v = *it[*ip];
    if((v.k==V_STR||v.k==V_SYMBOL)&&!strcmp(v.u.s,"~")&&*ip+1<n&&it[*ip+1]->k==V_STR){Value argument=*it[*ip+1];(*ip)+=2;return b_render(e,&argument,1);}
    if (v.k == V_BLOCK) { (*ip)++; return runBlockValue(e, v); }  /* inline (sub)expr */
    if ((v.k == V_STR || v.k == V_SYMBOL) && !strcmp(v.u.s,"@")) {
        /* `@[a b]` — an EVALUATED block value. In action-body block VALUES the
         * `@` and its block are raw tokens (`parts: parts ++ @[pa]`); parsePrimary
         * must combine them into a block whose elements are evaluated, or the `@`
         * comes back as data and `pa` is lost. */
        if (*ip+1 < n && it[*ip+1]->k == V_BLOCK) {
            Value blk = *it[*ip+1];
            int cnt = blk.u.block.b->n;
            Value **items=(Value**)xmalloc((cnt+1)*sizeof(Value*));
            for (int i=0;i<cnt;i++){
                Value *cp=(Value*)xmalloc(sizeof(Value));
                Value *single=(Value*)xmalloc(sizeof(Value)); single[0]=*blk.u.block.b->items[i];
                Block *sb=(Block*)xmalloc(sizeof *sb); sb->items=(Value**)xmalloc(sizeof(Value*)); sb->items[0]=single; sb->n=1; sb->cap=1;
                Value one=(Value){V_BLOCK, .u.block.b=sb};
                cp[0]=runBlockValue(e, one);
                free(sb); free(single);
                items[i]=cp;
            }
            (*ip)+=2;
            return v_block(items, cnt);
        }
    }
    if (v.k == V_STR || v.k == V_WORD) {
        /* the literal words true/false/null (the frontend emits them as plain
         * strings inside action-body block VALUES) resolve to the literals,
         * matching runNode0's word/load handling and the host. Without this,
         * `isFirst: false` bound isFirst to the TRUTHY string "false" and the
         * joinWith separator branch never ran. */
        if (!strcmp(v.u.s,"true")) { (*ip)++; return v_bool(1); }
        if (!strcmp(v.u.s,"false")) { (*ip)++; return v_bool(0); }
        if (!strcmp(v.u.s,"null")) { (*ip)++; return v_null(); }
        /* Lowered literal/unit data currently shares V_STR with source strings.
         * A final token cannot be a complete builtin call, so preserve it as
         * data before builtin dispatch (`equal? x "in"` must not call `in`). */
        if(*ip+1>=n){
            if(env_bound(e,v.u.s)){Value bound=env_get(e,v.u.s);(*ip)++;if(bound.k==V_FUNC||bound.k==V_BUILTIN)return applyFunc(e,bound,NULL,0);return bound;}
            (*ip)++;return v;
        }
        if (rt_builtin_known(v.u.s) && (!env_bound(e, v.u.s) || !is_shadowable(v.u.s))) {
            /* function head: apply to the following expression(s). Arity-aware
             * when the head has a known fixed arity (loop/map/fold/select take
             * trailing block args verbatim); otherwise scan to a statement
             * boundary so a call in statement position doesn't swallow later
             * statements (a define head, `if`, or a dynamic path write). */
            /* A string LITERAL that names a builtin (`"call" = op`, `intrinsicNode
             * "get"`) is data being compared / an ARG, not a call — same lost
             * word/string distinction as the bound-function branch below. If the
             * NEXT token is a binop, return the string as an operand; likewise if
             * there are fewer remaining tokens than the head's fixed arity, there
             * is no call to make (`"get"` at the end of an inline block). */
            if (*ip+1 < n && is_binop(*it[*ip+1])) { (*ip)++; return v; }
            {
                int A = fn_arity(v.u.s);
                if (A >= 0 && *ip + A >= n) { (*ip)++; return v; }
            }
            (*ip)++;
            AttrContext savedAttr=g_attrs;
            int hasAttr=collect_block_attrs(e, it, n, ip);
            int A = fn_arity(v.u.s);
            Value stackArgs[8];Value *args=n<=8?stackArgs:(Value*)xmalloc((n+1)*sizeof(Value));
            int m = 0;
            if (A >= 0) {
                for (int k=0; k<A && *ip<n; k++) {
                    const char *conversionType=m>0&&IS_STRLIKE(args[0].k)?args[0].u.s:NULL;
                    if(conversionType&&conversionType[0]==':')conversionType++;
                    if ((block_arg(v.u.s,k)||(!strcmp(v.u.s,"to")&&k==1&&conversionType&&(!strcmp(conversionType,"complex")||!strcmp(conversionType,"rational")))) && it[*ip]->k==V_BLOCK) {
                        args[m++] = *it[*ip]; (*ip)++;   /* literal block arg, verbatim */
                    } else args[m++] = evalExpr(e, it, n, ip);
                }
            } else {
                while (*ip < n && !is_binop(*it[*ip]) && !arg_boundary(it,n,*ip))
                    args[m++] = evalExpr(e, it, n, ip);
            }
            Value out;
            if (rt_builtin(v.u.s, e, args, m, &out)) { g_attrs=savedAttr; if(args!=stackArgs)free(args); return out; }
            g_attrs=savedAttr;
            (void)hasAttr;
            if(args!=stackArgs)free(args); die("unknown function in action"); return v_null();
        }
        if (binop_name(v.u.s)) { (*ip)++; return v; }   /* lone operator: data */
        if (env_bound(e, v.u.s)) {
            Value bv = env_get(e, v.u.s);
            if (bv.k==V_FUNC || bv.k==V_BUILTIN) {      /* callable: apply to args */
                /* A string LITERAL that names a bound function (`"define" = node\nodek`)
                 * is data being compared, not a call — the bare-word/string distinction
                 * is lost in the emitted block (both are v_str). A function call's first
                 * argument can never be a binop, so if the NEXT token is a binop this
                 * value is a comparison operand: return it, don't apply it. */
                if (*ip+1 < n && is_binop(*it[*ip+1])) { (*ip)++; return v; }
                (*ip)++;
                AttrContext savedAttr=g_attrs;
                int hasAttr=collect_block_attrs(e,it,n,ip);
                Value stackArgs[8];Value *args=n<=8?stackArgs:(Value*)xmalloc((n+1)*sizeof(Value));
                int m=0;
                while (*ip<n && !is_binop(*it[*ip]) && !arg_boundary(it,n,*ip))
                    args[m++]=evalExpr(e,it,n,ip);
                Value out=applyFunc(e,bv,args,m);
                g_attrs=savedAttr;
                (void)hasAttr;
                if(args!=stackArgs)free(args); return out;
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
    if (*ip >= n || !is_binop(*it[*ip])) return left;
    /* Infix chains are single-precedence and RIGHT-associative (host rule:
     * `a op1 b op2 c` == `a op1 (b op2 c)`, so `0 = x % 2` == `0 = (x % 2)`).
     * Collect the whole chain, then fold from the right. */
    Value stackOperands[8];const char *stackOps[8];Value *operands=n<=8?stackOperands:(Value*)xmalloc((size_t)(n+1)*sizeof(Value));
    const char **ops=n<=8?stackOps:(const char**)xmalloc((size_t)(n+1)*sizeof(const char*));
    int count=0;
    operands[count++]=left;
    while (*ip<n && is_binop(*it[*ip])) {
        ops[count-1]=(*it[*ip]).u.s; (*ip)++;
        operands[count++]=parsePrimary(e,it,n,ip);
    }
    Value result=operands[count-1];
    for (int i=count-2;i>=0;i--) result=apply_binop(e,operands[i],ops[i],result);
    if(operands!=stackOperands)free(operands);if(ops!=stackOps)free(ops);
    return result;
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
/* During argument collection a dynamic path is an ARG (a read: `cBlock n\items`,
 * `cPathKeyNode @DYN blk j`), NOT a statement boundary — but a path that would
 * begin a WRITE statement (`acc\[k]: v`, i.e. followed by a non-operator value)
 * does end the call so the write is its own statement. The old starts_stmt made
 * every dyn path a boundary, so `cBlock n\items` called cBlock with ZERO args
 * and the stray `n\items` read became the case result — every `[1 2 3]` block
 * arg came back raw. */
static int arg_boundary(Value **it, int n, int ip){
    Value v=*it[ip];
    if (v.k==V_STR){
        if (!strcmp(v.u.s,"->")) return 1;
        if (!strncmp(v.u.s,"@LBL:",5)) return 1;
        if (!strcmp(v.u.s,"if")) return 1;
    }
    if (v.k==V_PATH) return path_is_dyn(v)
        && (ip+1<n && !is_binop(*it[ip+1]) && !starts_stmt(*it[ip+1]));
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
    if (cur.k==V_BLOCK){ long idx=atol(key); if(idx<0||idx>=cur.u.block.b->n)return v_null(); return *cur.u.block.b->items[idx]; }
    if (cur.k==V_RANGE){ long idx=atol(key),count=iterator_count(cur); return idx>=0&&idx<count?iterator_item(cur,(int)idx):v_null(); }
    if (cur.k==V_DATE){Value args[2]={cur,v_str(key)};return b_get(e,args,2);}
    if (cur.k==V_COMPLEX){Value args[2]={cur,v_str(key)};return b_get(e,args,2);}
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
        if (i>=0){ dd->vals[i]=val; dict_invalidate_object(dd); return; }
        dict_reserve(dd,dd->n+1);
        dd->keys[dd->n]=(char*)xmalloc(strlen(last)+1); strcpy(dd->keys[dd->n],last);
        dd->vals[dd->n]=val; dd->n++; dict_invalidate_object(dd); dict_index_add(dd,dd->n-1); return;
    }
    if (cur.k==V_BLOCK){ long idx=atol(last); if(idx>=0&&idx<cur.u.block.b->n) *cur.u.block.b->items[idx]=val; }
}

/* evaluate ONE statement. A complex path write `[set, base, keyblock] value`
 * spans a head block plus a following value token; it must be consumed at
 * statement level (a bare evalExpr would see the head block as a 2-arg `set`
 * call and store a null value). Everything else is one expression. */
static Value evalStmt(Env *e, Value **it, int n, int *ip) {
    Value h = *it[*ip];
    if (h.k == V_STR && !strncmp(h.u.s,"@LBL:",5)) { /* `x: val` as an if-body */
        const char *nm = h.u.s+5; (*ip)++;
        Value val = evalExpr(e, it, n, ip);
        env_set(e, nm, val);
        return val;
    }
    if (h.k == V_BLOCK && h.u.block.b->n>=3 && (*h.u.block.b->items[0]).k==V_STR
        && !strcmp((*h.u.block.b->items[0]).u.s,"set")) {
        Value **sit=h.u.block.b->items; int sn=h.u.block.b->n;
        int si=1;
        Value base = evalExpr(e, sit, sn, &si);   /* the dict (by ref) */
        Value key  = evalExpr(e, sit, sn, &si);   /* computed key */
        (*ip)++;
        Value val = evalExpr(e, it, n, ip);
        b_set(e, (Value[]){base,key,val}, 3);
        return val;
    }
    if (h.k == V_PATH && path_is_dyn(h)) {          /* `path\[k]: val` as an if-body */
        /* a dyn path followed by a BINOP is a READ expression (`c\i + 1`),
         * not an assignment: only write when the next token is a value. */
        if (*ip+1 < n && !is_binop(*it[*ip+1])) {   /* WRITE: `c\i: value` */
            int i2 = *ip+1;
            Value val = evalExpr(e, it, n, &i2);
            path_write(e, h, val);
            *ip = i2; return val;
        }
        return evalExpr(e, it, n, ip);              /* READ expression or bare path */
    }
    return evalExpr(e, it, n, ip);
}
/* structural skip of one statement (mirrors evalStmt) */
static void skipStmt(Env *e, Value **it, int n, int *ip) {
    Value h = *it[*ip];
    if (h.k == V_STR && !strncmp(h.u.s,"@LBL:",5)) { /* `x: val` as an if-body */
        (*ip)++;                                    /* skip the define head */
        skipExpr(e, it, n, ip);                     /* skip the value */
        return;
    }
    if (h.k == V_BLOCK && h.u.block.b->n>=3 && (*h.u.block.b->items[0]).k==V_STR
        && !strcmp((*h.u.block.b->items[0]).u.s,"set")) {
        (*ip)++;                                    /* skip the head block */
        skipExpr(e, it, n, ip);                     /* skip the value */
        return;
    }
    if (h.k == V_PATH && path_is_dyn(h) && *ip+1 < n) {
        (*ip)++;                                    /* skip path */
        skipExpr(e, it, n, ip);                     /* skip value */
        return;
    }
    skipExpr(e, it, n, ip);
}
static Value evalSeq(Env *e, Value **it, int n) {
    Value r = v_null();
    int i = 0;
    while (i < n) {
        Value h = *it[i];
        if (h.k == V_PATH && path_is_dyn(h)) {          /* `path\[k]: val` */
            if (i+1 < n && !is_binop(*it[i+1])) {       /* path followed by value: WRITE */
                int i2 = i+1;
                Value val = evalExpr(e, it, n, &i2);
                path_write(e, h, val);
                i = i2; continue;
            }
            r = evalExpr(e, it, n, &i);                 /* READ expr (`c\i + 1`) or bare path */
            continue;
        }
        if (h.k == V_STR && !strncmp(h.u.s,"@LBL:",5)) { /* define `x: val` */
            const char *nm = h.u.s+5; i++;
            r = evalExpr(e, it, n, &i);
            env_set(e, nm, r);
            continue;
        }
        if ((h.k==V_STR||h.k==V_WORD) && !strcmp(h.u.s,"if")) { /* `if cond -> x` / `if cond [b]` */
            int ci = i+1;
            Value cond = evalExpr(e, it, n, &ci);
            if (ci<n && it[ci]->k==V_STR && !strcmp(it[ci]->u.s,"->")) {
                ci++;
                if (v_truthy(cond)) { r = evalStmt(e, it, n, &ci); }   /* run */
                else               { skipStmt(e, it, n, &ci); }        /* skip w/o side effects */
                i = ci; continue;
            }
            if (ci<n && it[ci]->k==V_BLOCK) {
                Value blk = *it[ci]; ci++;
                if (v_truthy(cond)) r = runBlockValue(e, blk);
                i = ci; continue;
            }
            r = cond; i = ci; continue;
        }
        if ((h.k==V_STR||h.k==V_WORD) && !strcmp(h.u.s,"switch")) {
            int ci=i+1;Value cond=evalExpr(e,it,n,&ci);
            if(ci<n&&it[ci]->k==V_BLOCK){Value yes=*it[ci++];if(ci<n&&it[ci]->k==V_BLOCK){Value no=*it[ci++];r=runBlockValue(e,v_truthy(cond)?yes:no);i=ci;continue;}}
            r=cond;i=ci;continue;
        }
        r = evalStmt(e, it, n, &i);
    }
    return r;
}
static Value runBlockValue(Env *e, Value block) {
    return evalSeq(e, block.u.block.b->items, block.u.block.b->n);
}

static int isIRAction(Value action){return action.k==V_FUNC&&action.u.fn.is_action;}
static int irActionSignal(Value action){
    if(!isIRAction(action))return ACTION_NORMAL;
    if(rt_ret_set)return ACTION_STOP;
    if(rt_brk_set){rt_brk_set=0;return ACTION_STOP;}
    if(rt_cont_set){rt_cont_set=0;return ACTION_CONTINUE;}
    return ACTION_NORMAL;
}
static Value makeIRAction(Env *closure,IR *body){
    Value action;action.k=V_FUNC;action.u.fn.params=NULL;action.u.fn.closure=closure;action.u.fn.constructor=0;action.u.fn.is_action=1;action.u.fn.exports=NULL;action.u.fn.nexports=0;
    if(body&&body->op&&(body->opcode==OP___SEQ)){action.u.fn.body=body->args;action.u.fn.nbody=body->nargs;}
    else{action.u.fn.body=(IR**)xmalloc(sizeof(IR*));action.u.fn.body[0]=body;action.u.fn.nbody=body?1:0;}
    return action;
}

/* bind a single parameter to an element and run an action block in a child env */
static void bindActionParam(Env *child, Value params, Value el) {
    if (params.k == V_STR || params.k == V_WORD || params.k == V_LITERAL)
        env_define_local(child, params.u.s, el);
    else if ((params.k==V_PATHLITERAL || params.k==V_PATH)
             && params.u.path.nsegs==1) env_define_local(child, params.u.path.segs[0], el);
    else if (params.k == V_BLOCK && params.u.block.b->n > 0) {
        Value p = *params.u.block.b->items[0];
        if (p.k == V_STR || p.k == V_WORD || p.k == V_LITERAL)
            env_define_local(child, p.u.s, el);
        else if ((p.k==V_PATHLITERAL || p.k==V_PATH) && p.u.path.nsegs==1)
            env_define_local(child, p.u.path.segs[0], el);
    }
}
static int actionParamCount(Value params){return params.k==V_BLOCK&&params.u.block.b->n>0?params.u.block.b->n:1;}
static const char *actionParamName(Value param){
    if(param.k==V_STR||param.k==V_WORD||param.k==V_LITERAL)return param.u.s;
    if((param.k==V_PATH||param.k==V_PATHLITERAL)&&param.u.path.nsegs==1)return param.u.path.segs[0];
    return NULL;
}
static void bindActionValueAt(Env *child,Value params,int at,Value value){
    if(params.k!=V_BLOCK){if(at==0){const char *name=actionParamName(params);if(name)env_define_local(child,name,value);}return;}
    if(at>=params.u.block.b->n)return;
    const char *name=actionParamName(*params.u.block.b->items[at]);
    if(name)env_define_local(child,name,value);
}
static void bindActionChunk(Env *child,Value params,Value collection,int start,int count){
    int width=actionParamCount(params);
    for(int j=0;j<width;j++){
        /* An empty parameter block is Arturo's explicit "ignore the item"
         * form.  It still advances one collection element per iteration, but
         * has no slot to bind. */
        if(params.k==V_BLOCK&&j>=params.u.block.b->n)continue;
        Value param=params.k==V_BLOCK?*params.u.block.b->items[j]:params;const char *name=actionParamName(param);
        if(name)env_define_local(child,name,start+j<count?iterator_item(collection,start+j):v_null());
    }
}
static Value applyActionIndexed(Env *parent, Value params, Value action, Value el,
                                const char *indexName, long index) {
    Env *child = env_new(isIRAction(action)?action.u.fn.closure:parent);
    /* params is a block of word-names, a single quoted word `'x` (a pathliteral
     * reference whose name is its only segment), or a bare passthrough word */
    bindActionParam(child,params,el);
    if(indexName&&*indexName)env_define_local(child,indexName,v_int(index));
    Value r;if(isIRAction(action)){child->rebind_parent=1;r=runSeq(child,action.u.fn.body,action.u.fn.nbody);}
    else r=evalSeq(child,action.u.block.b->items,action.u.block.b->n);
    env_release(child);return r;
}
static Value applyAction(Env *parent, Value params, Value action, Value el, long index) {
    char *indexName=NULL;
    if(rt_has_attr("with"))indexName=val_str(rt_attr_value("with",v_null()));
    Value result=applyActionIndexed(parent,params,action,el,indexName,index);
    free(indexName);
    return result;
}
static Value applyActionAt(Env *parent,Value params,Value action,Value collection,int start,long index){
    Env *child=env_new(isIRAction(action)?action.u.fn.closure:parent);bindActionChunk(child,params,collection,start,iterator_count(collection));
    char *indexName=NULL;if(rt_has_attr("with"))indexName=val_str(rt_attr_value("with",v_null()));
    if(indexName&&*indexName)env_define_local(child,indexName,v_int(index));
    Value result;if(isIRAction(action)){child->rebind_parent=1;result=runSeq(child,action.u.fn.body,action.u.fn.nbody);}
    else result=evalSeq(child,action.u.block.b->items,action.u.block.b->n);free(indexName);env_release(child);return result;
}
static Value applyFoldAction(Env *parent,Value params,Value action,Value accumulator,
                             Value collection,int start,int width,long index){
    Env *child=env_new(isIRAction(action)?action.u.fn.closure:parent);
    bindActionValueAt(child,params,0,accumulator);
    if(collection.k==V_DICT){
        bindActionValueAt(child,params,1,v_str(collection.u.dict->keys[start]));
        bindActionValueAt(child,params,2,collection.u.dict->vals[start]);
    }else for(int j=0;j<width&&start+j<iterator_count(collection);j++)
        bindActionValueAt(child,params,j+1,iterator_item(collection,start+j));
    char *indexName=NULL;if(rt_has_attr("with"))indexName=val_str(rt_attr_value("with",v_null()));
    if(indexName&&*indexName)env_define_local(child,indexName,v_int(index));
    Value result;if(isIRAction(action)){child->rebind_parent=1;result=runSeq(child,action.u.fn.body,action.u.fn.nbody);}
    else result=evalSeq(child,action.u.block.b->items,action.u.block.b->n);
    free(indexName);env_release(child);return result;
}

Value runSeq(Env *e, IR **seq, int n) {
    Value result = v_null();
    for (int i=0;i<n;i++) {
        result = runNode0(e, seq[i]);
        if (rt_ret_set || rt_brk_set || rt_cont_set) break;
    }
    return result;
}

/* host stdlib words are dispatch-through-the-symbol-table functions, so a user
 * binding shadows them; compiled opcodes always win (empirically classified). */
static int is_shadowable(const char *name){
    static const char *const words[] = {
        "sum","first","last","max","min","upper","lower","sort","filter","fold",
        "until","abs","floor","ceil","round","sqrt","trim","strip","repeat",
        "contains?","key?","values","keys","push","pop","remove","insert",
        "type","empty","index",
        NULL
    };
    static NameList nl={words,{0}};
    return name_list_find(&nl,name)>=0;
}

/* a call whose callee is intrinsic: dispatch to a builtin */
static int rt_builtin_cached(IR *fn, Env *e, Value *args, int n, Value *out){
    int i=fn->builtin;
    if(i<0)return 0;
    int expected=fn->arity;
    if(expected>=0&&n<expected){char msg[256];snprintf(msg,sizeof msg,"%s: not enough arguments (expected %d, got %d)",fn->name,expected,n);die(msg);}
    *out=BUILTINS[i].fn(e,args,n);return 1;
}
static Value callIntrinsic(Env *e, IR *node) {
    const char *name = node->fn->name;
    /* The frontend emits `ir_intrinsic` ONLY for a real builtin CALL HEAD
     * (user functions go through `ir_load`); the host's rule is that builtins
     * WIN over same-named user functions (`add: function [...]` then `add 3 4`
     * still calls the builtin — corpus/17_shadowing). The old bound-first check
     * made the compiler's OWN `fold` (a 1-arg user function in ir.art that the
     * intrinsic table wrongly lists as a builtin) dispatch to itself; that name
     * is now absent from the intrinsic table so the emitted code loads it, and
     * an intrinsic here is always the primitive. */
    Value stackAttrs[4], stackArgs[8];
    Value *attrValues=node->nattrs<=4?stackAttrs:(Value*)xmalloc((size_t)(node->nattrs+1)*sizeof(Value));
    for(int i=0;i<node->nattrs;i++)attrValues[i]=runNode0(e,node->attr_values[i]);
    Value *argv = node->nargs<=8?stackArgs:(Value*)xmalloc((node->nargs+1)*sizeof(Value));
    for (int i=node->nargs-1;i>=0;i--) argv[i] = runNode0(e, node->args[i]);
#define CI_RELEASE() do{ if(attrValues!=stackAttrs)free(attrValues); if(argv!=stackArgs)free(argv); }while(0)
    AttrContext previous=g_attrs;int replaceAttrs=node->nattrs>0;if(replaceAttrs){g_attrs.names=node->attr_names;g_attrs.values=attrValues;g_attrs.n=node->nattrs;}
    Value out;
    int arithmeticRef=node->nargs>0&&argv[0].k==V_PATH&&(node->fn->flags&IRF_ARITH_REF);
    if(arithmeticRef){MutTarget target;argv[0]=mut_load(e,argv[0],&target);if(rt_builtin_cached(node->fn,e,argv,node->nargs,&out)){mut_store(e,&target,out);if(replaceAttrs)g_attrs=previous;CI_RELEASE();return v_null();}}
    if ((node->fn->flags&IRF_SHADOWABLE) && env_bound(e, name)) {
        Value bound = env_get(e, name);
        if (bound.k==V_FUNC || bound.k==V_BUILTIN) {
            Value r = applyFunc(e, bound, argv, node->nargs);
            if(replaceAttrs)g_attrs=previous;CI_RELEASE();return r;
        }
    }
    if (rt_builtin_cached(node->fn, e, argv, node->nargs, &out)) { if(replaceAttrs)g_attrs=previous;CI_RELEASE();return out; }
    if(replaceAttrs)g_attrs=previous;CI_RELEASE();
#undef CI_RELEASE
    fprintf(stderr,"[unknown intrinsic: %s]\n", name);
    die("unknown intrinsic"); return v_null();
}

/* a `do` block body (`(__seq` in the emitted IR) is a statement sequence the
 * host runs as an EXPRESSION: `do ( = A B )` emits do(seq[=, A, B]) and the host
 * evaluates the leading infix symbol as a PREFIX call (`= A B` == `equal? A B`).
 * The kernel's runSeq treats each node as a bare statement and would return the
 * LAST operand (B) instead — so `or? (= :path t) (= :pathlabel t)` was always
 * truthy and cNode sent every dict through the path emitter. Dispatch a leading
 * passthrough whose text is an infix symbol to the mapped builtin over the
 * following statements (arity-limited, like the block-as-code evaluator). */
/* host auto-applies a block that is EXACTLY two expressions where one value is
 * a function: `[f x]` -> f(x), `[x f]` -> f(x). Applies to function bodies,
 * `do` blocks, and code-executed block values. A statement (`define`, `if`,
 * `loop`, `return`, ...) in the two-expression tail disables it — the host
 * only folds two trailing *value* expressions, not `[x: 5 f x]`. */
static int node_is_stmt(IR *node){
    if (!node || !node->op) return 1;
    return (node->opcode==OP_DEFINE)||(node->opcode==OP_LET)||(node->opcode==OP_IF)||
           (node->opcode==OP_UNLESS)||(node->opcode==OP_WHILE)||(node->opcode==OP_UNTIL)||
           (node->opcode==OP_LOOP)||(node->opcode==OP_RETURN)||(node->opcode==OP_BREAK)||
           (node->opcode==OP_CONTINUE)||(node->opcode==OP_SWITCH)||(node->opcode==OP_CASE);
}
static Value apply_tail_two(Env *e, Value second, Value last){
    if (last.k == V_FUNC && second.k != V_FUNC) return applyFunc(e, last, &second, 1);
    if (second.k == V_FUNC && last.k != V_FUNC) return applyFunc(e, second, &last, 1);
    return last;
}

static Value runDoSeq(Env *e, IR **seq, int n){
    if (n >= 3 && seq[0] && seq[0]->op && !strcmp(seq[0]->op,"passthrough")) {
        const char *nm = binop_name(seq[0]->v.u.s);
        if (nm) {
            int A = fn_arity(nm);
            int avail = n - 1;
            if (A >= 0 && A < avail) avail = A;
            Value stack[8];Value *argv=avail<=8?stack:(Value*)xmalloc((avail+1)*sizeof(Value));
            for (int k=0;k<avail;k++) argv[k] = runNode0(e, seq[1+k]);
            Value out;
            if (rt_builtin(nm, e, argv, avail, &out)) { if(argv!=stack)free(argv); return out; }
            if(argv!=stack)free(argv);
        }
    }
    Value r = v_null(), second = v_null();
    int tailed = 0;   /* a path call consumed its tail; skip the 2-expr fold */
    for (int i=0;i<n;i++){
        second = r;
        r = runNode0(e, seq[i]);
        if (rt_brk_set || rt_cont_set) break;
        if (rt_ret_set) {
            IR *pg = ret_pathget(seq[i]);
            if (pg && (r.k==V_FUNC || r.k==V_BUILTIN)) {
                rt_ret_set = 0;
                if (path_call_tail(e, seq, n, &i, pg, r, &r)) rt_ret_val = r;
                rt_ret_set = 1;
            }
            break;
        }
        if (r.k==V_FUNC && n>1 && path_call_tail(e, seq, n, &i, seq[i], r, &r)) { tailed = 1; continue; }
    }
    if (!tailed && n == 2 && !node_is_stmt(seq[0]) && !node_is_stmt(seq[1])) return apply_tail_two(e, second, r);
    return r;
}

static Value runNode0(Env *e, IR *node) {
    if (!node) return v_null();
    if (!node->op) return node->v;   /* a raw constant */

    switch (node->opcode) {
    case OP_CONST: return node->v;
    case OP_LOAD: {
        /* the emitted IR loads the literal words `true`/`false`/`null` as
         * `ir_load("true")` (e.g. a dict field `infix?: true`). The host treats
         * those as literals; bind them here so they don't come back null. */
        if (node->literal) return literal_value(node->literal);
        Value v = env_get(e, node->name);
        if (v.k != V_NULL) return v;
        if (rt_builtin_known(node->name)) {
            Value b;b.k=V_BUILTIN;b.u.s=(char*)node->name;return b;
        }
        if (!env_bound(e, node->name)) { char msg[256]; snprintf(msg,sizeof msg,"name error: identifier not found: %s",node->name); die(msg); }
        return v;
    }
    case OP_INTRINSIC: return v_str(node->name); /* hostword marker */
    case OP_WORD: {
        /* bare word in value position: load the binding if one exists, else call
         * a zero-arity builtin, else a builtin function value, else null. */
        if (node->literal) return literal_value(node->literal);
        Value v = env_get(e, node->name);
        if (v.k != V_NULL) return v;
        if (rt_builtin_known(node->name)) {
            if (rt_zero_arity(node->name)) {
                Value out, zargv[1];
                if (rt_builtin(node->name, e, zargv, 0, &out)) return out;
            }
            Value b;b.k=V_BUILTIN;b.u.s=(char*)node->name;return b;
        }
        return v_null();
    }
    case OP_PASSTHROUGH: return node->v;
    case OP___SEQ: return runSeq(e, node->args, node->nargs);

    case OP_DEFINE: {
        Value v = runNode0(e, node->args[0]);
        env_define_body(e, node->name, v);
        return v;
    }
    case OP_LET: {
        Value v = runNode0(e, node->args[0]);
        env_let(e, node->name, v);
        return v;
    }
    case OP_BLOCK: {
        Value **items=(Value**)xmalloc((node->nargs+1)*sizeof(Value*));
        int used=0;
        for (int i=0;i<node->nargs;i++){
            Value rv = runNode0(e, node->args[i]);
            Value *cp=(Value*)xmalloc(sizeof(Value)); *cp=rv; items[used++]=cp;
        }
        return v_block(items,used);
    }
    case OP_FUNCTION: {
        IR *params = node->args[0];
        IR **body = (IR**)xmalloc((node->nargs)*sizeof(IR*));
        for (int i=1;i<node->nargs;i++) body[i-1]=node->args[i];
        Value fn=v_func(params,body,node->nargs-1,e);
        for(int ai=0;ai<node->nattrs;ai++)if(!strcmp(node->attr_names[ai],"export")){
            IR *spec=node->attr_values[ai];if(spec&&spec->op&&(spec->opcode==OP_BLOCK)){
                fn.u.fn.exports=xmalloc((size_t)(spec->nargs+1)*sizeof(char*));
                for(int j=0;j<spec->nargs;j++){IR *item=spec->args[j];const char *name=NULL;if(item&&item->op&&(item->opcode==OP_LOAD))name=item->name;else if(item&&item->op&&(item->opcode==OP_CONST)&&(item->v.k==V_WORD||item->v.k==V_LITERAL||item->v.k==V_STR))name=item->v.u.s;if(name)fn.u.fn.exports[fn.u.fn.nexports++]=strdup(name);}
            }
        }
        return fn;
    }
    case OP_METHOD: {
        IR *params = node->args[0];
        IR **body = (IR**)xmalloc((node->nargs)*sizeof(IR*));
        for (int i=1;i<node->nargs;i++) body[i-1]=node->args[i];
        return v_func(params, body, node->nargs-1, e);
    }
    case OP_CONSTRUCTOR: {
        IR *params=node->nargs?node->args[0]:ir_block(NULL,0);Value v=v_func(params,NULL,0,e);v.u.fn.constructor=1;return v;
    }
    case OP_IS: {
        Value base=runNode0(e,node->args[0]);Value extra=runNode0(e,node->args[1]);return runtime_inherit(e,base.u.s,extra);
    }
    case OP_DICTIONARY: {
        IR *body=node->nargs ? node->args[0] : NULL;
        if(body&&body->op&&(body->opcode==OP___SEQ)&&body->nargs>0){int stringData=1;size_t length=0;for(int i=0;i<body->nargs;i++){IR *part=body->args[i];if(!part||!part->op||(part->opcode!=OP_CONST)||part->v.k!=V_CHAR){stringData=0;break;}length+=strlen(part->v.u.c);}if(stringData){char *path=xmalloc(length+1);size_t at=0;for(int i=0;i<body->nargs;i++){size_t n=strlen(body->args[i]->v.u.c);memcpy(path+at,body->args[i]->v.u.c,n);at+=n;}path[at]=0;FILE*f=fopen(path,"rb");if(!f){char msg[512];snprintf(msg,sizeof msg,"dictionary: file not found '%s'",path);free(path);die(msg);}fclose(f);free(path);}}
        Env *child=env_new(e);
        if(body&&body->op&&(body->opcode==OP___SEQ))for(int i=0;i<body->nargs;i++){IR *field=body->args[i];if(field&&field->op&&(field->opcode==OP_LOAD)&&field->name&&!env_bound(child,field->name))env_define_local(child,field->name,v_null());}
        if(body && body->op && (body->opcode==OP___SEQ))
            (void)runSeq(child,body->args,body->nargs);
        else if(body)
            (void)runNode0(child,body);
        char **keys=(char**)xmalloc((size_t)(child->n+1)*sizeof(char*));
        Value *vals=(Value*)xmalloc((size_t)(child->n+1)*sizeof(Value));
        for(int i=0;i<child->n;i++){
            keys[i]=strdup(child->names[i]);
            vals[i]=child->vals[i];
        }
        int count=child->n;Value result=v_dict(keys,vals,count);env_release(child);return result;
    }
    case OP_IF: {
        Value cond = runNode0(e, node->args[0]);
        if (v_truthy(cond)) {
            Value rv = node->nargs>1 ? runNode0(e, node->args[1]) : v_null();
            return rv;
        }
        if (node->nargs>2 && node->args[2]) return runNode0(e, node->args[2]);
        return v_null();
    }
    case OP_UNLESS: {
        Value cond=runNode0(e,node->args[0]);
        if(!v_truthy(cond)) return runNode0(e,node->args[1]);
        return v_null();
    }
    case OP_SWITCH: {
        Value cond=runNode0(e,node->args[0]);
        return runNode0(e,node->args[v_truthy(cond)?1:2]);
    }
    case OP_WHEN: {
        for(int i=0;i+1<node->nargs;i+=2){
            Value cond=runNode0(e,node->args[i]);
            if(v_truthy(cond)) return runNode0(e,node->args[i+1]);
        }
        return v_null();
    }
    case OP_USING: {
        Value value=runNode0(e,node->args[0]);
        Env *child=env_new(e);
        env_define_local(child,"this",value);
        child->rebind_parent=1;
        IR *body=node->args[1];
        Value result;if(body && body->op && (body->opcode==OP___SEQ))result=runSeq(child,body->args,body->nargs);
        else result=body?runNode0(child,body):v_null();
        env_release(child);return result;
    }
    case OP_TRY: case OP_THROWS_Q: {
        int predicate=(node->opcode==OP_THROWS_Q);
        jmp_buf jb;jmp_buf *prev=g_try_jmp;AttrContext savedAttrs=g_attrs;unsigned long savedEnvSerial=g_env_serial;g_try_jmp=&jb;
        if(setjmp(jb)==0){
            (void)runNode0(e,node->args[0]);g_try_jmp=prev;
            return predicate?v_bool(0):v_null();
        }
        g_try_jmp=prev;g_attrs=savedAttrs;env_rewind(savedEnvSerial);
        return predicate?v_bool(1):v_error(g_last_error);
    }
    case OP_ENSURE: {
        Value condition=runNode0(e,node->args[0]);
        if(!v_truthy(condition))die("assertion failed");
        return v_null();
    }
    case OP_DO: {
        IR *body = node->args[0];
        /* a __seq body is already the block's statements; running it yields
         * the last value (a block result there is data, e.g. `do [[1 2][3 4]]`).
         * Use the expression runner so a leading infix symbol is a prefix call. */
        if (body && body->op && (body->opcode==OP___SEQ)) return runDoSeq(e, body->args, body->nargs);
        /* a stored block value (e.g. `do b` where b = [10 * 3]) runs as code */
        Value v = runNode0(e, body);
        if (v.k == V_BLOCK) return runBlockValue(e, v);
        return v;
    }
    case OP_RETURN: {
        /* Evaluate the argument FIRST: `return <call f>` must run f (whose
         * applyFunc resets rt_ret_set at entry) without clobbering the return
         * flag this op is about to raise. Setting the flag before evaluating
         * the argument loses the return whenever the argument is a function
         * call — f's applyFunc clears it back to 0 and the return vanishes,
         * so the caller's `if not? isIR node -> return ...` falls through. */
        rt_ret_val = runNode0(e, node->args[0]);
        rt_ret_set = 1;
        return rt_ret_val;
    }
    case OP_BREAK: { rt_brk_set=1; return v_null(); }
    case OP_CONTINUE: { rt_cont_set=1; return v_null(); }
    case OP_HIGHER: {
        Value coll=runNode0(e,node->args[0]);Value params=runNode0(e,node->args[1]);Value action=makeIRAction(e,node->args[2]);
        Value stackAttrs[4];Value *attrValues=node->nattrs<=4?stackAttrs:(Value*)xmalloc((size_t)(node->nattrs+1)*sizeof(Value));
        for(int i=0;i<node->nattrs;i++)attrValues[i]=runNode0(e,node->attr_values[i]);
        AttrContext previous=g_attrs;int replaceAttrs=node->nattrs>0;if(replaceAttrs){g_attrs.names=node->attr_names;g_attrs.values=attrValues;g_attrs.n=node->nattrs;}
        Value args[3]={coll,params,action},result;
        if(!rt_builtin_cached(node,e,args,3,&result)){if(replaceAttrs)g_attrs=previous;if(attrValues!=stackAttrs)free(attrValues);die("unknown higher-order intrinsic");}
        if(replaceAttrs)g_attrs=previous;if(attrValues!=stackAttrs)free(attrValues);return result;
    }
    case OP_WHILE: {
        rt_brk_set=0; rt_cont_set=0;
        while (1) {
            Value cond = runNode0(e, node->args[0]); if (rt_ret_set) break;
            if (!v_truthy(cond)) break;
            runNode0(e, node->args[1]); if (rt_ret_set || rt_brk_set) break;
            rt_cont_set=0;
        }
        rt_brk_set=0; rt_cont_set=0;
        if (rt_ret_set) return rt_ret_val;
        return v_null();
    }
    case OP_UNTIL: {
        rt_brk_set=0; rt_cont_set=0;
        while (1) {
            runNode0(e, node->args[0]); if (rt_ret_set || rt_brk_set) break;
            rt_cont_set=0;
            Value cond = runNode0(e, node->args[1]); if (rt_ret_set) break;
            if (v_truthy(cond)) break;
        }
        rt_brk_set=0; rt_cont_set=0;
        if (rt_ret_set) return rt_ret_val;
        return v_null();
    }
    case OP_LOOP: {
        Value coll = runNode0(e, node->args[0]);
        Value params = runNode0(e, node->args[1]);
        char *indexName=NULL;
        for(int ai=0;ai<node->nattrs;ai++)if(!strcmp(node->attr_names[ai],"with")){
            Value indexValue=runNode0(e,node->attr_values[ai]);
            indexName=val_str(indexValue);
            break;
        }
        IR *body = node->args[2];
        IR **body_items = body && body->op && (body->opcode==OP___SEQ) ? body->args : &body;
        int body_n = body && body->op && (body->opcode==OP___SEQ) ? body->nargs : 1;
        rt_brk_set=0; rt_cont_set=0;
        if (coll.k==V_DICT) {
            Dict *dd=coll.u.dict;
            for (int i=0;i<dd->n;i++) {
                Env *child=env_new(e);
                if (params.k==V_BLOCK && params.u.block.b->n==1)
                    env_define_local(child, (*params.u.block.b->items[0]).u.s, dd->vals[i]);
                else if (params.k==V_BLOCK && params.u.block.b->n>=2) {
                    env_define_local(child, (*params.u.block.b->items[0]).u.s, v_str(dd->keys[i]));
                    env_define_local(child, (*params.u.block.b->items[1]).u.s, dd->vals[i]);
                }
                if(indexName&&*indexName)env_define_local(child,indexName,v_int(i));
                child->rebind_parent=1;
                runSeq(child, body_items, body_n);
                env_release(child);
                if (rt_ret_set || rt_brk_set) break;
                rt_cont_set=0;
            }
        } else if (coll.k==V_BLOCK || coll.k==V_RANGE || coll.k==V_STR) {
            int count=iterator_count(coll),width=actionParamCount(params),group=0;
            for (int i=0;i<count;i+=width,group++) {
                Env *child=env_new(e);bindActionChunk(child,params,coll,i,count);
                if(indexName&&*indexName)env_define_local(child,indexName,v_int(group));
                child->rebind_parent=1;
                runSeq(child, body_items, body_n);
                env_release(child);
                if (rt_ret_set || rt_brk_set) break;
                rt_cont_set=0;
            }
        } else die("loop: unsupported collection");
        rt_brk_set=0; rt_cont_set=0;
        free(indexName);
        if (rt_ret_set) return rt_ret_val;
        return v_null();
    }
    case OP_RANGE: {
        Value from = runNode0(e, node->args[0]);
        Value to = runNode0(e, node->args[1]);
        Value stackAttrs[4];Value *attrValues=node->nattrs<=4?stackAttrs:(Value*)xmalloc((size_t)(node->nattrs+1)*sizeof(Value));
        for(int i=0;i<node->nattrs;i++)attrValues[i]=runNode0(e,node->attr_values[i]);
        AttrContext previous=g_attrs;
        if(node->nattrs>0){g_attrs.names=node->attr_names;g_attrs.values=attrValues;g_attrs.n=node->nattrs;}
        Value args[2]={from,to};Value result=b_range(e,args,2);
        if(node->nattrs>0)g_attrs=previous;
        if(attrValues!=stackAttrs)free(attrValues);
        return result;
    }
    case OP_CALL: {
        if (node->fn && node->fn->op && (node->fn->opcode==OP_INTRINSIC)){
            Value r = callIntrinsic(e, node);
            return r;
        }
        /* user callee: evaluate to a function value and apply */
        Value fn = runNode0(e, node->fn);
        /* a path whose value is DATA is not a call on the host (CheckCallablePath
         * only fires for a function), so a front-grouped `return d\add 3 4` where
         * `d\add` holds 5 returns the value and drops the args — it must not die. */
        if (is_pathget_ir(node->fn) && fn.k != V_FUNC && fn.k != V_BUILTIN) return fn;
        Value stackAttrs[4],stackArgs[8];Value *attrValues=node->nattrs<=4?stackAttrs:(Value*)xmalloc((size_t)(node->nattrs+1)*sizeof(Value));
        for(int i=0;i<node->nattrs;i++)attrValues[i]=runNode0(e,node->attr_values[i]);
        Value *argv=node->nargs<=8?stackArgs:(Value*)xmalloc((node->nargs+1)*sizeof(Value));
        for (int i=node->nargs-1;i>=0;i--) argv[i]=runNode0(e,node->args[i]);
        if (fn.k==V_FUNC || fn.k==V_BUILTIN){AttrContext previous=g_attrs;int replaceAttrs=node->nattrs>0;if(replaceAttrs){g_attrs.names=node->attr_names;g_attrs.values=attrValues;g_attrs.n=node->nattrs;}Value out=applyFunc(e,fn,argv,node->nargs);if(replaceAttrs)g_attrs=previous;if(attrValues!=stackAttrs)free(attrValues);if(argv!=stackArgs)free(argv);return out;}
        if(attrValues!=stackAttrs)free(attrValues);if(argv!=stackArgs)free(argv);
        die("cannot call non-function");
        return v_null();
    }
    default: break;
    }
    {char msg[160];snprintf(msg,sizeof msg,"unknown IR op: %s",node->op?node->op:"(null)");die(msg);}; return v_null();
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
static int is_id_start(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_id_in(int c){ return is_id_start(c)||(c>='0'&&c<='9'); }
static int is_digit(int c){ return c>='0'&&c<='9'; }
static int in_syms(int c){ return c=='~'||c=='!'||c=='@'||c=='#'||c=='$'||c=='%'||c=='^'||c=='&'||c=='*'||c=='-'||c=='='||c=='+'||c=='<'||c=='>'||c=='/'||c=='|'||c=='?'; }

/* current 1-based line: one plus the number of newlines before the cursor.
 * Positions only ever advance within one lex_source call, so the count is
 * cached and only the new bytes are rescanned (amortized O(1) per token). */
static int g_line_scan_pos = 0;
static int g_line_count = 1;
static void line_count_reset(void){ g_line_scan_pos = 0; g_line_count = 1; }
static int lx_line(LX*x){
    for(int i=g_line_scan_pos; i<x->pos && i<x->len; i++) if(x->s[i]=='\n') g_line_count++;
    g_line_scan_pos = x->pos;
    return g_line_count;
}

static void sb_init(SB*s){ s->cap=32; s->len=0; s->b=xmalloc(32); s->b[0]=0; }
static void sb_add(SB*s,int c){ if(s->len+2>s->cap){s->cap*=2;s->b=xrealloc(s->b,s->cap);} s->b[s->len++]=(char)c; s->b[s->len]=0; }
static void sb_adds(SB*s,const char*st){ for(;*st;st++) sb_add(s,*st); }

typedef struct { Value **items; int n, cap; } BLD;
static void bld_init(BLD*b){ b->cap=8; b->n=0; b->items=xmalloc(b->cap*sizeof(Value*)); }
static void bld_add(BLD*b,Value v){ if(b->n>=b->cap){ b->cap*=2; b->items=xrealloc(b->items,b->cap*sizeof(Value*)); } b->items[b->n]=(Value*)xmalloc(sizeof(Value)); b->items[b->n][0]=v; b->n++; }
static Value bld_block(BLD*b){ return v_block(b->items,b->n); }

/* line-map capture: every flat element in `to :block` order gets one entry.
 * Inline parens are a single element whose contents contribute no entries;
 * dynamic path segments and blocks do not add entries themselves (the block
 * open `[` is the one element, its contents are separate flat elements). */
static int g_line_capture = 1;
static int g_last_line = 1;
static void lb_add(BLD*b,Value v){ if(g_line_capture) line_map_add(g_last_line); bld_add(b,v); }

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

/* port of parse.nim's parseSafeString: `«« ... »»` — content between the
 * opening and closing guillemet pairs, CR/LF normalized to LF, no dedent,
 * terminated by the first `»»` (0xC2 0xBB 0xC2 0xBB). */
static void parse_safe_string(LX*x,SB*v){
    x->pos+=4; /* skip «« */
    while(x->pos<x->len){
        int c=(unsigned char)lx_at(x);
        if(c==0xC2 && (unsigned char)lx_peek(x,1)==0xBB &&
           (unsigned char)lx_peek(x,2)==0xC2 && (unsigned char)lx_peek(x,3)==0xBB){
            x->pos+=4; break; /* closing »» */
        }
        if(c=='\r'){ sb_add(v,'\n'); x->pos++; if(x->pos<x->len&&lx_at(x)=='\n')x->pos++; }
        else if(c=='\n'){ sb_add(v,'\n'); x->pos++; }
        else { sb_add(v,c); x->pos++; }
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
            if(LK(pos)=='='){
                pos++;
                if(LK(pos)=='>'){pos++;sb_adds(glyph,"<=>");}
                else if(LK(pos)=='='){pos++;sb_adds(glyph,"<==");}
                else sb_adds(glyph,"<=");
            }
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
        "char","string","word","literal","label","attribute","attributelabel","attributeLabel","path",
        "pathlabel","pathLabel","pathliteral","pathLiteral","symbol","symbolliteral","symbolLiteral","unit","quantity","error",
        "errorkind","errorKind","regex","color","date","binary","dictionary","object","store",
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
static char *dedent_curly_text(const char *input){
    size_t len=strlen(input),minimum=(size_t)-1,line=0;
    while(line<len){size_t end=line;while(end<len&&input[end]!='\n')end++;size_t p=line,indent=0;while(p<end&&(input[p]==' '||input[p]=='\t')){p++;indent++;}if(p<end&&indent<minimum)minimum=indent;line=end<len?end+1:end;}
    if(minimum==(size_t)-1)minimum=0;
    size_t cap=len+1,used=0;char *out=xmalloc(cap);line=0;
    while(line<len){size_t end=line;while(end<len&&input[end]!='\n')end++;size_t p=line,drop=0;while(p<end&&drop<minimum&&(input[p]==' '||input[p]=='\t')){p++;drop++;}if(end>p){memcpy(out+used,input+p,end-p);used+=end-p;}if(end<len)out[used++]='\n';line=end<len?end+1:end;}
    size_t first=0;while(first<used&&isspace((unsigned char)out[first]))first++;size_t last=used;while(last>first&&isspace((unsigned char)out[last-1]))last--;memmove(out,out+first,last-first);out[last-first]=0;return out;
}
static Value parse_curly(LX*x){
    x->pos++; /* consume '{' */
    int regex=0,verbatim=0;
    char flags[8]={0}; int nflags=0;
    if(lx_at(x)=='!'){
        x->pos++;
        while(lx_at(x) && is_id_in((unsigned char)lx_at(x))) x->pos++;
    }
    if(lx_at(x)==':'){ x->pos++;verbatim=1; }
    else if(lx_at(x)=='/'){ x->pos++; regex=1; }
    SB sb; sb_init(&sb);
    int depth=1;
    while(x->pos<x->len && depth>0){
        int c=lx_at(x);
        if(verbatim){
            /* `{: ... :}` closes at `:}`; a bare `}` is content */
            if(c==':' && (unsigned char)lx_peek(x,1)=='}'){ x->pos+=2; break; }
            if(c=='\r'){ sb_add(&sb,'\n'); x->pos++; if(x->pos<x->len&&lx_at(x)=='\n')x->pos++; }
            else if(c=='\n'){ sb_add(&sb,'\n'); x->pos++; }
            else { sb_add(&sb,c); x->pos++; }
            continue;
        }
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
        else if(c=='\r'){ sb_add(&sb,'\n'); x->pos++; if(x->pos<x->len&&lx_at(x)=='\n')x->pos++; }
        else if(c=='\n'){ sb_add(&sb,'\n'); x->pos++; }
        else { sb_add(&sb,c); x->pos++; }
    }
    flags[nflags]=0;
    if(lx_at(x)==':'){ x->pos++; return v_token(V_LABEL, sb.b); }
    if(regex) return v_token(V_REGEX, sb.b);
    if(!verbatim){char *normalized=dedent_curly_text(sb.b);free(sb.b);Value out=v_str(normalized);free(normalized);return out;}
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
            x->pos++; int saveCap=g_line_capture; g_line_capture=0; Value sub=parse_block(x,0,1,0); g_line_capture=saveCap;
            if(n>=cap){cap*=2;segs=xrealloc(segs,cap*sizeof(Value));} segs[n++]=sub;
        } else break;
    }
    return v_pathv(segs,n);
}

static Value parse_block(LX*x,int level,int isSubBlock,int isSubInline){
    BLD b; bld_init(&b);
    int oldCapture = g_line_capture;
    if(isSubBlock || isSubInline) g_line_capture = 0;
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
        g_last_line = lx_line(x);
        int c=lx_at(x);
        if(c=='"'){
            SB sb; sb_init(&sb); parse_string(x,&sb,'"');
            if(lx_at(x)==':'){ x->pos++; lb_add(&b, v_token(V_LABEL, sb.b)); }
            else lb_add(&b, v_str(sb.b));
            free(sb.b);
        } else if(c==':'){
            SB sb; sb_init(&sb); parse_ident(x,&sb,0);
            if(sb.len==0){ free(sb.b); sb.b=NULL;
                if(lx_at(x)==':'){ x->pos++; lb_add(&b,v_token(V_SYMBOL,"::")); }
                else if(lx_at(x)=='='){ x->pos++; lb_add(&b,v_token(V_SYMBOL,":=")); }
                else lb_add(&b,v_token(V_SYMBOL,":"));
            } else {
                /* host `newType`: a `:name` whose name is not a known ValueKind
                 * resolves to `:object` (e.g. `:interpret`, `:emit`, `:foo`).
                 * Known names keep their own name. */
                if(lx_known_type(sb.b)) lb_add(&b, v_token(V_TYPE, sb.b));
                else lb_add(&b, v_token(V_TYPE, "object"));
                free(sb.b);
            }
        } else if(is_digit(c)){
            /* `0x...` hex literal: host lexes 0xE8 as 232 (lowercase `0x` only;
             * `0X1f` is `0` + the word `X1f` on the host, which then errors) */
            if(c=='0'&&lx_peek(x,1)=='x'&&isxdigit((unsigned char)lx_peek(x,2))){
                x->pos+=2;SB hx;sb_init(&hx);
                while(x->pos<x->len&&isxdigit((unsigned char)lx_at(x))){sb_add(&hx,lx_at(x));x->pos++;}
                lb_add(&b,v_int((long)strtoll(hx.b,NULL,16)));free(hx.b);continue;
            }
            SB sb; sb_init(&sb); int hasDot; parse_number(x,&sb,&hasDot);
            if(!hasDot&&lx_at(x)==':'&&is_digit((unsigned char)lx_peek(x,1))){
                x->pos++;SB denominator;sb_init(&denominator);while(x->pos<x->len&&is_digit((unsigned char)lx_at(x))){sb_add(&denominator,lx_at(x));x->pos++;}
                if(lx_at(x)=='`'){x->pos++;SB unit;sb_init(&unit);while(x->pos<x->len&&(is_id_in((unsigned char)lx_at(x))||lx_at(x)=='/'||lx_at(x)=='*'||lx_at(x)=='.')){sb_add(&unit,lx_at(x));x->pos++;}lb_add(&b,v_quantity((double)atol(sb.b)/(double)atol(denominator.b),unit.b));free(unit.b);}
                else lb_add(&b,v_rational(atol(sb.b),atol(denominator.b)));free(denominator.b);free(sb.b);continue;
            }
            if(lx_at(x)=='`'){
                x->pos++;SB unit;sb_init(&unit);while(x->pos<x->len&&(is_id_in((unsigned char)lx_at(x))||lx_at(x)=='/'||lx_at(x)=='*'||lx_at(x)=='.')){sb_add(&unit,lx_at(x));x->pos++;}
                if(hasDot)lb_add(&b,v_quantity(atof(sb.b),unit.b));else lb_add(&b,v_quantity_int(atol(sb.b),unit.b));free(unit.b);free(sb.b);continue;
            }
            /* exponent */
            if((lx_at(x)=='e'||lx_at(x)=='E') && (is_digit((unsigned char)lx_peek(x,1))||lx_peek(x,1)=='+'||lx_peek(x,1)=='-')){
                sb_add(&sb,x->s[x->pos]); x->pos++;
                if(lx_at(x)=='+'||lx_at(x)=='-'){ sb_add(&sb,lx_at(x)); x->pos++; }
                while(x->pos<x->len && is_digit((unsigned char)x->s[x->pos])){ sb_add(&sb,x->s[x->pos]); x->pos++; }
                lb_add(&b, v_float(atof(sb.b)));
            } else if(hasDot){
                char *firstDot=strchr(sb.b,'.');
                if(firstDot&&strchr(firstDot+1,'.')){
                    if((lx_at(x)=='-'||lx_at(x)=='+')&&isalnum((unsigned char)lx_peek(x,1)))
                        while(x->pos<x->len&&(isalnum((unsigned char)lx_at(x))||lx_at(x)=='.'||lx_at(x)=='-'||lx_at(x)=='+')){sb_add(&sb,lx_at(x));x->pos++;}
                    lb_add(&b,v_version(sb.b));
                }
                else lb_add(&b, v_float(atof(sb.b)));
            } else {
                lb_add(&b, v_int_from_text(sb.b));
            }
            free(sb.b);
        } else if((unsigned char)c==0xC3 && (unsigned char)lx_peek(x,1)==0xB8){
            x->pos+=2;lb_add(&b,v_null());
        } else if((unsigned char)c==0xE2 && (unsigned char)lx_peek(x,1)==0x88 && (unsigned char)lx_peek(x,2)==0x9E){
            x->pos+=3;lb_add(&b,v_float(INFINITY));
        } else if((unsigned char)c==0xC2 && (unsigned char)lx_peek(x,1)==0xAB){
            if((unsigned char)lx_peek(x,2)==0xC2 && (unsigned char)lx_peek(x,3)==0xAB){
                /* `«« ... »»` multiline string */
                SB sb;sb_init(&sb);parse_safe_string(x,&sb);lb_add(&b,v_str(sb.b));free(sb.b);
            } else {
                x->pos+=2;while(lx_at(x)==' '||lx_at(x)=='\t')x->pos++;
                SB sb;sb_init(&sb);while(x->pos<x->len&&lx_at(x)!='\r'&&lx_at(x)!='\n'){sb_add(&sb,lx_at(x));x->pos++;}
                while(sb.len>0&&(sb.b[sb.len-1]==' '||sb.b[sb.len-1]=='\t'))sb.b[--sb.len]=0;
                lb_add(&b,v_str(sb.b));free(sb.b);
            }
        } else if(c=='#' && is_id_in((unsigned char)lx_peek(x,1))){
            x->pos++;SB sb;sb_init(&sb);while(x->pos<x->len&&is_id_in((unsigned char)lx_at(x))){sb_add(&sb,lx_at(x));x->pos++;}lb_add(&b,v_color_hex(sb.b));free(sb.b);
        } else if(in_syms(c)){
            /* `---` starts a dash multiline string running to EOF: any further
             * dashes are skipped, the rest is read to EOF and dedented+stripped */
            if(c=='-' && (unsigned char)lx_peek(x,1)=='-' && (unsigned char)lx_peek(x,2)=='-'){
                SB sb; sb_init(&sb);
                x->pos+=3;
                while(x->pos<x->len && (unsigned char)lx_at(x)=='-') x->pos++;
                while(x->pos<x->len){
                    int ch=(unsigned char)lx_at(x);
                    if(ch=='\r'){ sb_add(&sb,'\n'); x->pos++; if(x->pos<x->len&&lx_at(x)=='\n')x->pos++; }
                    else if(ch=='\n'){ sb_add(&sb,'\n'); x->pos++; }
                    else { sb_add(&sb,ch); x->pos++; }
                }
                char *norm=dedent_curly_text(sb.b); free(sb.b);
                lb_add(&b, v_str(norm)); free(norm);
            } else {
                SB g; sb_init(&g); int isSym=lx_symbol(x,&g);
                if(isSym && g.len>0) lb_add(&b, v_token(V_SYMBOL, g.b));
                free(g.b);
            }
        } else if(c=='\\'){
            int n=lx_peek(x,1);
            if(is_id_start((unsigned char)n)||n=='['){
                Value root=v_token(V_WORD,"this");
                Value p=parse_path(x,root,0);
                if(lx_at(x)==':'){ x->pos++; lb_add(&b, mk_pathkind(V_PATHLABEL,p)); }
                else lb_add(&b, p);
            } else if(n=='\\'){ x->pos+=2; lb_add(&b,v_token(V_SYMBOL,"\\\\")); }
            else if(n=='/'){ x->pos+=2; lb_add(&b,v_token(V_SYMBOL,"//")); }
            else { x->pos++; lb_add(&b,v_token(V_SYMBOL,"\\")); }
        } else if(is_id_start(c)){
            SB sb; sb_init(&sb); parse_ident(x,&sb,1);
            if(sb.len==1 && sb.b[0]=='_'){ lb_add(&b,v_token(V_SYMBOL,"_")); free(sb.b); }
            else if(lx_at(x)==':'){ x->pos++; lb_add(&b, v_token(V_LABEL, sb.b)); free(sb.b); }
            else if(lx_at(x)=='\\' && (is_id_start((unsigned char)lx_peek(x,1))||is_digit((unsigned char)lx_peek(x,1))||lx_peek(x,1)=='[')){
                Value root=v_token(V_WORD, sb.b); free(sb.b);
                Value p=parse_path(x,root,0);
                if(lx_at(x)==':'){ x->pos++; lb_add(&b, mk_pathkind(V_PATHLABEL,p)); }
                else lb_add(&b, p);
            } else if(lx_at(x)=='\\'){
                x->pos++; lb_add(&b, v_token(V_SYMBOL, "\\")); free(sb.b);
            } else {
                lb_add(&b, v_token(V_WORD, sb.b)); free(sb.b);
            }
        } else if(c=='\''){
            int initialP=x->pos;
            SB sb; sb_init(&sb); parse_ident(x,&sb,0);
            if(sb.len==0){ free(sb.b);
                /* empty after tick: symbol-literal or char */
                if(in_syms(lx_at(x))){
                    SB g; sb_init(&g); lx_symbol(x,&g);
                    /* backslash-escape char like '\n' */
                    if(lx_at(x)=='\''){ x->pos++; lb_add(&b, v_char(g.b[1])); }
                    else if(!strcmp(g.b,"\\")) { /* \n style */ if(lx_at(x)=='n'){x->pos++;lb_add(&b,v_char('\n'));} else if(lx_at(x)=='t'){x->pos++;lb_add(&b,v_char('\t'));} else lb_add(&b,v_token(V_SYMBOL,g.b)); }
                    else lb_add(&b, v_token(V_SYMBOLLITERAL, g.b));
                    free(g.b);
                } else {
                    x->pos=initialP; SB s2; sb_init(&s2); parse_string(x,&s2,'\''); lb_add(&b, v_char_text(s2.b)); free(s2.b);
                }
            } else {
                if(lx_at(x)=='\\' && (is_id_start((unsigned char)lx_peek(x,1))||is_digit((unsigned char)lx_peek(x,1)))){
                    Value root=v_token(V_WORD, sb.b); free(sb.b);
                    Value p=parse_path(x,root,1);
                    lb_add(&b, mk_pathkind(V_PATHLITERAL,p));
                } else if(lx_at(x)=='\''){
                    x->pos++; lb_add(&b, v_char_text(sb.b)); free(sb.b);
                } else {
                    lb_add(&b, v_token(V_LITERAL, sb.b)); free(sb.b);
                }
            }
        } else if(c=='`'){
            x->pos++; SB sb; sb_init(&sb);
            while(x->pos<x->len && (is_id_in((unsigned char)x->s[x->pos])||x->s[x->pos]=='.'||x->s[x->pos]=='/')){ sb_add(&sb,x->s[x->pos]); x->pos++; }
            lb_add(&b, v_unit(sb.b)); free(sb.b);
        } else if(c=='.'){
            if(lx_peek(x,1)=='.'){ x->pos+=2; if(lx_at(x)=='.'){ x->pos++; lb_add(&b,v_token(V_SYMBOL,"...")); } else lb_add(&b,v_token(V_SYMBOL,"..")); }
            else if(lx_peek(x,1)=='/'){ x->pos+=2; lb_add(&b,v_token(V_SYMBOL,"./")); }
            else { SB sb; sb_init(&sb); parse_ident(x,&sb,0); if(lx_at(x)==':'){ x->pos++; lb_add(&b,v_token(V_ATTRIBUTELABEL,sb.b)); } else lb_add(&b,v_token(V_ATTRIBUTE,sb.b)); free(sb.b); }
        } else if(c=='['){
            x->pos++; Value sub=parse_block(x,level+1,1,0); lb_add(&b, sub);
        } else if(c==']'){
            if(isSubBlock){ x->pos++; break; }
            else break; /* stray */
        } else if(c=='('){
            x->pos++; Value sub=parse_block(x,level+1,0,1); lb_add(&b, sub);
        } else if(c==')'){
            if(isSubInline){ x->pos++; break; }
            else break;
        } else if(c=='{'){
            lb_add(&b, parse_curly(x));
        } else {
            x->pos++; /* skip unknown byte */
        }
    }
    Value r=bld_block(&b);
    g_line_capture = oldCapture;
    if(isSubInline) r.k=V_INLINE;
    return r;
}

Value lex_source(const char *s){
    LX x; x.s=s; x.pos=0; x.len=(int)strlen(s);
    line_count_reset();
    line_map_reset();
    return parse_block(&x,0,0,0);
}

int runtime_line_of(const char *src, int index){
    if(!src) return 0;
    if(index>=0 && index<g_line_map.n) return g_line_map.lines[index];
    return 0;
}
