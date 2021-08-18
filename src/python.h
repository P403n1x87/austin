// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018 Gabriele N. Tornetta <phoenix1987@gmail.com>.
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// COPYRIGHT NOTICE: The content of this file is composed of different parts
//                   taken from different versions of the source code of
//                   Python. The authors of those sources hold the copyright
//                   for most of the content of this header file.

#ifndef PYTHON_H
#define PYTHON_H

#include <stdint.h>
#include <stdlib.h>


// ---- object.h --------------------------------------------------------------

#define PyObject_HEAD                   PyObject    ob_base;
#define PyObject_VAR_HEAD               PyVarObject ob_base;

#ifdef Py_TRACE_REFS
#define _PyObject_HEAD_EXTRA            \
    struct _object *_ob_next;           \
    struct _object *_ob_prev;

#define _PyObject_EXTRA_INIT 0, 0,

#else
#define _PyObject_HEAD_EXTRA
#define _PyObject_EXTRA_INIT
#endif


typedef ssize_t Py_ssize_t;

typedef struct _object {
    _PyObject_HEAD_EXTRA
    ssize_t ob_refcnt;
    struct _typeobject *ob_type;
} PyObject;


typedef struct {
    PyObject ob_base;
    Py_ssize_t ob_size; /* Number of items in variable part */
} PyVarObject;


// ---- code.h ----------------------------------------------------------------

typedef struct {
    PyObject_HEAD
    int co_argcount;		/* #arguments, except *args */
    int co_nlocals;		/* #local variables */
    int co_stacksize;		/* #entries needed for evaluation stack */
    int co_flags;		/* CO_..., see below */
    PyObject *co_code;		/* instruction opcodes */
    PyObject *co_consts;	/* list (constants used) */
    PyObject *co_names;		/* list of strings (names used) */
    PyObject *co_varnames;	/* tuple of strings (local variable names) */
    PyObject *co_freevars;	/* tuple of strings (free variable names) */
    PyObject *co_cellvars;      /* tuple of strings (cell variable names) */
    PyObject *co_filename;	/* string (where it was loaded from) */
    PyObject *co_name;		/* string (name, for reference) */
    int co_firstlineno;		/* first source line number */
    PyObject *co_lnotab;	/* string (encoding addr<->lineno mapping) */
} PyCodeObject2;

typedef struct {
    PyObject_HEAD
    int co_argcount;		/* #arguments, except *args */
    int co_kwonlyargcount;	/* #keyword only arguments */
    int co_nlocals;		/* #local variables */
    int co_stacksize;		/* #entries needed for evaluation stack */
    int co_flags;		/* CO_..., see below */
    PyObject *co_code;		/* instruction opcodes */
    PyObject *co_consts;	/* list (constants used) */
    PyObject *co_names;		/* list of strings (names used) */
    PyObject *co_varnames;	/* tuple of strings (local variable names) */
    PyObject *co_freevars;	/* tuple of strings (free variable names) */
    PyObject *co_cellvars;      /* tuple of strings (cell variable names) */
    unsigned char *co_cell2arg; /* Maps cell vars which are arguments. */
    PyObject *co_filename;	/* unicode (where it was loaded from) */
    PyObject *co_name;		/* unicode (name, for reference) */
    int co_firstlineno;		/* first source line number */
    PyObject *co_lnotab;	/* string (encoding addr<->lineno mapping) */
} PyCodeObject3_3;

