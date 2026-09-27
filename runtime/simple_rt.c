#include "simple_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ---------- Memory + Mark-and-Sweep GC ---------- */

typedef struct GCHeader {
    struct GCHeader *next;
    size_t size;
    int marked;
    void (*finalizer)(void *);
} GCHeader;

static GCHeader *s_gc_heap = NULL;
static size_t s_gc_allocated = 0;
static size_t s_gc_threshold = 1024 * 1024; /* 1 MB */
static void ***s_gc_roots = NULL;
static size_t s_gc_root_count = 0;
static size_t s_gc_root_cap = 0;

void *s_alloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}
void s_free(void *p) { free(p); }

void s_gc_set_finalizer(void *p, void (*fn)(void *)) {
    if (!p) return;
    GCHeader *h = ((GCHeader *)p) - 1;
    h->finalizer = fn;
}

void *s_gc_alloc(size_t n) {
    if (s_gc_allocated + n > s_gc_threshold)
        s_gc_collect();
    GCHeader *h = (GCHeader *)calloc(1, sizeof(GCHeader) + n);
    if (!h) {
        s_gc_collect();
        h = (GCHeader *)calloc(1, sizeof(GCHeader) + n);
        if (!h) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    h->size = n;
    h->marked = 0;
    h->next = s_gc_heap;
    s_gc_heap = h;
    s_gc_allocated += n;
    return (void *)(h + 1);
}

void s_gc_add_root(void **root) {
    if (s_gc_root_count >= s_gc_root_cap) {
        s_gc_root_cap = s_gc_root_cap ? s_gc_root_cap * 2 : 16;
        s_gc_roots = (void ***)realloc(s_gc_roots, s_gc_root_cap * sizeof(void **));
    }
    s_gc_roots[s_gc_root_count++] = root;
}

void s_gc_remove_root(void **root) {
    for (size_t i = 0; i < s_gc_root_count; i++) {
        if (s_gc_roots[i] == root) {
            s_gc_roots[i] = s_gc_roots[--s_gc_root_count];
            return;
        }
    }
}

static int s_gc_points_to(void *ptr, GCHeader *h) {
    void *payload = (void *)(h + 1);
    return ptr >= payload && (char *)ptr < (char *)payload + h->size;
}

static void s_gc_mark_ptr(void *ptr) {
    if (!ptr) return;
    for (GCHeader *h = s_gc_heap; h; h = h->next) {
        if (s_gc_points_to(ptr, h) || ptr == (void *)(h + 1)) {
            if (h->marked) return;
            h->marked = 1;
            /* conservative: scan payload for pointers */
            void **slots = (void **)(h + 1);
            size_t nslots = h->size / sizeof(void *);
            for (size_t i = 0; i < nslots; i++)
                s_gc_mark_ptr(slots[i]);
            return;
        }
    }
}

void s_gc_collect(void) {
    /* clear marks */
    for (GCHeader *h = s_gc_heap; h; h = h->next)
        h->marked = 0;
    /* mark from roots */
    for (size_t i = 0; i < s_gc_root_count; i++) {
        if (s_gc_roots[i] && *s_gc_roots[i])
            s_gc_mark_ptr(*s_gc_roots[i]);
    }
    /* sweep */
    GCHeader **pp = &s_gc_heap;
    size_t still = 0;
    while (*pp) {
        GCHeader *h = *pp;
        if (!h->marked) {
            *pp = h->next;
            still += 0;
            if (h->finalizer) h->finalizer((void *)(h + 1));
            free(h);
        } else {
            h->marked = 0;
            still += h->size;
            pp = &h->next;
        }
    }
    s_gc_allocated = still;
}

size_t s_gc_live_count(void) {
    size_t n = 0;
    for (GCHeader *h = s_gc_heap; h; h = h->next) n++;
    return n;
}

void s_gc_set_threshold(size_t bytes) {
    s_gc_threshold = bytes ? bytes : 1024 * 1024;
}

/* ---------- Object ---------- */
SObject *s_object_retain(SObject *o) {
    return o; /* GC manages lifetime */
}
void s_object_release(SObject *o) {
    (void)o; /* GC manages lifetime */
}
s_bool s_object_is_a(SObject *o, const SClassInfo *cls) {
    if (!o || !cls) return false;
    const SClassInfo *c = o->cls;
    while (c) {
        if (c == cls) return true;
        c = c->parent;
    }
    return false;
}
const char *s_object_class_name(SObject *o) {
    return (o && o->cls) ? o->cls->name : "null";
}

/* ---------- Strings ---------- */
s_string s_str_from_cstr(const char *c) {
    if (!c) c = "";
    size_t n = strlen(c);
    char *s = (char *)s_gc_alloc(n + 1);
    memcpy(s, c, n + 1);
    return s;
}
s_string s_str_concat(s_string a, s_string b) {
    size_t la = a ? strlen(a) : 0, lb = b ? strlen(b) : 0;
    char *r = (char *)s_gc_alloc(la + lb + 1);
    if (a) memcpy(r, a, la);
    if (b) memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return r;
}
s_string s_str_from_int(s_int v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return s_str_from_cstr(buf);
}
s_string s_str_from_float(s_float v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", v);
    return s_str_from_cstr(buf);
}
s_int s_str_length(s_string s) { return s ? (s_int)strlen(s) : 0; }
s_int s_str_compare(s_string a, s_string b) {
    if (!a) a = "";
    if (!b) b = "";
    return (s_int)strcmp(a, b);
}
s_string s_str_substring(s_string s, s_int start, s_int len) {
    if (!s) return s_str_from_cstr("");
    s_int n = (s_int)strlen(s);
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (len < 0) len = 0;
    if (start + len > n) len = n - start;
    char *r = (char *)s_gc_alloc((size_t)len + 1);
    memcpy(r, s + start, (size_t)len);
    r[len] = '\0';
    return r;
}
void s_str_free(s_string s) { free(s); }

/* ---------- List ---------- */
SList *s_list_create(void) {
    return (SList *)s_gc_alloc(sizeof(SList));
}
void s_list_push(SList *l, void *item) {
    if (!l) return;
    if (l->count >= l->capacity) {
        size_t nc = l->capacity ? l->capacity * 2 : 8;
        l->items = (void **)realloc(l->items, nc * sizeof(void *));
        l->capacity = nc;
    }
    l->items[l->count++] = item;
}
void *s_list_get(SList *l, s_int index) {
    if (!l || index < 0 || (size_t)index >= l->count) return NULL;
    return l->items[index];
}
void s_list_set(SList *l, s_int index, void *item) {
    if (!l || index < 0 || (size_t)index >= l->count) return;
    l->items[index] = item;
}
s_int s_list_length(SList *l) { return l ? (s_int)l->count : 0; }
void s_list_free(SList *l) {
    if (!l) return;
    free(l->items);
    free(l);
}

/* ---------- Dict ---------- */
static unsigned s_hash(const char *s) {
    unsigned h = 5381;
    if (!s) return h;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h;
}
SDict *s_dict_create(void) {
    SDict *d = (SDict *)s_gc_alloc(sizeof(SDict));
    d->capacity = 16;
    d->entries = (SDictEntry *)s_gc_alloc(d->capacity * sizeof(SDictEntry));
    return d;
}
void s_dict_put(SDict *d, s_string key, void *value) {
    if (!d) return;
    if (d->count * 2 >= d->capacity) {
        size_t nc = d->capacity * 2;
        SDictEntry *ne = (SDictEntry *)s_gc_alloc(nc * sizeof(SDictEntry));
        for (size_t i = 0; i < d->capacity; i++) {
            if (!d->entries[i].used) continue;
            unsigned h = s_hash(d->entries[i].key) % nc;
            while (ne[h].used) h = (h + 1) % nc;
            ne[h] = d->entries[i];
        }
        free(d->entries);
        d->entries = ne;
        d->capacity = nc;
    }
    unsigned h = s_hash(key) % d->capacity;
    while (d->entries[h].used && strcmp(d->entries[h].key, key ? key : "") != 0)
        h = (h + 1) % d->capacity;
    if (!d->entries[h].used) {
        d->entries[h].key = s_str_from_cstr(key ? key : "");
        d->count++;
    }
    d->entries[h].value = value;
    d->entries[h].used = 1;
}
void *s_dict_get(SDict *d, s_string key) {
    if (!d) return NULL;
    unsigned h = s_hash(key) % d->capacity;
    for (size_t i = 0; i < d->capacity; i++) {
        size_t idx = (h + i) % d->capacity;
        if (!d->entries[idx].used) return NULL;
        if (strcmp(d->entries[idx].key, key ? key : "") == 0)
            return d->entries[idx].value;
    }
    return NULL;
}
s_bool s_dict_has(SDict *d, s_string key) { return s_dict_get(d, key) != NULL; }
s_int s_dict_length(SDict *d) { return d ? (s_int)d->count : 0; }
void s_dict_free(SDict *d) {
    if (!d) return;
    for (size_t i = 0; i < d->capacity; i++)
        if (d->entries[i].used) free(d->entries[i].key);
    free(d->entries);
    free(d);
}

/* ---------- Math ---------- */
s_float s_math_sin(s_float x)   { return sin(x); }
s_float s_math_cos(s_float x)   { return cos(x); }
s_float s_math_tan(s_float x)   { return tan(x); }
s_float s_math_sqrt(s_float x)  { return sqrt(x); }
s_float s_math_pow(s_float b, s_float e) { return pow(b, e); }
s_float s_math_log(s_float x)   { return log(x); }
s_float s_math_exp(s_float x)   { return exp(x); }
s_float s_math_fabs(s_float x)  { return fabs(x); }
s_float s_math_floor(s_float x) { return floor(x); }
s_float s_math_ceil(s_float x)  { return ceil(x); }
s_int   s_math_abs(s_int x)     { return x < 0 ? -x : x; }
s_int   s_math_min(s_int a, s_int b) { return a < b ? a : b; }
s_int   s_math_max(s_int a, s_int b) { return a > b ? a : b; }

/* ---------- I/O ---------- */
void s_print_str(s_string s)   { fputs(s ? s : "", stdout); }
void s_print_int(s_int v)      { printf("%lld", (long long)v); }
void s_print_float(s_float v)  { printf("%g", v); }
void s_print_bool(s_bool v)    { fputs(v ? "true" : "false", stdout); }
void s_print_ln(void)          { fputc('\n', stdout); }
s_string s_read_line(void) {
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) return s_str_from_cstr("");
    size_t n = strlen(buf);
    if (n && buf[n-1] == '\n') buf[n-1] = '\0';
    return s_str_from_cstr(buf);
}

