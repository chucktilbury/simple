#ifndef SIMPLE_RT_H
#define SIMPLE_RT_H

/*
 * Simple language runtime – C ABI used by the LLVM backend.
 * Provides: objects, strings, lists, dicts, math, I/O, exceptions stubs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Core types ---------- */
typedef int64_t   s_int;
typedef uint64_t  s_uint;
typedef double    s_float;
typedef bool      s_bool;
typedef char*     s_string;

/* ---------- Object model ---------- */
typedef struct SClassInfo SClassInfo;
typedef struct SObject SObject;

struct SClassInfo {
    const char *name;
    const SClassInfo *parent;   /* single inheritance */
    size_t instance_size;
    void (*destroy)(SObject *self);
    /* optional vtable slot count / pointers follow in generated code */
};

struct SObject {
    const SClassInfo *cls;
    int32_t mark;               /* GC mark bit */
    int32_t _pad;
};

#define S_OBJECT_HEADER SObject _sobj;

static inline void s_object_init(SObject *o, const SClassInfo *cls) {
    o->cls = cls;
    o->mark = 0;
}

SObject *s_object_retain(SObject *o);   /* no-op under GC; kept for ABI */
void     s_object_release(SObject *o);  /* no-op under GC; kept for ABI */
s_bool   s_object_is_a(SObject *o, const SClassInfo *cls);
const char *s_object_class_name(SObject *o);
void        s_set_classname(void *obj, const char *name);
const char *s_get_classname(void *obj);
s_bool      s_is_classname(void *obj, const char *name);
void        s_register_base(const char *cls, const char *base);
void        s_reg_member(const char *cls, const char *member, int vis); /* 0 pub 1 prot 2 priv */
int         s_check_access(const char *accessor, const char *owner, const char *member);
void        s_access_or_die(const char *accessor, const char *owner, const char *member);




/* ---------- Memory + Garbage Collection ---------- */
/* All heap objects for GC must be allocated via s_gc_alloc. */
void *s_alloc(size_t n);          /* raw calloc (non-GC) */
void  s_free(void *p);

/* GC-managed allocation (objects, strings, lists, dicts) */
void *s_gc_alloc(size_t n);
void  s_gc_set_finalizer(void *p, void (*fn)(void *));
void  s_gc_collect(void);         /* force a collection cycle */
void  s_gc_add_root(void **root); /* register a stack/global root pointer */
void  s_gc_remove_root(void **root);
size_t s_gc_live_count(void);
void  s_gc_set_threshold(size_t bytes); /* collect when allocated exceeds this */

/* ---------- Strings ---------- */
s_string s_str_from_cstr(const char *c);
s_string s_str_concat(s_string a, s_string b);
s_string s_str_from_int(s_int v);
s_string s_str_from_float(s_float v);
s_int    s_str_length(s_string s);
s_int    s_str_compare(s_string a, s_string b);
s_string s_str_substring(s_string s, s_int start, s_int len);
void     s_str_free(s_string s);

/* Formatted strings: "a {} {name}" with positional + named args.
 * Call s_str_fmt_start(fmt), then s_str_fmt_put(name_or_null, value_as_string)
 * for each arg in order, then s_str_fmt_finish(). */
void     s_str_fmt_start(s_string fmt);
void     s_str_fmt_put(const char *name, s_string value);
s_string s_str_fmt_finish(void);
s_string s_str_format2(s_string fmt, s_string a0, s_string a1);
s_string s_str_format3(s_string fmt, s_string a0, s_string a1, s_string a2);


/* ---------- List (dynamic array of void*) ---------- */
typedef struct {
    void **items;
    size_t count;
    size_t capacity;
} SList;

SList   *s_list_create(void);
void     s_list_push(SList *l, void *item);
void    *s_list_get(SList *l, s_int index);
void     s_list_set(SList *l, s_int index, void *item);
s_int    s_list_length(SList *l);
void     s_list_free(SList *l);