typedef struct {
    PyObject_HEAD
    int co_argcount;		/* #arguments, except *args */
    int co_kwonlyargcount;	/* #keyword only arguments */
    int co_nlocals;		/* #local variables */
    int co_stacksize;		/* #entries needed for evaluation stack */
    int co_flags;		/* CO_..., see below */
    int co_firstlineno;   /* first source line number */
    PyObject *co_code;		/* instruction opcodes */
    PyObject *co_consts;	/* list (constants used) */
    PyObject *co_names;		/* list of strings (names used) */
    PyObject *co_varnames;	/* tuple of strings (local variable names) */
    PyObject *co_freevars;	/* tuple of strings (free variable names) */
    PyObject *co_cellvars;      /* tuple of strings (cell variable names) */
    unsigned char *co_cell2arg; /* Maps cell vars which are arguments. */
    PyObject *co_filename;	/* unicode (where it was loaded from) */
    PyObject *co_name;		/* unicode (name, for reference) */
    PyObject *co_lnotab;	/* string (encoding addr<->lineno mapping) */
} PyCodeObject3_6;

typedef struct {
    PyObject_HEAD
    int co_argcount;            /* #arguments, except *args */
    int co_posonlyargcount;     /* #positional only arguments */
    int co_kwonlyargcount;      /* #keyword only arguments */
    int co_nlocals;             /* #local variables */
    int co_stacksize;           /* #entries needed for evaluation stack */
    int co_flags;               /* CO_..., see below */
    int co_firstlineno;         /* first source line number */
    PyObject *co_code;          /* instruction opcodes */
    PyObject *co_consts;        /* list (constants used) */
    PyObject *co_names;         /* list of strings (names used) */
    PyObject *co_varnames;      /* tuple of strings (local variable names) */
    PyObject *co_freevars;      /* tuple of strings (free variable names) */
    PyObject *co_cellvars;      /* tuple of strings (cell variable names) */
    Py_ssize_t *co_cell2arg;    /* Maps cell vars which are arguments. */
    PyObject *co_filename;      /* unicode (where it was loaded from) */
    PyObject *co_name;          /* unicode (name, for reference) */
    PyObject *co_lnotab;        /* string (encoding addr<->lineno mapping) */
} PyCodeObject3_8;

typedef union {
  PyCodeObject2   v2;
  PyCodeObject3_3 v3_3;
  PyCodeObject3_6 v3_6;
  PyCodeObject3_8 v3_8;
} PyCodeObject;


// ---- frameobject.h ---------------------------------------------------------

typedef struct _frame2_3 {
    PyObject_VAR_HEAD
    struct _frame2_3 *f_back;   /* previous frame, or NULL */
    PyCodeObject *f_code;       /* code segment */
    PyObject *f_builtins;       /* builtin symbol table (PyDictObject) */
    PyObject *f_globals;        /* global symbol table (PyDictObject) */
    PyObject *f_locals;         /* local symbol table (any mapping) */
    PyObject **f_valuestack;    /* points after the last local */
    PyObject **f_stacktop;
    PyObject *f_trace;          /* Trace function */

    PyObject *f_exc_type, *f_exc_value, *f_exc_traceback;
    PyObject *f_gen;

    int f_lasti;                /* Last instruction if called */
    int f_lineno;               /* Current line number */
} PyFrameObject2;

typedef struct _frame3_7 {
    PyObject_VAR_HEAD
    struct _frame3_7 *f_back;   /* previous frame, or NULL */
    PyCodeObject *f_code;       /* code segment */
    PyObject *f_builtins;       /* builtin symbol table (PyDictObject) */
    PyObject *f_globals;        /* global symbol table (PyDictObject) */
    PyObject *f_locals;         /* local symbol table (any mapping) */
    PyObject **f_valuestack;    /* points after the last local */
    PyObject **f_stacktop;
    PyObject *f_trace;          /* Trace function */
    char f_trace_lines;         /* Emit per-line trace events? */
    char f_trace_opcodes;       /* Emit per-opcode trace events? */
    PyObject *f_gen;

    int f_lasti;                /* Last instruction if called */
    int f_lineno;               /* Current line number */
} PyFrameObject3_7;

