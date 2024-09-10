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

#define PY_THREAD_C

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>

#include "argparse.h"
#include "cache.h"
#include "error.h"
#include "events.h"
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "platform.h"
#include "stack.h"
#include "timing.h"
#include "version.h"

#include "heap.h"
#include "py_thread.h"

// ----------------------------------------------------------------------------
// -- Platform-dependent implementations of _py_thread__is_idle
// ----------------------------------------------------------------------------

#if defined(PL_LINUX)

  #include "linux/py_thread.h"
  #if defined NATIVE && defined HAVE_BFD
  #include "linux/addr2line.h"
  #endif

#elif defined(PL_WIN)

  #include "win/py_thread.h"

#elif defined(PL_MACOS)

  #include "mac/py_thread.h"

#endif


// ---- PRIVATE ---------------------------------------------------------------

#define NULL_HEAP ((_heap_t) {NULL, 0})

static _heap_t      _frames      = NULL_HEAP;
static _heap_t      _frames_heap = NULL_HEAP;

static size_t           max_pid    = 0;
#ifdef NATIVE
static void          ** _tids      = NULL;
static unsigned char *  _tids_idle = NULL;
static unsigned char *  _tids_int  = NULL;
static char          ** _kstacks   = NULL;
#endif

// ----------------------------------------------------------------------------
#define _use_heaps (pargs.heap > 0)

static inline void
_py_thread__read_frames(py_thread_t * self) {
  if (!pargs.heap)
    return;

  size_t newsize;
  size_t maxsize = pargs.heap >> 1;

  if (isvalid(self->proc->frames.newhi)) {
    newsize = self->proc->frames.newhi - self->proc->frames.newlo;
    if (newsize > maxsize) {
      newsize = maxsize + sizeof(PyFrameObject);
    }
    if (newsize > _frames.size) {
      _frames.content = realloc(_frames.content, newsize);
      _frames.size = newsize;
      self->proc->frames.hi = self->proc->frames.newhi;
      self->proc->frames.lo = self->proc->frames.newlo;
    }
    if (fail(copy_memory(self->raddr.pref, self->proc->frames.lo, newsize, _frames.content))) {
      log_d("Failed to read remote frame area; will reset");
      sfree(_frames.content);
      _frames = NULL_HEAP;
      self->proc->frames = NULL_MEM_BLOCK;
    }
  }

  if (isvalid(self->proc->frames_heap.newhi)) {
    newsize = self->proc->frames_heap.newhi - self->proc->frames_heap.newlo;
    if (newsize > maxsize) {
      newsize = maxsize + sizeof(PyFrameObject);
    }
    if (newsize > _frames_heap.size) {
      _frames_heap.content = realloc(_frames_heap.content, newsize);
      _frames_heap.size = newsize;
      self->proc->frames_heap.hi = self->proc->frames_heap.newhi;
      self->proc->frames_heap.lo = self->proc->frames_heap.newlo;
    }
    if (fail(copy_memory(self->raddr.pref, self->proc->frames_heap.lo, newsize, _frames_heap.content))) {
      log_d("Failed to read remote frame area near heap; will reset");
      sfree(_frames_heap.content);
      _frames_heap = NULL_HEAP;
      self->proc->frames_heap = NULL_MEM_BLOCK;

    }
  }
} /* _py_thread__read_frames */


// ----------------------------------------------------------------------------
static inline void
_py_thread__read_stack(py_thread_t * self) {
  if (!pargs.heap || !isvalid(self->stack))
    return;

  // For now we read a single datastack chunk up to the requested heap size.

  size_t maxsize = pargs.heap < self->stack_size ? pargs.heap : self->stack_size;

  if (maxsize > _frames.size) {
    _frames.content = realloc(_frames.content, maxsize);
  }

  if (fail(copy_memory(self->raddr.pref, self->stack, maxsize, _frames.content))) {
    log_d("Failed to read remote thread stack data");
    sfree(_frames.content);
    _frames = NULL_HEAP;
  }
}