/* ---------- Dict (string → void*) ---------- */
typedef struct {
    char *key;
    void *value;
    int used;
} SDictEntry;

typedef struct {
    SDictEntry *entries;
    size_t capacity;
    size_t count;
} SDict;

SDict   *s_dict_create(void);
void     s_dict_put(SDict *d, s_string key, void *value);
void    *s_dict_get(SDict *d, s_string key);
s_bool   s_dict_has(SDict *d, s_string key);
s_int    s_dict_length(SDict *d);
void     s_dict_free(SDict *d);

/* Box primitives so lists/dicts can store them as void* */
void    *s_box_int(s_int v);
s_int    s_unbox_int(void *p);
void    *s_box_float(s_float v);
s_float  s_unbox_float(void *p);
void    *s_box_bool(s_bool v);
s_bool   s_unbox_bool(void *p);
s_string s_key_from_int(s_int v);

/* Generators / iterators (yield) */
typedef struct SGen {
    int state;
    int done;
    void *value;
    s_int ivals[16];
    void *pvals[8];
    int (*step)(struct SGen *);
} SGen;

SGen *s_gen_create(int (*step)(struct SGen *));
int   s_gen_next(SGen *g);     /* 1 = yielded a value, 0 = done */
void *s_gen_value(SGen *g);
s_int s_gen_int(SGen *g);
void  s_gen_set_int(SGen *g, int slot, s_int v);
s_int s_gen_get_int(SGen *g, int slot);
void  s_gen_set_ptr(SGen *g, int slot, void *p);
void *s_gen_get_ptr(SGen *g, int slot);
void  s_gen_yield(SGen *g, void *value);
void  s_gen_set_state(SGen *g, int state);
int   s_gen_get_state(SGen *g);
void  s_gen_set_done(SGen *g);




/* ---------- Math ---------- */
s_float s_math_sin(s_float x);
s_float s_math_cos(s_float x);
s_float s_math_tan(s_float x);
s_float s_math_sqrt(s_float x);
s_float s_math_pow(s_float base, s_float exp);
s_float s_math_log(s_float x);
s_float s_math_exp(s_float x);
s_float s_math_fabs(s_float x);
s_float s_math_floor(s_float x);
s_float s_math_ceil(s_float x);
s_int   s_math_abs(s_int x);
s_int   s_math_min(s_int a, s_int b);
s_int   s_math_max(s_int a, s_int b);

/* ---------- I/O ---------- */
void s_print_str(s_string s);
void s_print_int(s_int v);
void s_print_float(s_float v);
void s_print_bool(s_bool v);
void s_print_ln(void);
s_string s_read_line(void);

/* ---------- Casting helpers ---------- */
s_int   s_cast_float_to_int(s_float v);
s_float s_cast_int_to_float(s_int v);
s_string s_cast_int_to_string(s_int v);
s_string s_cast_float_to_string(s_float v);
s_int   s_cast_string_to_int(s_string s);
s_float s_cast_string_to_float(s_string s);
s_bool  s_cast_int_to_bool(s_int v);

/* ---------- Exceptions (setjmp / longjmp) ---------- */
#include <setjmp.h>

#define S_MAX_EXCEPT_FRAMES 64

typedef struct {
    const char *name;     /* exception type name, e.g. "IOError" */
    SObject *data;        /* optional payload */
} SException;

typedef struct {
    jmp_buf env;
} SExceptFrame;

/* Enter a try region: returns pointer to jmp_buf for setjmp.
 * Use: if (setjmp(*s_except_enter()) == 0) { try-body } else { handlers }
 * Always call s_except_leave() when leaving the try region (including finally).
 */
jmp_buf *s_except_enter(void);
void     s_except_leave(void);
void     s_raise(const char *name);
void     s_raise_obj(SObject *obj);
const SException *s_except_current(void);
void     s_except_clear(void);
s_bool   s_except_matches(const char *name);
const char *s_except_name(void);


#ifdef __cplusplus
}
#endif

#endif /* SIMPLE_RT_H */

int s_is_int_box(void *p);