typedef struct _frame3_10 {
    PyObject_VAR_HEAD
    struct _frame3_10 *f_back;  /* previous frame, or NULL */
    PyCodeObject *f_code;       /* code segment */
    PyObject *f_builtins;       /* builtin symbol table (PyDictObject) */
    PyObject *f_globals;        /* global symbol table (PyDictObject) */
    PyObject *f_locals;         /* local symbol table (any mapping) */
    PyObject **f_valuestack;    /* points after the last local */
    PyObject *f_trace;          /* Trace function */
    int f_stackdepth;           /* Depth of value stack */
    char f_trace_lines;         /* Emit per-line trace events? */
    char f_trace_opcodes;       /* Emit per-opcode trace events? */

    /* Borrowed reference to a generator, or NULL */
    PyObject *f_gen;

    int f_lasti;                /* Last instruction if called */
    int f_lineno;               /* Current line number. Only valid if non-zero */
} PyFrameObject3_10;

typedef union {
  PyFrameObject2    v2;
  PyFrameObject3_7  v3_7;
  PyFrameObject3_10 v3_10;
} PyFrameObject;

// ---- include/objimpl.h -----------------------------------------------------

typedef union _gc_head3_7 {
    struct {
        union _gc_head3_7 *gc_next;
        union _gc_head3_7 *gc_prev;
        Py_ssize_t gc_refs;
    } gc;
    long double dummy;  /* force worst-case alignment */
} PyGC_Head3_7;

typedef struct {
    uintptr_t _gc_next;
    uintptr_t _gc_prev;
} PyGC_Head3_8;

// ---- internal/mem.h --------------------------------------------------------

#define NUM_GENERATIONS 3

struct gc_generation3_7 {
    PyGC_Head3_7 head;
    int threshold; /* collection threshold */
    int count; /* count of allocations or collections of younger
                  generations */
};


struct gc_generation3_8 {
    PyGC_Head3_8 head;
    int threshold; /* collection threshold */
    int count; /* count of allocations or collections of younger
                  generations */
};

/* Running stats per generation */
struct gc_generation_stats {
    Py_ssize_t collections;
    Py_ssize_t collected;
    Py_ssize_t uncollectable;
};

struct _gc_runtime_state3_7 {
    PyObject *trash_delete_later;
    int trash_delete_nesting;
    int enabled;
    int debug;
    struct gc_generation3_7 generations[NUM_GENERATIONS];
    PyGC_Head3_7 *generation0;
    struct gc_generation3_7 permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    int collecting;
};

struct _gc_runtime_state3_8 {
    PyObject *trash_delete_later;
    int trash_delete_nesting;
    int enabled;
    int debug;
    struct gc_generation3_8 generations[NUM_GENERATIONS];
    PyGC_Head3_8 *generation0;
    struct gc_generation3_8 permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    int collecting;
};

typedef union {
    struct _gc_runtime_state3_7 v3_7;
    struct _gc_runtime_state3_8 v3_8;
} GCRuntimeState;

// ---- pystate.h -------------------------------------------------------------

struct _ts; /* Forward */

typedef struct _is2 {
    struct _is2 *next;
    struct _ts *tstate_head;
    void* gc;  /* Dummy */
} PyInterpreterState2;

// ---- internal/pycore_interp.h ----------------------------------------------

typedef void *PyThread_type_lock;

typedef struct _Py_atomic_int {
    int _value;
} _Py_atomic_int;

struct _pending_calls {
    PyThread_type_lock lock;
    _Py_atomic_int calls_to_do;
    int async_exc;
#define NPENDINGCALLS 32
    struct {
        int (*func)(void *);
        void *arg;
    } calls[NPENDINGCALLS];
    int first;
    int last;
};

struct _ceval_state {
    int recursion_limit;
    int tracing_possible;
    _Py_atomic_int eval_breaker;
    _Py_atomic_int gil_drop_request;
    struct _pending_calls pending;
};

typedef struct _is3_9 {

    struct _is3_9 *next;
    struct _is3_9 *tstate_head;

    /* Reference to the _PyRuntime global variable. This field exists
       to not have to pass runtime in addition to tstate to a function.
       Get runtime from tstate: tstate->interp->runtime. */
    struct pyruntimestate *runtime;

    int64_t id;
    int64_t id_refcount;
    int requires_idref;
    PyThread_type_lock id_mutex;

    int finalizing;

    struct _ceval_state ceval;
    struct _gc_runtime_state3_8 gc;
} PyInterpreterState3_9;