/* ---------- Casts ---------- */
s_int   s_cast_float_to_int(s_float v)    { return (s_int)v; }
s_float s_cast_int_to_float(s_int v)      { return (s_float)v; }
s_string s_cast_int_to_string(s_int v)    { return s_str_from_int(v); }
s_string s_cast_float_to_string(s_float v){ return s_str_from_float(v); }
s_int s_cast_string_to_int(s_string s) {
    if (!s) return 0;
    return (s_int)strtoll(s, NULL, 10);
}
s_float s_cast_string_to_float(s_string s) {
    if (!s) return 0.0;
    return strtod(s, NULL);
}
s_bool s_cast_int_to_bool(s_int v) { return v != 0; }

/* ---------- Exceptions ---------- */
static SExceptFrame s_except_stack[S_MAX_EXCEPT_FRAMES];
static int s_except_sp = 0;
static SException s_current_exception = {NULL, NULL};

jmp_buf *s_except_enter(void) {
    if (s_except_sp >= S_MAX_EXCEPT_FRAMES) {
        fprintf(stderr, "exception stack overflow\n");
        exit(1);
    }
    return &s_except_stack[s_except_sp++].env;
}

void s_except_leave(void) {
    if (s_except_sp > 0) s_except_sp--;
}

void s_raise(const char *name) {
    s_current_exception.name = name ? name : "Error";
    s_current_exception.data = NULL;
    if (s_except_sp > 0) {
        longjmp(s_except_stack[s_except_sp - 1].env, 1);
    }
    fprintf(stderr, "Unhandled exception: %s\n", s_current_exception.name);
    exit(1);
}

