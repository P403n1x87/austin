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

#if defined PL_WIN
#define fprintfp _fprintf_p
#else
#define fprintfp fprintf
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
    if (fail(copy_memory(self->raddr.pid, self->proc->frames.lo, newsize, _frames.content))) {
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
    if (fail(copy_memory(self->raddr.pid, self->proc->frames_heap.lo, newsize, _frames_heap.content))) {
      log_d("Failed to read remote frame area near heap; will reset");
      sfree(_frames_heap.content);
      _frames_heap = NULL_HEAP;
      self->proc->frames_heap = NULL_MEM_BLOCK;

    }
  }
}


// ----------------------------------------------------------------------------
#ifdef DEBUG
static unsigned int _frame_cache_miss = 0;
static unsigned int _frame_cache_total = 0;
#endif

static inline int
_py_thread__resolve_py_stack(py_thread_t * self) {
  lru_cache_t * cache = self->proc->frame_cache;

  for (int i = 0; i < stack_pointer(); i++) {
    py_frame_t py_frame = stack_py_get(i);

    int       lasti     = py_frame.lasti;
    key_dt    frame_key = ((key_dt) py_frame.code << 16) | lasti;
    frame_t * frame     = lru_cache__maybe_hit(cache, frame_key);
    
    #ifdef DEBUG
    _frame_cache_total++;
    #endif

    if (!isvalid(frame)) {
      #ifdef DEBUG
      _frame_cache_miss++;
      #endif
      frame = _frame_from_code_raddr(
        &(raddr_t) {self->raddr.pid, py_frame.code}, lasti, self->proc->py_v
      );
      if (!isvalid(frame)) {
        log_ie("Failed to get frame from code object");
        // Truncate the stack to the point where we have successfully resolved.
        _stack->pointer = i;
        FAIL;
      }
      lru_cache__store(cache, frame_key, frame);
    }

    stack_set(i, frame);
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
static inline int
_py_thread__push_frame_from_addr(py_thread_t * self, PyFrameObject * frame_obj, void ** prev) {
  if (!isvalid(self))
    FAIL;
  
  V_DESC(self->proc->py_v);
  
  void * origin = *prev;
  
  *prev = V_FIELD_PTR(void *, frame_obj, py_frame, o_back);
  if (unlikely(origin == *prev)) {
    log_d("Frame points to itself!");
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

  raddr_t raddr = {self->raddr.pid, *prev};
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
}


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
  
  invalid = fail(_py_thread__resolve_py_stack(self)) || invalid;

  return invalid;
}


#ifdef NATIVE
// ----------------------------------------------------------------------------
int
py_thread__set_idle(py_thread_t * self) {
  unsigned char bit   = 1 << (self->tid & 7);
  size_t        index = self->tid >> 3;

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
int
py_thread__save_kernel_stack(py_thread_t * self) {
  char stack_path[48];
  int  fd;

  if (!isvalid(_kstacks))
    FAIL;

  sfree(_kstacks[self->tid]);

  sprintf(stack_path, "/proc/%d/task/%ld/stack", self->proc->pid, self->tid);
  fd = open(stack_path, O_RDONLY);
  if (fd == -1)
    FAIL;

  _kstacks[self->tid] = (char *) calloc(1, MAX_STACK_FILE_SIZE);
  if (read(fd, _kstacks[self->tid], MAX_STACK_FILE_SIZE) == -1) {
    log_e("stack: failed to read %s", stack_path);
    close(fd);
    FAIL;
  };
  close(fd);

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

  lru_cache_t * cache   = self->proc->frame_cache;
  void        * context = _tids[self->tid];

  if (!isvalid(context)) {
    log_e("libunwind: unexpected invalid context");
    FAIL;
  }

  if (fail(wait_unw_init_remote(&cursor, self->proc->unwind.as, context)))
    FAIL;

  stack_native_reset();

  do {
    if (unw_get_reg(&cursor, UNW_REG_IP, &pc)) {
      log_e("libunwind: cannot read program counter\n");
      FAIL;
    }

    #ifdef DEBUG
    _frame_cache_total++;
    #endif

    frame_t * frame = lru_cache__maybe_hit(cache, pc);
    if (!isvalid(frame)) {
      #ifdef DEBUG
      _frame_cache_miss++;
      #endif
      char * scope, * filename;
      vm_range_t * range = NULL;
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
            frame = get_native_frame(range->name, pc - base);
        }
        #endif
      }
      if (!isvalid(frame)) {
        if (unw_get_proc_name(&cursor, _native_buf, MAXLEN, &offset) == 0) {
          scope = strdup(_native_buf);
        }
        else {
          scope = strdup("<unnamed>");
          offset = 0;
        }
        if (isvalid(range))
          filename = strdup(range->name);
        else {
          sprintf(_native_buf, "native@%lx", pc);
          filename = strdup(_native_buf);
        }

        frame = frame_new(filename, scope, offset);
        if (!isvalid(frame)) {
          log_ie("Failed to make native frame");
          sfree(filename);
          sfree(scope);
          FAIL;
        }
      }

      lru_cache__store(cache, (key_dt) pc, (value_t) frame);
    }

    stack_native_push(frame);
  } while (!stack_native_full() && unw_step(&cursor) > 0);
  
  SUCCESS;
}
#endif

// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
int
py_thread__fill_from_raddr(py_thread_t * self, raddr_t * raddr, py_proc_t * proc) {
  if (!isvalid(self))
    FAIL;

  V_DESC(proc->py_v);

  PyThreadState ts;

  self->invalid = TRUE;

  if (fail(copy_from_raddr(raddr, ts))) {
    log_ie("Cannot read remote PyThreadState");
    FAIL;
  }

  self->proc = proc;

  self->raddr = *raddr;
  
  self->top_frame = V_FIELD(void*, ts, py_thread, o_frame);
   
  self->next_raddr = (raddr_t) {
    raddr->pid,
    V_FIELD(void*, ts, py_thread, o_next) == raddr->addr \
      ? NULL \
      : V_FIELD(void*, ts, py_thread, o_next)
  };

  self->tid = V_FIELD(long, ts, py_thread, o_thread_id);
  if (self->tid == 0) {
    // If we fail to get a valid Thread ID, we resort to the PyThreadState
    // remote address
    log_t("Thread ID fallback to remote address");
    self->tid = (uintptr_t) raddr->addr;
  }
  #if defined PL_LINUX
  else {
    if (
      likely(proc->extra->pthread_tid_offset)
      && success(read_pthread_t(self->raddr.pid, (void *) self->tid
    ))) {
      int o = proc->extra->pthread_tid_offset;
      self->tid = o > 0 ? _pthread_buffer[o] : (pid_t) ((pid_t *) _pthread_buffer)[-o];
      if (self->tid >= max_pid || self->tid == 0) {
        log_e("Invalid TID detected");
        FAIL;
      }
      #ifdef NATIVE
      // TODO: If a TID is reused we will never seize it!
      if (!isvalid(_tids[self->tid])) {
        if (fail(wait_ptrace(PTRACE_SEIZE, self->tid, 0, 0))) {
          log_e("ptrace: cannot seize thread %d: %d\n", self->tid, errno);
          FAIL;
        }
        else {
          log_d("ptrace: thread %d seized", self->tid);
        }
        _tids[self->tid] = _UPT_create(self->tid);
        if (!isvalid(_tids[self->tid])) {
          log_e("libunwind: failed to create context for thread %d", self->tid);
          FAIL;
        }
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
  if (self->invalid || !isvalid(self->next_raddr.addr))
    FAIL;

  return py_thread__fill_from_raddr(self, &(self->next_raddr), self->proc);
}


// ----------------------------------------------------------------------------
#if defined PL_WIN
  #define MEM_METRIC "%I64d"
#else
  #define MEM_METRIC "%ld"
#endif
#define TIME_METRIC "%lu"
#define IDLE_METRIC "%d"
#define METRIC_SEP  ","

void
py_thread__print_collapsed_stack(py_thread_t * self, ctime_t time_delta, ssize_t mem_delta) {
  if (!pargs.full && pargs.memory && mem_delta == 0)
    return;

  if (self->invalid)
    return;

  if (pargs.exclude_empty && stack_is_empty())
    // Skip if thread has no frames and we want to exclude empty threads
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
      #ifdef NATIVE
      // If we don't sample the threads stall :(
      _py_thread__unwind_native_frame_stack(self);
      #endif
      return;
    }
  }

  #ifdef NATIVE

  // We sample the kernel frame stack BEFORE interrupting because otherwise
  // we would see the ptrace syscall call stack, which is not very interesting.
  // The downside is that the kernel stack might not be in sync with the other
  // ones.
  if (pargs.kernel) {
    _py_thread__unwind_kernel_frame_stack(self);
  }
  if (fail(_py_thread__unwind_native_frame_stack(self))) {
    log_ie("Failed to unwind native stack");
    return;
  }

  // Update the thread state to improve guarantees that it will be in sync with
  // the native stack just collected
  py_thread__fill_from_raddr(self, &self->raddr, self->proc);
  #endif

  // Group entries by thread.
  fprintfp(
    pargs.output_file, pargs.head_format, self->proc->pid, self->tid,
    // These are relevant only in `where` mode
    is_idle           ? "💤" : "🚀",
    self->proc->child ? "🧒" : ""
  );

  if (isvalid(self->top_frame)) {
    if (fail(_py_thread__unwind_frame_stack(self))) {
      fprintf(pargs.output_file, ";:INVALID:");
      stats_count_error();
    }
  }

  #ifdef NATIVE

  while (!stack_native_is_empty()) {
    frame_t * native_frame = stack_native_pop();
    if (!isvalid(native_frame)) {
      log_e("Invalid native frame");
      break;
    }
    char * eval_frame_fn = strstr(native_frame->scope, "PyEval_EvalFrame");
    int is_frame_eval = FALSE;
    if (isvalid(eval_frame_fn)) {
      char c = *(eval_frame_fn+16);
      V_DESC(self->proc->py_v);
      is_frame_eval = (c == 'D') || (PYVER_ATMOST(3, 5) && c == 'E');
    }
    if (!stack_is_empty() && is_frame_eval) {
      // TODO: if the py stack is empty we have a mismatch.
      frame_t * frame = stack_pop();
      fprintf(pargs.output_file, pargs.format, frame->filename, frame->scope, frame->line);
    }
    else {
      fprintf(pargs.output_file, pargs.native_format, native_frame->filename, native_frame->scope, native_frame->line);
    }
  }
  if (!stack_is_empty()) {
    log_d("Stack mismatch: left with %d Python frames after interleaving", stack_pointer());
    austin_errno = ETHREADINV;
    #ifdef DEBUG
    fprintf(pargs.output_file, ";:%ld FRAMES LEFT:", stack_pointer());
    #endif
  }
  while (!stack_kernel_is_empty()) {
    char * scope = stack_kernel_pop();
    fprintf(pargs.output_file, pargs.kernel_format, scope);
    free(scope);
  }

  #else
  while (!stack_is_empty()) {
    frame_t * frame = stack_pop();
    fprintfp(pargs.output_file, pargs.format, frame->filename, frame->scope, frame->line);
  }
  #endif


  if (pargs.gc && py_proc__is_gc_collecting(self->proc) == TRUE) {
    fprintf(pargs.output_file, ";:GC:");
    stats_gc_time(time_delta);
  }

  if (unlikely(pargs.where))
    return;

  // Finish off sample with the metric(s)
  if (pargs.full) {
    fprintf(pargs.output_file, " " TIME_METRIC METRIC_SEP IDLE_METRIC METRIC_SEP MEM_METRIC "\n",
      time_delta, !!is_idle, mem_delta
    );
  }
  else {
    if (pargs.memory)
      fprintf(pargs.output_file, " " MEM_METRIC "\n", mem_delta);
    else
      fprintf(pargs.output_file, " " TIME_METRIC "\n", time_delta);
  }

  // Update sampling stats
  stats_count_sample();
  stats_check_duration(stopwatch_duration());
} /* py_thread__print_collapsed_stack */


// ----------------------------------------------------------------------------
int
py_thread_allocate(void) {
  if (isvalid(_stack))
    SUCCESS;

  if (fail(stack_allocate(MAX_STACK_SIZE)))
    FAIL;

  #if defined PL_WIN
  // On Windows we need to fetch process and thread information to detect idle
  // threads. We allocate a buffer for periodically fetching that data and, if
  // needed we grow it at runtime.
  _pi_buffer_size = (1 << 16) * sizeof(void *);
  _pi_buffer      = calloc(1, _pi_buffer_size);
  if (!isvalid(_pi_buffer))
    FAIL;
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
  FAIL;

ok:
  #endif

  SUCCESS;
}


// ----------------------------------------------------------------------------
void
py_thread_free(void) {
  #if defined PL_WIN
  sfree(_pi_buffer);
  #endif

  log_d(
    "Frame cache hit ratio: %d/%d (%0.2f%%)\n",
    _frame_cache_total - _frame_cache_miss,
    _frame_cache_total,
    (_frame_cache_total - _frame_cache_miss) * 100.0 / _frame_cache_total
  );

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