// ----------------------------------------------------------------------------
static inline int
_py_thread__resolve_py_stack(py_thread_t * self) {
  lru_cache_t * cache = self->proc->frame_cache;

  for (int i = 0; i < stack_pointer(); i++) {
    py_frame_t py_frame = stack_py_get(i);

    #ifdef NATIVE
    if (py_frame.origin == CFRAME_MAGIC) {
      stack_set(i, CFRAME_MAGIC);
      continue;
    }
    #endif
    int       lasti     = py_frame.lasti;
    key_dt    frame_key = py_frame_key(py_frame.code, lasti);
    frame_t * frame     = lru_cache__maybe_hit(cache, frame_key);

    if (!isvalid(frame)) {
      frame = _frame_from_code_raddr(self->proc, py_frame.code, lasti, self->proc->py_v);
      if (!isvalid(frame)) {
        log_ie("Failed to get frame from code object");
        // Truncate the stack to the point where we have successfully resolved.
        _stack->pointer = i;
        set_error(ETHREAD);
        FAIL;
      }
      lru_cache__store(cache, frame_key, frame);
      if (pargs.binary) {
        mojo_frame(frame);
      }
    }

    stack_set(i, frame);
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
static inline int
_py_thread__push_frame_from_addr(py_thread_t * self, PyFrameObject * frame_obj, void ** prev) {
  if (!isvalid(self)) {
    log_e("Not pushing frame from invalid thread");
    set_error(ETHREAD);
    FAIL;
  }
  
  V_DESC(self->proc->py_v);
  
  void * origin = *prev;
  
  *prev = V_FIELD_PTR(void *, frame_obj, py_frame, o_back);
  if (unlikely(origin == *prev)) {
    log_d("Frame points to itself!");
    set_error(ETHREAD);
    FAIL;
  }

  stack_py_push(
    origin,
    V_FIELD_PTR(void *, frame_obj, py_frame, o_code),
    V_FIELD_PTR(int, frame_obj, py_frame, o_lasti)
  );

  SUCCESS;
}


// ----------------------------------------------------------------------------
static inline int
_py_thread__push_frame_from_raddr(py_thread_t * self, void ** prev) {
  PyFrameObject frame;

  raddr_t raddr = {self->raddr.pref, *prev};
  if (fail(copy_from_raddr_v((&raddr), frame, self->proc->py_v->py_frame.size))) {
    log_ie("Cannot read remote PyFrameObject");
    FAIL;
  }

  return _py_thread__push_frame_from_addr(self, &frame, prev);
}


// ----------------------------------------------------------------------------
#define REL(raddr, block, base) (raddr - block.lo + base)

#ifdef DEBUG
static unsigned int _frames_total = 0;
static unsigned int _frames_miss  = 0;
#endif

static inline int
_py_thread__push_frame(py_thread_t * self, void ** prev) {
  void * raddr = *prev;
  if (_use_heaps) {
    #ifdef DEBUG
    _frames_total++;
    #endif
    py_proc_t * proc = self->proc;

    if (isvalid(_frames.content)
      && raddr >= proc->frames.lo
      && raddr <  proc->frames.lo + _frames.size
    ) {
      return _py_thread__push_frame_from_addr(
        self, REL(raddr, proc->frames, _frames.content), prev
      );
    }
    else if (isvalid(_frames_heap.content)
      && raddr >= proc->frames_heap.lo
      && raddr <  proc->frames_heap.lo + _frames_heap.size
    ) {
      return _py_thread__push_frame_from_addr(
        self, REL(raddr, proc->frames_heap, _frames_heap.content), prev
      );
    }

    #ifdef DEBUG
    _frames_miss++;
    #endif

    // Miss: update ranges
    // We quite likely set the bss map data so this should be a pretty reliable
    // platform-independent way of dualising the frame heap.
    if (raddr >= proc->map.bss.base && raddr <= proc->map.bss.base + (1 << 27)) {
      if (raddr + sizeof(PyFrameObject) > proc->frames_heap.newhi) {
        proc->frames_heap.newhi = raddr + sizeof(PyFrameObject);
      }
      if (raddr < proc->frames_heap.newlo) {
        proc->frames_heap.newlo = raddr;
      }
    }
    else {    
      if (raddr + sizeof(PyFrameObject) > proc->frames.newhi) {
        proc->frames.newhi = raddr + sizeof(PyFrameObject);
      }
      if (raddr < proc->frames.newlo) {
        proc->frames.newlo = raddr;
      }
    }
  }

  return _py_thread__push_frame_from_raddr(self, prev);
} /* _py_thread__push_frame */


// ----------------------------------------------------------------------------
static inline int
_py_thread__push_iframe_from_addr(py_thread_t * self, PyInterpreterFrame * iframe, void ** prev) {
  V_DESC(self->proc->py_v);
  
  void * origin     = *prev;
  void * code_raddr = V_FIELD_PTR(void *, iframe, py_iframe, o_code);

  *prev = V_FIELD_PTR(void *, iframe, py_iframe, o_previous);
  if (unlikely(origin == *prev)) {
    log_d("Interpreter frame points to itself!");
    set_error(ETHREAD);
    FAIL;
  }

  if (V_MIN(3, 12) && V_FIELD_PTR(char, iframe, py_iframe, o_owner) == FRAME_OWNED_BY_CSTACK) {
    // This is a shim frame that we can ignore
    #ifdef NATIVE
    // In native mode we take this as the marker for the beginning of the stack
    // for a call to PyEval_EvalFrameDefault.
    stack_py_push_cframe();
    #endif
    SUCCESS;
  }

  stack_py_push(
    origin,
    code_raddr,
    (((int)(V_FIELD_PTR(void *, iframe, py_iframe, o_prev_instr) - code_raddr)) - py_v->py_code.o_code) / sizeof(_Py_CODEUNIT)
  );

  #ifdef NATIVE
  if (V_EQ(3, 11) && V_FIELD_PTR(int, iframe, py_iframe, o_is_entry)) {
    // This marks the end of a CFrame
    stack_py_push_cframe();
  }
  #endif

  SUCCESS;
}


// ----------------------------------------------------------------------------
static inline int
_py_thread__push_iframe_from_raddr(py_thread_t * self, void ** prev) {
  PyInterpreterFrame iframe;

  V_DESC(self->proc->py_v);

  if (fail(copy_py(self->raddr.pref, *prev, py_iframe, iframe))) {
    log_ie("Cannot read remote PyInterpreterFrame");
    FAIL;
  }

  return _py_thread__push_iframe_from_addr(self, &iframe, prev);
}


// ----------------------------------------------------------------------------
static inline int
_py_thread__push_iframe(py_thread_t * self, void ** prev) {
  void * raddr = *prev;
  if (_use_heaps) {
    #ifdef DEBUG
    _frames_total++;
    #endif

    if (isvalid(_frames.content) && (raddr >= self->stack && raddr < self->stack + self->stack_size)) {
      return _py_thread__push_iframe_from_addr(self, raddr - self->stack + _frames.content, prev);
    }

    #ifdef DEBUG
    _frames_miss++;
    #endif
  }

  return _py_thread__push_iframe_from_raddr(self, prev);
} /* _py_thread__push_iframe */


// ----------------------------------------------------------------------------
static inline int
_py_thread__unwind_frame_stack(py_thread_t * self) {
  int invalid = FALSE;

  _py_thread__read_frames(self);
  
  stack_reset();

  void * prev = self->top_frame;
  if (fail(_py_thread__push_frame(self, &prev))) {
    log_ie("Failed to fill top frame");
    FAIL;
  }

  while (isvalid(prev)) {
    if (fail(_py_thread__push_frame(self, &prev))) {
      log_d("Failed to retrieve frame #%d (from top).", stack_pointer());
      invalid = TRUE;
      break;
    }
    if (stack_full()) {
      log_w("Invalid frame stack: too tall");
      invalid = TRUE;
      break;
    }
    if (stack_has_cycle()) {
      log_d("Circular frame reference detected");
      invalid = TRUE;
      break;
    }
  }

  return invalid;
}


// ----------------------------------------------------------------------------
static inline int
_py_thread__unwind_iframe_stack(py_thread_t * self, void * iframe_raddr) {
  int invalid = FALSE; 
  void * curr = iframe_raddr;
  
  while (isvalid(curr)) {
    if (fail(_py_thread__push_iframe(self, &curr))) {
      log_d("Failed to retrieve iframe #%d", stack_pointer());
      invalid = TRUE;
      break;
    }

    if (stack_full()) {
      log_w("Invalid frame stack: too tall");
      invalid = TRUE;
      break;
    }

    if (stack_has_cycle()) {
      log_d("Circular frame reference detected");
      invalid = TRUE;
      break;
    }
  }
  
  invalid = fail(_py_thread__resolve_py_stack(self)) || invalid;

  return invalid;
}



// ----------------------------------------------------------------------------
static inline int
_py_thread__unwind_cframe_stack(py_thread_t * self) {
  PyCFrame cframe;

  int invalid = FALSE;

  _py_thread__read_stack(self);

  stack_reset();

  V_DESC(self->proc->py_v);

  if (fail(copy_py(self->raddr.pref, self->top_frame, py_cframe, cframe))) {
    log_ie("Cannot read remote PyCFrame");
    FAIL;
  }

  invalid = fail(_py_thread__unwind_iframe_stack(self, V_FIELD(void *, cframe, py_cframe, o_current_frame)));
  if (invalid)
    return invalid;

  return invalid;
}


#ifdef NATIVE
// ----------------------------------------------------------------------------
int
py_thread__set_idle(py_thread_t * self) {
  unsigned char bit   = 1 << (self->tid & 7);
  size_t        index = self->tid >> 3;

  if (index > (max_pid >> 3)) {
    log_e("Invalid TID");
    set_error(ETHREAD);
    FAIL;
  }

  if (_py_thread__is_idle(self)) {
    _tids_idle[index] |= bit;
  } else {
    _tids_idle[index] &= ~bit;
  }

  SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_thread__set_interrupted(py_thread_t * self, int state) {
  unsigned char bit   = 1 << (self->tid & 7);
  size_t        index = self->tid >> 3;

  if (state) {
    _tids_int[index] |= bit;
  } else {
    _tids_int[index] &= ~bit;
  }

  SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_thread__is_interrupted(py_thread_t * self) {
  return _tids_int[self->tid >> 3] & (1 << (self->tid & 7));
}

// ----------------------------------------------------------------------------
#define MAX_STACK_FILE_SIZE 2048

#if defined __arm__
#define TID_FMT "%d"
#else
#define TID_FMT "%ld"
#endif

int
py_thread__save_kernel_stack(py_thread_t * self) {
  char  stack_path[48];

  if (!isvalid(_kstacks)) {
    log_e("Invalid kernel stack");
    set_error(ETHREAD);
    FAIL;
  }

  sfree(_kstacks[self->tid]);

  sprintf(stack_path, "/proc/%d/task/" TID_FMT "/stack", self->proc->pid, self->tid);
  cu_fd fd = open(stack_path, O_RDONLY);
  if (fd == -1) {
    log_e("Failed to open %s", stack_path);
    set_error(ETHREAD);
    FAIL;
  }

  _kstacks[self->tid] = (char *) calloc(1, MAX_STACK_FILE_SIZE);
  if (read(fd, _kstacks[self->tid], MAX_STACK_FILE_SIZE) == -1) {
    log_e("stack: failed to read %s", stack_path);
    set_error(ETHREAD);
    FAIL;
  };

  SUCCESS;
}

// ----------------------------------------------------------------------------
static inline int
_py_thread__unwind_kernel_frame_stack(py_thread_t * self) {
  char * line = _kstacks[self->tid];
  if (!isvalid(line))
    SUCCESS;

  log_t("linux: unwinding kernel stack");

  stack_kernel_reset();

  for (;;) {
    char * eol = strchr(line, '\n');
    if (!isvalid(eol))
      break;
    *eol = '\0';

    char * b = strchr(line, ']');
    if (isvalid(b)) {
      char * e = strchr(++b, '+');
      if (isvalid(e))
        *e = 0;

      stack_kernel_push(strdup(++b));
    }
    line = eol + 1;
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
static char _native_buf[MAXLEN];

static inline int
wait_unw_init_remote(unw_cursor_t * c, unw_addr_space_t as, void * arg) {
  int outcome = 0;
  ctime_t end = gettime() + 1000;
  while(gettime() <= end && (outcome = unw_init_remote(c, as, arg)) == -UNW_EBADREG)
    sched_yield();
  if (fail(outcome))
    log_e("unwind: failed to initialize cursor (%d)", outcome);
  return outcome;
}

static inline int
_py_thread__unwind_native_frame_stack(py_thread_t * self) {
  unw_cursor_t cursor;
  unw_word_t   offset, pc;

  lru_cache_t * cache        = self->proc->frame_cache;
  lru_cache_t * string_cache = self->proc->string_cache;
  void        * context      = _tids[self->tid];

  stack_native_reset();

  if (!isvalid(context)) {
    _tids[self->tid] = _UPT_create(self->tid);
    if (!isvalid(_tids[self->tid])) {
      log_e("libunwind: failed to re-create context for thread %d", self->tid);
      set_error(ETHREAD);
      FAIL;
    }
    if (!isvalid(context)) {
      log_e("libunwind: unexpected invalid context");
      set_error(ETHREAD);
      FAIL;
    }
  }

  if (fail(wait_unw_init_remote(&cursor, self->proc->unwind.as, context))) {
    log_e("libunwind: failed to initialize remote cursor");
    set_error(ETHREAD);
    FAIL;
  }

  do {
    if (unw_get_reg(&cursor, UNW_REG_IP, &pc)) {
      log_e("libunwind: cannot read program counter\n");
      set_error(ETHREAD);
      FAIL;
    }

    key_dt frame_key = (key_dt) pc;

    frame_t * frame = lru_cache__maybe_hit(cache, frame_key);
    if (!isvalid(frame)) {
      char       * scope    = NULL;
      char       * filename = NULL;
      vm_range_t * range    = NULL;
      if (pargs.where) {
        range = vm_range_tree__find(self->proc->maps_tree, pc);
        // TODO: A failed attempt to find a range is an indication that we need
        // to regenerate the VM maps. This would be of no use at the moment,
        // since we only use them in `where` mode where we sample just once. If
        // we resort to improving addr2line and use the VM range tree for
        // normal mode, then we should consider catching the case
        // !isvalid(range) and regenerate the VM range tree with fresh data.
        #ifdef HAVE_BFD
        if (isvalid(range)) {
          unw_word_t base = (unw_word_t) hash_table__get(
            self->proc->base_table, string__hash(range->name)
          );
          if (base > 0)
            frame = get_native_frame(range->name, pc - base, frame_key);
        }
        #endif
      }
      if (!isvalid(frame)) {
        unw_proc_info_t pi;
        if (success(unw_get_proc_info(&cursor, &pi))) {
          key_dt scope_key = (key_dt) pi.start_ip;
          scope = lru_cache__maybe_hit(string_cache, scope_key);
          if (!isvalid(scope)) {
            if (unw_get_proc_name(&cursor, _native_buf, MAXLEN, &offset) == 0) {
              scope = strdup(_native_buf);
              lru_cache__store(string_cache, scope_key, scope);
              if (pargs.binary) {
                mojo_string_event(scope_key, scope);
              }
            }
          }
          if (pargs.binary && isvalid(scope)) {
            scope = (char *) scope_key;
          }
        }
        if (!isvalid(scope)) {
          scope = UNKNOWN_SCOPE;
          offset = 0;
        }
        if (isvalid(range))  // For now this is only relevant in `where` mode
          filename = strdup(range->name);
        else {
          // The program counter carries information about the file name *and*
          // the line number. Given that we don't resolve the file name using
          // memory ranges at runtime for performance reasons, we need to store
          // the PC value so that we can later resolve it to a file name and
          // line number, instead of doing the more sensible thing of using
          // something like `scope_key+1`, or the resolved base address.
          key_dt filename_key = (key_dt) pc;
          filename = lru_cache__maybe_hit(string_cache, filename_key);
          if (!isvalid(filename)) {
            sprintf(_native_buf, "native@" ADDR_FMT, pc);
            filename = strdup(_native_buf);
            lru_cache__store(string_cache, (key_dt) pc, filename);
            if (pargs.binary) {
              mojo_string_event(filename_key, filename);
            }
          }
          if (pargs.binary) {
            filename = (char *) filename_key;
          }
        }

        frame = frame_new(frame_key, filename, scope, offset, 0, 0, 0);
        if (!isvalid(frame)) {
          log_ie("Failed to make native frame");
          set_error(ETHREAD);
          FAIL;
        }
      }

      lru_cache__store(cache, frame_key, (value_t) frame);
      if (pargs.binary) {
        mojo_frame(frame);
      }
    }

    stack_native_push(frame);
  } while (!stack_native_full() && unw_step(&cursor) > 0);
  
  SUCCESS;
} /* _py_thread__unwind_native_frame_stack */


// ----------------------------------------------------------------------------
static inline int
_py_thread__seize(py_thread_t * self) {
  // TODO: If a TID is reused we will never seize it!
  if (!isvalid(_tids[self->tid])) {
    if (fail(wait_ptrace(PTRACE_SEIZE, self->tid, 0, 0))) {
      log_e("ptrace: cannot seize thread %d: %d\n", self->tid, errno);
      set_error(ETHREAD);
      FAIL;
    }
    else {
      log_d("ptrace: thread %d seized", self->tid);
    }
    _tids[self->tid] = _UPT_create(self->tid);
    if (!isvalid(_tids[self->tid])) {
      log_e("libunwind: failed to create context for thread %d", self->tid);
      set_error(ETHREAD);
      FAIL;
    }
  }
  SUCCESS;
}

#endif /* NATIVE */



// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
int
py_thread__fill_from_raddr(py_thread_t * self, raddr_t * raddr, py_proc_t * proc) {
  if (!isvalid(self)) {
    log_e("Cannot fill invalid thread");
    set_error(ETHREAD);
    FAIL;
  }

  V_DESC(proc->py_v);

  PyThreadState ts;
  _PyStackChunk chunk;

  self->invalid = TRUE;

  if (fail(copy_from_raddr(raddr, ts))) {
    log_ie("Cannot read remote PyThreadState");
    FAIL;
  }
  
  self->stack = NULL;

  if (pargs.heap && V_MIN(3, 11)) {
    // Get the thread stack information.
    void * stack_raddr = V_FIELD(void *, ts, py_thread, o_stack);
    
    if (fail(copy_datatype(self->raddr.pref, stack_raddr, chunk))) {
      // Best effort
      log_d("Cannot read thread data stack");
    }
    else {
      self->stack = stack_raddr;
      self->stack_size = chunk.size;
    }
  }
  
  self->proc = proc;

  self->raddr = *raddr;
  
  self->top_frame = V_FIELD(void*, ts, py_thread, o_frame);

  self->status = V_FIELD(tstate_status_t, ts, py_thread, o_status);

  self->next_raddr = (raddr_t) {
    raddr->pref,
    V_FIELD(void*, ts, py_thread, o_next) == raddr->addr \
      ? NULL \
      : V_FIELD(void*, ts, py_thread, o_next)
  };

  #if defined PL_MACOS
  self->tid = V_FIELD(long, ts, py_thread, o_thread_id);
  #else
  if (V_MIN(3, 11)) {
    self->tid = V_FIELD(long, ts, py_thread, o_native_thread_id);
  }
  else {
    self->tid = V_FIELD(long, ts, py_thread, o_thread_id);
  }
  #endif
  if (self->tid == 0) {
    // If we fail to get a valid Thread ID, we resort to the PyThreadState
    // remote address
    log_e("Failed to retrieve OS thread information");
    set_error(ETHREAD);
    FAIL;
  }
  #if defined PL_LINUX
  else {
    if (V_MIN(3, 11)) {
      // We already have the native thread id
      #ifdef NATIVE
      if (fail(_py_thread__seize(self))) {
        FAIL;
      }
      #endif
    }
    else if (
      likely(proc->extra->pthread_tid_offset)
      && success(read_pthread_t(self->proc, (void *) self->tid
    ))) {
      int o = proc->extra->pthread_tid_offset;
      self->tid = o > 0
        ? proc->extra->_pthread_buffer[o]
        : (pid_t) ((pid_t *) proc->extra->_pthread_buffer)[-o];
      if (self->tid >= max_pid || self->tid == 0) {
        log_e("Invalid TID detected");
        self->tid = 0;
        FAIL;
      }
      #ifdef NATIVE
      if (fail(_py_thread__seize(self))) {
        FAIL;
      }
      #endif
    }
  }
  #endif

  self->invalid = FALSE;
  SUCCESS;
} /* py_thread__fill_from_raddr */


// ----------------------------------------------------------------------------
int
py_thread__next(py_thread_t * self) {
  if (self->invalid) {
    log_e("Invalid thread or no address for next thread: %p", self);
    set_error(ETHREADINV);
    FAIL;
  }

  if (!isvalid(self->next_raddr.addr)) {
    austin_errno = ETHREADNONEXT;
    FAIL;
  }

  log_t("Found next thread");

  return py_thread__fill_from_raddr(self, &(self->next_raddr), self->proc);
}


// ----------------------------------------------------------------------------
void
py_thread__emit_collapsed_stack(py_thread_t * self, int64_t interp_id, ctime_t time_delta, ssize_t mem_delta) {
  if (!pargs.full && pargs.memory && mem_delta == 0)
    return;

  if (self->invalid)
    return;

  if (mem_delta == 0 && time_delta == 0)
    return;

  int is_idle = FALSE;
  if (pargs.full || pargs.sleepless || unlikely(pargs.where)) {
    #ifdef NATIVE
    size_t index  = self->tid >> 3;
    int    offset = self->tid & 7;

    is_idle = _tids_idle[index] & (1 << offset);
    #else
    is_idle = _py_thread__is_idle(self);
    #endif
    if (!pargs.full && is_idle && pargs.sleepless) {
      return;
    }
  }

  // Group entries by thread.
  emit_stack(
    pargs.head_format, self->proc->pid, interp_id, self->tid,
    // These are relevant only in `where` mode
    is_idle           ? "💤" : "🚀",
    self->proc->child ? "🧒" : ""
  );

  int error = FALSE;

  #ifdef NATIVE

  // We sample the kernel frame stack BEFORE interrupting because otherwise
  // we would see the ptrace syscall call stack, which is not very interesting.
  // The downside is that the kernel stack might not be in sync with the other
  // ones.
  if (pargs.kernel) {
    _py_thread__unwind_kernel_frame_stack(self);
  }
  if (fail(_py_thread__unwind_native_frame_stack(self))) {
    emit_invalid_frame();
    error = TRUE;
  }

  // Update the thread state to improve guarantees that it will be in sync with
  // the native stack just collected
  py_thread__fill_from_raddr(self, &self->raddr, self->proc);
  #endif

  V_DESC(self->proc->py_v);

  if (isvalid(self->top_frame)) {
    if (V_MIN(3, 11)) {
      if (fail(_py_thread__unwind_cframe_stack(self))) {
        emit_invalid_frame();
        error = TRUE;
      }
    }
    else {
      if (fail(_py_thread__unwind_frame_stack(self))) {
        emit_invalid_frame();
        error = TRUE;
      }
    }
    
    if (fail(_py_thread__resolve_py_stack(self))) {
      emit_invalid_frame();
      error = TRUE;
    }
  }

  #ifdef NATIVE

  if (V_MIN(3, 11)) {
    // We expect a CFrame to sit at the top of the stack
    if (!stack_is_empty() && stack_pop() != CFRAME_MAGIC) {
      log_e("Invalid resolved Python stack");
    }
  }
  while (!stack_native_is_empty()) {
    frame_t * native_frame = stack_native_pop();
    if (!isvalid(native_frame)) {
      log_e("Invalid native frame");
      break;
    }
    char * scope = pargs.binary
      ? lru_cache__maybe_hit(self->proc->string_cache, (key_dt) native_frame->scope)
      : native_frame->scope;
    if (!isvalid(scope)) {
      scope = UNKNOWN_SCOPE;
    }

    int is_frame_eval = (scope == UNKNOWN_SCOPE) ? FALSE : isvalid(strstr(scope, "PyEval_EvalFrameDefault"));
    if (!stack_is_empty() && is_frame_eval) {
      // TODO: if the py stack is empty we have a mismatch.
      frame_t * frame = stack_pop();
      if (V_MIN(3, 11)) {
        while (frame != CFRAME_MAGIC) {
          emit_frame_ref(pargs.format, frame);

          if (stack_is_empty())
            break;

          frame = stack_pop();
        }
      }
      else {
        emit_frame_ref(pargs.format, frame);
      }
    }
    else {
      emit_frame_ref(pargs.native_format, native_frame);
    }
  }
  if (!stack_is_empty()) {
    log_d("Stack mismatch: left with %d Python frames after interleaving", stack_pointer());
    set_error(ETHREADINV);
    #ifdef DEBUG
    emit_frames_left(stack_pointer());
    #endif
  }
  while (!stack_kernel_is_empty()) {
    char * scope = stack_kernel_pop();
    emit_kernel_frame(pargs.kernel_format, scope);
    free(scope);
  }

  #else
  while (!stack_is_empty()) {
    frame_t * frame = stack_pop();
    emit_frame_ref(pargs.format, frame);
  }
  #endif

  if (pargs.gc && py_proc__is_gc_collecting(self->proc) == TRUE) {
    emit_gc();
    stats_gc_time(time_delta);
  }

  if (unlikely(pargs.where))
    return;

  // Finish off sample with the metric(s)
  if (pargs.full) {
    emit_full_metrics(time_delta, !!is_idle, mem_delta);
  }
  else {
    if (pargs.memory) {
      emit_memory_metric(mem_delta);
    } else {
      emit_time_metric(time_delta);
    }
  }

  // Update sampling stats
  stats_count_sample();
  if (error) stats_count_error();
  stats_check_duration(stopwatch_duration());
} /* py_thread__emit_collapsed_stack */


// ----------------------------------------------------------------------------
int
py_thread_allocate(void) {
  if (isvalid(_stack))
    SUCCESS;

  if (fail(stack_allocate(MAX_STACK_SIZE))) {
    log_e("Failed to allocate stack");
    set_error(ETHREAD);
    FAIL;
  }

  #if defined PL_WIN
  // On Windows we need to fetch process and thread information to detect idle
  // threads. We allocate a buffer for periodically fetching that data and, if
  // needed we grow it at runtime.
  _pi_buffer_size = (1 << 16) * sizeof(void *);
  _pi_buffer      = calloc(1, _pi_buffer_size);
  if (!isvalid(_pi_buffer)) {
    set_error(ETHREAD);
    FAIL;
  }
  #endif

  max_pid = pid_max() + 1;

  #ifdef NATIVE
  _tids = (void **) calloc(max_pid, sizeof(void *));
  if (!isvalid(_tids))
    goto failed;

  size_t bmsize = (max_pid >> 3) + 1;

  _tids_idle = (unsigned char *) calloc(bmsize, sizeof(unsigned char));
  if (!isvalid(_tids_idle))
    goto failed;

  _tids_int = (unsigned char *) calloc(bmsize, sizeof(unsigned char));
  if (!isvalid(_tids_int))
    goto failed;

  if (pargs.kernel) {
    _kstacks = (char **) calloc(max_pid, sizeof(char *));
    if (!isvalid(_kstacks))
      goto failed;
  }
  goto ok;

failed:
  sfree(_tids);
  sfree(_tids_idle);
  sfree(_tids_int);
  sfree(_kstacks);
  
  set_error(ETHREAD);
  FAIL;

ok:
  #endif /* NATIVE */

  SUCCESS;
}


// ----------------------------------------------------------------------------
void
py_thread_free(void) {
  #if defined PL_WIN
  sfree(_pi_buffer);
  #endif

  #ifdef DEBUG
  if (_frames_total) {
    log_d(
      "Frame heaps hit ratio: %d/%d (%0.2f%%)\n",
      _frames_total - _frames_miss,
      _frames_total,
      (_frames_total - _frames_miss) * 100.0 / _frames_total
    );
  }
  #endif

  stack_deallocate();
  sfree(_frames.content);
  sfree(_frames_heap.content);

  #ifdef NATIVE
  for (pid_t tid = 0; tid < max_pid; tid++) {
    if (isvalid(_tids[tid])) {
      _UPT_destroy(_tids[tid]);
      if (fail(wait_ptrace(PTRACE_DETACH, tid, 0, 0))) {
        log_d("ptrace: failed to detach thread %ld", tid);
      } else {
        log_d("ptrace: thread %ld detached", tid);
      }
    }
    if (isvalid(_kstacks) && isvalid(_kstacks[tid])) {
      sfree(_kstacks[tid]);
    }
  }
  sfree(_tids);
  sfree(_tids_idle);
  sfree(_tids_int);
  sfree(_kstacks);
  #endif
}