void s_raise_obj(SObject *obj) {
    s_current_exception.name = s_object_class_name(obj);
    s_current_exception.data = obj;
    if (s_except_sp > 0) {
        longjmp(s_except_stack[s_except_sp - 1].env, 1);
    }
    fprintf(stderr, "Unhandled exception object: %s\n", s_current_exception.name);
    exit(1);
}

const SException *s_except_current(void) {
    return &s_current_exception;
}

void s_except_clear(void) {
    s_current_exception.name = NULL;
    s_current_exception.data = NULL;
}

s_bool s_except_matches(const char *name) {
    if (!s_current_exception.name) return false;
    if (!name) return true;
    return strcmp(s_current_exception.name, name) == 0;
}


/* ---------- Formatted strings ---------- */
#define S_FMT_MAX_ARGS 32
static char *s_fmt_template = NULL;
static struct {
    char name[64];
    int has_name;
    char *value;
    int used;
} s_fmt_args[S_FMT_MAX_ARGS];
static int s_fmt_nargs = 0;
static int s_fmt_pos = 0;

void s_str_fmt_start(s_string fmt) {
    free(s_fmt_template);
    s_fmt_template = strdup(fmt ? fmt : "");
    s_fmt_nargs = 0;
    s_fmt_pos = 0;
    for (int i = 0; i < S_FMT_MAX_ARGS; i++) {
        s_fmt_args[i].used = 0;
        s_fmt_args[i].has_name = 0;
        s_fmt_args[i].name[0] = '\0';
        s_fmt_args[i].value = NULL;
    }
}