typedef union {
  PyInterpreterState2   v2;
  PyInterpreterState3_9 v3_9;
} PyInterpreterState;


// Dummy struct _frame
struct _frame;

typedef int (*Py_tracefunc)(PyObject *, struct _frame *, int, PyObject *);


typedef struct _ts3_3 {
    struct _ts3_3 *next;
    PyInterpreterState *interp;

    struct _frame *frame;
    int recursion_depth;
    char overflowed;
    char recursion_critical;
    int tracing;
    int use_tracing;

    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;

    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;

    PyObject *exc_type;
    PyObject *exc_value;
    PyObject *exc_traceback;

    PyObject *dict;  /* Stores per-thread state */

    int tick_counter;

    int gilstate_counter;

    PyObject *async_exc; /* Asynchronous exception to raise */
    long thread_id; /* Thread id where this tstate was created */
} PyThreadState2;


typedef struct _ts3_4 {
    struct _ts3_4 *prev;
    struct _ts3_4 *next;
    PyInterpreterState *interp;

    struct _frame *frame;
    int recursion_depth;
    char overflowed;
    char recursion_critical;
    int tracing;
    int use_tracing;

    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;

    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;

    PyObject *exc_type;
    PyObject *exc_value;
    PyObject *exc_traceback;

    PyObject *dict;  /* Stores per-thread state */

    int gilstate_counter;

    PyObject *async_exc; /* Asynchronous exception to raise */
    long thread_id; /* Thread id where this tstate was created */
} PyThreadState3_4;



typedef struct _err_stackitem {
    PyObject *exc_type, *exc_value, *exc_traceback;
    struct _err_stackitem *previous_item;
} _PyErr_StackItem;

typedef struct _ts_3_7 {
    struct _ts *prev;
    struct _ts *next;
    PyInterpreterState *interp;
    struct _frame *frame;
    int recursion_depth;
    char overflowed;
    char recursion_critical;
    int stackcheck_counter;
    int tracing;
    int use_tracing;
    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;
    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;
    _PyErr_StackItem exc_state;
    _PyErr_StackItem *exc_info;
    PyObject *dict;  /* Stores per-thread state */
    int gilstate_counter;
    PyObject *async_exc; /* Asynchronous exception to raise */
    unsigned long thread_id; /* Thread id where this tstate was created */
} PyThreadState3_7;


typedef struct _ts3_8 {
    struct _ts *prev;
    struct _ts *next;
    PyInterpreterState *interp;
    PyFrameObject *frame;
    int recursion_depth;
    int recursion_headroom; /* Allow 50 more calls to handle any errors. */
    int stackcheck_counter;
    int tracing;
    int use_tracing;
    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;
    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;
    _PyErr_StackItem exc_state;
    _PyErr_StackItem *exc_info;
    PyObject *dict;  /* Stores per-thread state */
    int gilstate_counter;
    PyObject *async_exc; /* Asynchronous exception to raise */
    unsigned long thread_id; /* Thread id where this tstate was created */
    int trash_delete_nesting;
    PyObject *trash_delete_later;
    void (*on_delete)(void *);
    void *on_delete_data;
    int coroutine_origin_tracking_depth;
    PyObject *async_gen_firstiter;
    PyObject *async_gen_finalizer;
    PyObject *context;
    uint64_t context_ver;
    uint64_t id;
} PyThreadState3_8;


typedef union {
  PyThreadState2   v2;
  PyThreadState3_4 v3_4;
  PyThreadState3_8 v3_8;
} PyThreadState;

// ---- internal/pystate.h ----------------------------------------------------