void s_str_fmt_put(const char *name, s_string value) {
    if (s_fmt_nargs >= S_FMT_MAX_ARGS) return;
    int i = s_fmt_nargs++;
    if (name && name[0]) {
        s_fmt_args[i].has_name = 1;
        strncpy(s_fmt_args[i].name, name, 63);
        s_fmt_args[i].name[63] = '\0';
    } else {
        s_fmt_args[i].has_name = 0;
    }
    free(s_fmt_args[i].value);
    s_fmt_args[i].value = strdup(value ? value : "");
    s_fmt_args[i].used = 0;
}

static const char *s_fmt_next_positional(void) {
    while (s_fmt_pos < s_fmt_nargs) {
        if (!s_fmt_args[s_fmt_pos].has_name && !s_fmt_args[s_fmt_pos].used) {
            s_fmt_args[s_fmt_pos].used = 1;
            return s_fmt_args[s_fmt_pos++].value;
        }
        s_fmt_pos++;
    }
    return "";
}

static const char *s_fmt_named(const char *name, size_t len) {
    char key[64];
    if (len >= sizeof(key)) len = sizeof(key) - 1;
    memcpy(key, name, len);
    key[len] = '\0';
    for (int i = 0; i < s_fmt_nargs; i++) {
        if (s_fmt_args[i].has_name && strcmp(s_fmt_args[i].name, key) == 0) {
            s_fmt_args[i].used = 1;
            return s_fmt_args[i].value ? s_fmt_args[i].value : "";
        }
    }
    return "";
}

s_string s_str_fmt_finish(void) {
    const char *p = s_fmt_template ? s_fmt_template : "";
    size_t cap = 128;
    size_t len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return s_str_from_cstr("");
    out[0] = '\0';

    while (*p) {
        if (*p == '{' && p[1] == '{') {
            /* escaped {{ */
            if (len + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
            out[len++] = '{';
            p += 2;
            continue;
        }
        if (*p == '}' && p[1] == '}') {
            if (len + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
            out[len++] = '}';
            p += 2;
            continue;
        }
        if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (!end) {
                /* literal { */
                if (len + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
                out[len++] = *p++;
                continue;
            }
            const char *rep;
            if (end == p + 1) {
                rep = s_fmt_next_positional();
            } else {
                rep = s_fmt_named(p + 1, (size_t)(end - p - 1));
            }
            size_t rl = strlen(rep);
            while (len + rl + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
            memcpy(out + len, rep, rl);
            len += rl;
            out[len] = '\0';
            p = end + 1;
            continue;
        }
        if (len + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
        out[len++] = *p++;
        out[len] = '\0';
    }

    s_string result = s_str_from_cstr(out);
    free(out);
    free(s_fmt_template);
    s_fmt_template = NULL;
    for (int i = 0; i < s_fmt_nargs; i++)
        free(s_fmt_args[i].value);
    s_fmt_nargs = 0;
    return result;
}

s_string s_str_format2(s_string fmt, s_string a0, s_string a1) {
    s_str_fmt_start(fmt);
    s_str_fmt_put(NULL, a0);
    s_str_fmt_put(NULL, a1);
    return s_str_fmt_finish();
}

s_string s_str_format3(s_string fmt, s_string a0, s_string a1, s_string a2) {
    s_str_fmt_start(fmt);
    s_str_fmt_put(NULL, a0);
    s_str_fmt_put(NULL, a1);
    s_str_fmt_put(NULL, a2);
    return s_str_fmt_finish();
}


void *s_box_int(s_int v) {
    s_int *p = (s_int *)s_gc_alloc(2 * sizeof(s_int));
    if (p) { p[0] = 1; p[1] = v; }
    return p;
}
s_int s_unbox_int(void *p) {
    if (!p) return 0;
    s_int *x = (s_int *)p;
    return (x[0] == 1) ? x[1] : 0;
}
int s_is_int_box(void *p) {
    if (!p) return 0;
    return ((s_int *)p)[0] == 1;
}

void *s_box_float(s_float v) {
    s_float *p = (s_float *)s_gc_alloc(sizeof(s_float));
    if (p) *p = v;
    return p;
}
s_float s_unbox_float(void *p) { return p ? *(s_float *)p : 0.0; }

void *s_box_bool(s_bool v) {
    s_bool *p = (s_bool *)s_gc_alloc(sizeof(s_bool));
    if (p) *p = v;
    return p;
}
s_bool s_unbox_bool(void *p) { return p ? *(s_bool *)p : 0; }

s_string s_key_from_int(s_int v) { return s_str_from_int(v); }


SGen *s_gen_create(int (*step)(SGen *)) {
    SGen *g = (SGen *)s_gc_alloc(sizeof(SGen));
    if (!g) return NULL;
    g->state = 0;
    g->done = 0;
    g->value = NULL;
    g->step = step;
    for (int i = 0; i < 16; i++) g->ivals[i] = 0;
    for (int i = 0; i < 8; i++) g->pvals[i] = NULL;
    return g;
}

int s_gen_next(SGen *g) {
    if (!g || g->done || !g->step) return 0;
    int r = g->step(g);
    if (r == 0) g->done = 1;
    return r;
}

void *s_gen_value(SGen *g) { return g ? g->value : NULL; }
s_int s_gen_int(SGen *g) { return s_unbox_int(g ? g->value : NULL); }
void  s_gen_set_int(SGen *g, int slot, s_int v) { if (g && slot >= 0 && slot < 16) g->ivals[slot] = v; }
s_int s_gen_get_int(SGen *g, int slot) { return (g && slot >= 0 && slot < 16) ? g->ivals[slot] : 0; }
void  s_gen_set_ptr(SGen *g, int slot, void *p) { if (g && slot >= 0 && slot < 8) g->pvals[slot] = p; }
void *s_gen_get_ptr(SGen *g, int slot) { return (g && slot >= 0 && slot < 8) ? g->pvals[slot] : NULL; }

void s_gen_yield(SGen *g, void *value) { if (g) g->value = value; }
void s_gen_set_state(SGen *g, int state) { if (g) g->state = state; }
int  s_gen_get_state(SGen *g) { return g ? g->state : 0; }
void s_gen_set_done(SGen *g) { if (g) g->done = 1; }

const char *s_except_name(void) {
    const SException *e = s_except_current();
    return (e && e->name) ? e->name : "";
}


enum { S_CN_CAP = 256 };
static void *s_cn_ptr[S_CN_CAP];
static const char *s_cn_name[S_CN_CAP];
static int s_cn_n = 0;

void s_set_classname(void *obj, const char *name) {
    if (!obj) return;
    for (int i = 0; i < s_cn_n; i++)
        if (s_cn_ptr[i] == obj) { s_cn_name[i] = name; return; }
    if (s_cn_n < S_CN_CAP) {
        s_cn_ptr[s_cn_n] = obj;
        s_cn_name[s_cn_n] = name;
        s_cn_n++;
    }
}
const char *s_get_classname(void *obj) {
    for (int i = 0; i < s_cn_n; i++)
        if (s_cn_ptr[i] == obj) return s_cn_name[i];
    return "";
}
static const char *s_inh_child[64];
static const char *s_inh_base[64];
static int s_inh_n = 0;

void s_register_base(const char *cls, const char *base) {
    if (s_inh_n < 64) { s_inh_child[s_inh_n] = cls; s_inh_base[s_inh_n] = base; s_inh_n++; }
}

s_bool s_is_classname(void *obj, const char *name) {
    if (!obj || !name) return 0;
    const char *have = s_get_classname(obj);
    if (have && strcmp(have, name) == 0) return 1;
    for (int step = 0; step < 16 && have && have[0]; ++step) {
        const char *next = NULL;
        for (int i = 0; i < s_inh_n; i++) {
            if (s_inh_child[i] && strcmp(s_inh_child[i], have) == 0) {
                next = s_inh_base[i];
                if (next && strcmp(next, name) == 0) return 1;
            }
        }
        if (!next) break;
        have = next;
    }
    return 0;
}


enum { S_MEM_CAP = 256 };
static const char *s_mem_cls[S_MEM_CAP];
static const char *s_mem_name[S_MEM_CAP];
static int s_mem_vis[S_MEM_CAP];
static int s_mem_n = 0;

void s_reg_member(const char *cls, const char *member, int vis) {
    if (s_mem_n >= S_MEM_CAP || !cls || !member) return;
    s_mem_cls[s_mem_n] = cls;
    s_mem_name[s_mem_n] = member;
    s_mem_vis[s_mem_n] = vis;
    s_mem_n++;
}

static int s_is_subclass(const char *child, const char *anc) {
    if (!child || !anc) return 0;
    if (strcmp(child, anc) == 0) return 1;
    const char *have = child;
    for (int step = 0; step < 16; ++step) {
        const char *next = NULL;
        for (int i = 0; i < s_inh_n; i++) {
            if (s_inh_child[i] && strcmp(s_inh_child[i], have) == 0) {
                next = s_inh_base[i];
                if (next && strcmp(next, anc) == 0) return 1;
            }
        }
        if (!next) break;
        have = next;
    }
    return 0;
}

static int s_lookup_member(const char *owner, const char *member, int *vis_out, const char **owner_out) {
    /* search owner then bases */
    const char *cls = owner;
    for (int step = 0; step < 16 && cls; ++step) {
        for (int i = 0; i < s_mem_n; i++) {
            if (s_mem_cls[i] && s_mem_name[i] &&
                strcmp(s_mem_cls[i], cls) == 0 && strcmp(s_mem_name[i], member) == 0) {
                if (vis_out) *vis_out = s_mem_vis[i];
                if (owner_out) *owner_out = s_mem_cls[i];
                return 1;
            }
        }
        const char *next = NULL;
        for (int i = 0; i < s_inh_n; i++)
            if (s_inh_child[i] && strcmp(s_inh_child[i], cls) == 0) { next = s_inh_base[i]; break; }
        cls = next;
    }
    return 0;
}

int s_check_access(const char *accessor, const char *owner, const char *member) {
    int vis = 0;
    const char *decl = owner;
    if (!s_lookup_member(owner, member, &vis, &decl))
        return 1; /* unknown member: allow (public by default) */
    if (vis == 0) return 1;
    const char *acc = accessor ? accessor : "";
    if (vis == 2) /* private */
        return acc[0] && strcmp(acc, decl) == 0;
    /* protected */
    return acc[0] && s_is_subclass(acc, decl);
}

void s_access_or_die(const char *accessor, const char *owner, const char *member) {
    if (s_check_access(accessor, owner, member)) return;
    fprintf(stderr, "runtime access error: '%s' cannot access %s::%s\n",
            accessor && accessor[0] ? accessor : "<module>",
            owner ? owner : "?", member ? member : "?");
    exit(1);
}