typedef struct pyruntimestate3_7 {
    int initialized;
    int core_initialized;
    PyThreadState *finalizing;

    struct pyinterpreters3_7 {
        PyThread_type_lock mutex;
        PyInterpreterState *head;
        PyInterpreterState *main;
        int64_t next_id;
    } interpreters;
#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _gc_runtime_state3_7 gc;
    // struct _warnings_runtime_state warnings;
    // struct _ceval_runtime_state ceval;
    // struct _gilstate_runtime_state gilstate;
} _PyRuntimeState3_7;

// ---- internal/pycore_pystate.h ---------------------------------------------

typedef struct pyruntimestate3_8 {
    int preinitializing;
    int preinitialized;
    int core_initialized;
    int initialized;
    PyThreadState *finalizing;

    struct pyinterpreters3_8 {
        PyThread_type_lock mutex;
        PyInterpreterState *head;
        PyInterpreterState *main;
        int64_t next_id;
    } interpreters;
    // XXX Remove this field once we have a tp_* slot.
    struct _xidregistry {
        PyThread_type_lock mutex;
        struct _xidregitem *head;
    } xidregistry;

    unsigned long main_thread;

#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _gc_runtime_state3_8 gc;
    // struct _ceval_runtime_state ceval;
    // struct _gilstate_runtime_state gilstate;
} _PyRuntimeState3_8;


typedef union {
  _PyRuntimeState3_7 v3_7;
  _PyRuntimeState3_8 v3_8;
} _PyRuntimeState;

// ---- unicodeobject.h -------------------------------------------------------

typedef uint32_t Py_UCS4;
typedef uint16_t Py_UCS2;
typedef uint8_t Py_UCS1;

#define PY_UNICODE_TYPE Py_UCS4

typedef PY_UNICODE_TYPE Py_UNICODE;


typedef struct {
    PyObject_HEAD
    Py_ssize_t length;          /* Length of raw Unicode data in buffer */
    Py_UNICODE *str;            /* Raw Unicode buffer */
    long hash;                  /* Hash value; -1 if not set */
    PyObject *defenc;           /* (Default) Encoded version as Python string */
} PyUnicodeObject2;


typedef Py_ssize_t Py_hash_t;

typedef struct {
    PyObject_HEAD
    Py_ssize_t length;          /* Number of code points in the string */
    Py_hash_t hash;             /* Hash value; -1 if not set */
    struct {
        unsigned int interned:2;
        unsigned int kind:3;
        unsigned int compact:1;
        unsigned int ascii:1;
        unsigned int ready:1;
        unsigned int :24;
    } state;
    wchar_t *wstr;              /* wchar_t representation (null-terminated) */
} PyASCIIObject;

typedef struct {
    PyASCIIObject _base;
    Py_ssize_t utf8_length;     /* Number of bytes in utf8, excluding the
                                 * terminating \0. */
    char *utf8;                 /* UTF-8 representation (null-terminated) */
    Py_ssize_t wstr_length;     /* Number of code points in wstr, possible
                                 * surrogates count as two code points. */
} PyCompactUnicodeObject;


typedef struct {
    PyCompactUnicodeObject _base;
    union {
        void *any;
        Py_UCS1 *latin1;
        Py_UCS2 *ucs2;
        Py_UCS4 *ucs4;
    } data;                     /* Canonical, smallest-form Unicode buffer */
} PyUnicodeObject3;


typedef union {
  PyUnicodeObject2 v2;
  PyUnicodeObject3 v3;
} PyUnicodeObject;

// ---- bytesobject.h ---------------------------------------------------------

typedef struct {
    PyObject_VAR_HEAD
    Py_hash_t ob_shash;
    char ob_sval[1];
} PyBytesObject;


// ---- stringobject.h --------------------------------------------------------

typedef struct {
    PyObject_VAR_HEAD
    long ob_shash;
    int ob_sstate;
    char ob_sval[1];
} PyStringObject; /* From Python 2.7 */


// ----------------------------------------------------------------------------

#endif // PYTHON36_H
