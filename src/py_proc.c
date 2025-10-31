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

#define PY_PROC_C

#include "platform.h"

#ifdef PL_WIN
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "argparse.h"
#include "bin.h"
#include "env.h"
#include "error.h"
#include "events.h"
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "py_interp.h"
#include "py_string.h"
#include "stack.h"
#include "stats.h"
#include "timer.h"

#include "py_proc.h"
#include "py_thread.h"

// ---- PRIVATE ---------------------------------------------------------------

// In native mode we have both the Python and native stacks (the kernel stack
// is negligible). We make sure we have a cache large enough to hold the full.
// stack.
#ifdef NATIVE
#define MAX_FRAME_CACHE_SIZE (MAX_STACK_SIZE << 1)
#else
#define MAX_FRAME_CACHE_SIZE MAX_STACK_SIZE
#endif
#define MAX_STRING_CACHE_SIZE LRU_CACHE_EXPAND
#define MAX_CODE_CACHE_SIZE   LRU_CACHE_EXPAND

#define py_proc__memcpy(self, raddr, size, dest) copy_memory(self->ref, raddr, size, dest)

// ----------------------------------------------------------------------------
// -- Platform-dependent implementations of _py_proc__init
// ----------------------------------------------------------------------------

// Forward declaration
static int
_py_proc__check_sym(py_proc_t*, char*, void*);

#if defined(PL_LINUX)

#include "linux/py_proc.h"

#elif defined(PL_WIN)

#include "win/py_proc.h"

#elif defined(PL_MACOS)

#include "mac/py_proc.h"

#endif

// ----------------------------------------------------------------------------
static int
_py_proc__check_sym(py_proc_t* self, char* name, void* value) {
    if (!(isvalid(self) && isvalid(name) && isvalid(value)))
        return 0;

    for (register int i = 0; i < DYNSYM_COUNT; i++) {
        if (success(symcmp(name, i))) {
            self->symbols[i] = value;
            log_d("Symbol %s found @ %p", name, value);
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
static int
_get_version_from_executable(char* binary, int* major, int* minor, int* patch) {
    cu_pipe* fp;
    char     version[64];
    char     cmd[256];

#if defined PL_WIN
    sprintf(cmd, "\"\"%s\"\" -V 2>&1", binary);
#else
    sprintf(cmd, "%s -V 2>&1", binary);
#endif

    fp = _popen(cmd, "r");
    if (!isvalid(fp)) { // GCOV_EXCL_START
        set_error(OS, "Cannot open pipe");
        FAIL;
    } // GCOV_EXCL_STOP

    log_d("Getting Python version from executable %s", binary);

    while (fgets(version, sizeof(version) - 1, fp) != NULL) {
        if (sscanf(version, "Python %d.%d.%d", major, minor, patch) == 3)
            SUCCESS;
    }

    set_error(BINARY, "Cannot determine Python version from executable");
    FAIL;
} /* _get_version_from_executable */

static int
_get_version_from_filename(char* filename, const char* needle, int* major, int* minor, int* patch) {
#if defined PL_LINUX /* LINUX */
    char*  base       = filename;
    char*  end        = base + strlen(base);
    size_t needle_len = strlen(needle);

    while (base < end) {
        base = strstr(base, needle);
        if (!isvalid(base)) { // GCOV_EXCL_LINE
            break;            // GCOV_EXCL_LINE
        }
        base += needle_len;
        if (sscanf(base, "%u.%u", major, minor) == 2) {
            SUCCESS;
        }
    }

#elif defined PL_WIN /* WIN */
    // Assume the library path is of the form *.python3[0-9]+[.]dll
    int n = strlen(filename);
    if (n < 10)
        FAIL;

    char* p = filename + n - 1;
    while (*(p--) != 'n' && p > filename)
        ;
    p++;
    *major = *(p++) - '0';
    if (*major != 3)
        FAIL;

    if (sscanf(p, "%d.dll", minor) == 1)
        SUCCESS;

#elif defined PL_MACOS /* MAC */
    char* ver_needle = strstr(filename, "3.");
    if (ver_needle != NULL && sscanf(ver_needle, "%d.%d", major, minor) == 2) {
        SUCCESS;
    }

#endif

    FAIL;
} /* _get_version_from_filename */

#if defined PL_MACOS
static int
_find_version_in_binary(char* path, int* version) {
    size_t      binary_size = 0;
    struct stat s;

    log_d("Finding version in binary %s", path);

    cu_fd fd = open(path, O_RDONLY);
    if (fd == -1) {
        set_error(IO, "Cannot open binary file");
        FAIL;
    }

    if (fstat(fd, &s) == -1) {
        set_error(IO, "Cannot determine size of binary file");
        FAIL;
    }

    binary_size = s.st_size;

    cu_map_t* binary_map = map_new(fd, binary_size, MAP_PRIVATE);
    if (!isvalid(binary_map)) {
        set_error(IO, "Cannot map binary file to memory");
        FAIL;
    }

    char   needle[3]    = {0x00, '3', '.'};
    size_t current_size = binary_size;
    char*  current_pos  = binary_map->addr;
    int    major, minor, patch;
    major = 0;
    while (true) {
        char* p = memmem(current_pos, current_size, needle, sizeof(needle));
        if (!isvalid(p))
            break;
        if (sscanf(++p, "%d.%d.%d", &major, &minor, &patch) == 3)
            break;
        current_size -= p - current_pos + sizeof(needle);
        current_pos   = p + sizeof(needle);
    }

    if (major >= 3) {
        *version = PYVERSION(major, minor, patch);
        SUCCESS;
    }

    set_error(VERSION, "Cannot find Python version from binary");
    FAIL;
} /* _find_version_in_binary */
#endif

static int
_py_proc__infer_python_version(py_proc_t* self) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid process structure");
        FAIL;
    } // GCOV_EXCL_STOP

    int major = 0, minor = 0, patch = 0;

    // Starting with Python 3.13 we can use the PyRuntime structure
    if (isvalid(self->symbols[DYNSYM_RUNTIME])) {
        _Py_DebugOffsets py_d;
        if (fail(py_proc__get_type(self, self->symbols[DYNSYM_RUNTIME], py_d))) {
            log_e("Cannot copy PyRuntimeState structure from remote address");
            FAIL;
        }

        if (0 == memcmp(py_d.v3_13.cookie, _Py_Debug_Cookie, sizeof(py_d.v3_13.cookie))) {
            uint64_t version = py_d.v3_13.version;
            major            = (version >> 24) & 0xFF;
            minor            = (version >> 16) & 0xFF;
            patch            = (version >> 8) & 0xFF;

            log_d("Python version (from debug offsets): %d.%d.%d", major, minor, patch);

            self->py_v = get_version_descriptor(major, minor, patch);
            if (!isvalid(self->py_v)) { // GCOV_EXCL_START
                FAIL;
            } // GCOV_EXCL_STOP

            init_version_descriptor(self->py_v, &py_d);

            SUCCESS;
        }
        log_d("PyRuntimeState structure does not match expected cookie");
    }

    // Starting with Python 3.11 we can rely on the Py_Version symbol
    if (isvalid(self->symbols[DYNSYM_HEX_VERSION])) {
        unsigned long py_version = 0;

        if (fail( // GCOV_EXCL_START
                py_proc__memcpy(self, self->symbols[DYNSYM_HEX_VERSION], sizeof(py_version), &py_version)
            )) {
            FAIL;
        } // GCOV_EXCL_STOP

        major = (py_version >> 24) & 0xFF;
        minor = (py_version >> 16) & 0xFF;
        patch = (py_version >> 8) & 0xFF;

        log_d("Python version (from symbol): %d.%d.%d", major, minor, patch);

        self->py_v = get_version_descriptor(major, minor, patch);
        if (!isvalid(self->py_v)) { // GCOV_EXCL_START
            FAIL;
        } // GCOV_EXCL_STOP

        SUCCESS;
    }

    // Try to infer the Python version from the library file name.
    if (isvalid(self->lib_path)
        && success(_get_version_from_filename(self->lib_path, LIB_NEEDLE, &major, &minor, &patch)))
        goto from_filename;

// On Linux, the actual executable is sometimes picked as a library. Hence we
// try to execute the library first and see if we get a version from it. If
// not, we fall back to the actual binary, if any.
#if defined PL_UNIX
    if (isvalid(self->lib_path) && (success(_get_version_from_executable(self->lib_path, &major, &minor, &patch))))
        goto from_exe;
#endif

    if (isvalid(self->bin_path) && (success(_get_version_from_executable(self->bin_path, &major, &minor, &patch))))
        goto from_exe;

    // Try to infer the Python version from the executable file name.
    if (isvalid(self->bin_path)
        && success(_get_version_from_filename(self->bin_path, "python", &major, &minor, &patch)))
        goto from_filename;

#if defined PL_MACOS
    if (major == 0) {
        // We still haven't found a Python version so we look at the binary
        // content for clues
        int version;
        if (isvalid(self->bin_path) && (success(_find_version_in_binary(self->bin_path, &version)))) {
            log_d("Python version (from binary content): %d.%d.%d", major, minor, patch);
            self->py_v = get_version_descriptor(MAJOR(version), MINOR(version), PATCH(version));
            if (!isvalid(self->py_v)) {
                FAIL;
            }

            SUCCESS;
        }
    }
#endif

    set_error(VERSION, "Cannot infer Python version");
    FAIL;

from_exe:
    log_d("Python version (from executable): %d.%d.%d", major, minor, patch);
    goto set_version;

from_filename:
    log_d("Python version (from file name): %d.%d.%d", major, minor, patch);
    goto set_version;

set_version:
    self->py_v = get_version_descriptor(major, minor, patch);
    if (!isvalid(self->py_v)) { // GCOV_EXCL_START
        FAIL;
    } // GCOV_EXCL_STOP

    SUCCESS;
}

// ----------------------------------------------------------------------------
// Get an interpreter state field from the prefetch buffer or fall back to
// copying the field from the remote process.
#define _py_proc__get_interpreter_state_field(self, interp, field, dst)                          \
    (self->py_v->py_is.o_##field >= self->interpreter_state_com.base_offset                      \
             && self->py_v->py_is.o_##field                                                      \
                    < self->interpreter_state_com.base_offset + self->interpreter_state_com.size \
         ? memcpy(                                                                               \
               &dst,                                                                             \
               self->interpreter_state_com.data                                                  \
                   + (self->py_v->py_is.o_##field - self->interpreter_state_com.base_offset),    \
               sizeof(dst)                                                                       \
           ) != &dst                                                                             \
         : py_proc__copy_field_v(self, is, field, interp, dst))

// ----------------------------------------------------------------------------
static int
_py_proc__check_interp_state(py_proc_t* self, raddr_t interp) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid process structure");
        FAIL;
    } // GCOV_EXCL_STOP

    V_DESC(self->py_v);

    V_ALLOCA(thread, tstate);

    raddr_t tstate_head = NULL;
    if (fail(_py_proc__get_interpreter_state_field(self, interp, tstate_head, tstate_head)))
        FAIL;

    if (fail(py_proc__copy_v(self, thread, tstate_head, &tstate)))
        FAIL;

    log_t("PyThreadState head loaded @ %p", V_FIELD(raddr_t, is, py_is, o_tstate_head));

    if (V_FIELD(raddr_t, tstate, py_thread, o_interp) != interp) {
        set_error(PYOBJECT, "PyThreadState head does not point to interpreter state");
        FAIL;
    }

    log_d("Found possible interpreter state @ %p (offset %p).", interp, interp - self->map.exe.base);

    py_thread_t thread       = py_thread__init(self);
    raddr_t     thread_raddr = NULL;
    if (fail(_py_proc__get_interpreter_state_field(self, interp, tstate_head, thread_raddr))) // GCOV_EXCL_LINE
        FAIL;                                                                                 // GCOV_EXCL_LINE

    if (fail(py_thread__read_remote(&thread, thread_raddr))) // GCOV_EXCL_LINE
        FAIL;                                                // GCOV_EXCL_LINE

    log_d("Stack trace constructed from possible interpreter state");

    self->gc_state_raddr = (raddr_t)(((char*)interp) + py_v->py_is.o_gc);
    log_d("GC runtime state @ %p", self->gc_state_raddr);

    if (V_MIN(3, 11)) {
        // In Python 3.11 we can make use of the native_thread_id field on Linux
        // to get the thread id. We need to destroy the stack chunk though, to avoid
        // a memory leak.
        stack_chunk__destroy(thread.stack);

        SUCCESS;
    }

#if defined PL_LINUX
    // Try to determine the TID by reading the remote struct pthread structure.
    // We can then use this information to parse the appropriate procfs file and
    // determine the native thread's running state.
    raddr_t initial_thread_addr = thread.addr;
    while (isvalid(thread.addr)) {
        if (success(_infer_tid_field_offset(&thread)))
            SUCCESS;
        if (!error_is(OS)) // GCOV_EXCL_START
            FAIL;

        if (fail(py_thread__next(&thread))) {
            log_d("Failed to get next thread while inferring TID field offset");
            FAIL;
        }

        if (thread.addr == initial_thread_addr)
            break;
    }
    log_d("tid field offset not ready");
    FAIL;
    // GCOV_EXCL_STOP
#endif /* PL_LINUX */

    SUCCESS;
}

// ----------------------------------------------------------------------------
static int
_py_proc__scan_bss(py_proc_t* self) {
    // Starting with Python 3.11, BSS scans fail because it seems that the
    // interpreter state is stored in the data section. In this case, we shift
    // our data queries into the data section. We then take steps of 64KB
    // backwards and try to find the interpreter state. This is a bit of a hack
    // for now, but it seems to work with decent performance. Note that if we
    // fail the first scan, we then look for actual interpreter states rather
    // than pointers to it. This make the search a little slower, since we now
    // have to check every value in the range. However, the step size we chose
    // seems to get us close enough in a few attempts.
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid process structure");
        FAIL;
    } // GCOV_EXCL_STOP

    if (!isvalid(self->map.bss.base)) { // GCOV_EXCL_START
        set_error(BINARY, "Invalid BSS section");
        FAIL;
    } // GCOV_EXCL_STOP

    cu_void* bss = malloc(self->map.bss.size);
    if (!isvalid(bss)) { // GCOV_EXCL_START
        set_error(MALLOC, "Cannot allocate memory for BSS scan");
        FAIL;
    } // GCOV_EXCL_STOP

    size_t step = self->map.bss.size > 0x10000 ? 0x10000 : self->map.bss.size;

    for (int shift = 0; shift < 1; shift++) {
        raddr_t base = self->map.bss.base - (shift * step);
        if (fail(py_proc__memcpy(self, base, self->map.bss.size, bss))) { // GCOV_EXCL_START
            FAIL;
        } // GCOV_EXCL_STOP

        log_d("Scanning the BSS section @ %p (shift %d)", base, shift);

        void* upper_bound = (void*)((char*)bss + (shift ? step : self->map.bss.size));
        for (register raddr_t* raddr = (raddr_t*)bss; (raddr_t)raddr < upper_bound; raddr++) {
            if ((!shift && success(_py_proc__check_interp_state(self, *raddr)))
                || (shift && success(_py_proc__check_interp_state(self, (raddr_t)raddr - bss + base)))) {
                log_d(
                    "Possible interpreter state referenced by BSS @ %p (offset %x)",
                    (raddr_t)raddr - (raddr_t)bss + (raddr_t)base, (raddr_t)raddr - (raddr_t)bss
                );
                self->istate_raddr = shift ? (raddr_t)raddr - bss + base : *raddr;
                SUCCESS;
            }

            // If we don't have symbols we tolerate memory copy errors.
            if (error_is(OS) || (self->sym_loaded && error_is(MEMCOPY)))
                FAIL;
        }
#if defined PL_WIN
        break;
#endif
    }

    set_error(OS, "Uninitialized data section scan failed"); // GCOV_EXCL_LINE
    FAIL;                                                    // GCOV_EXCL_LINE
}

// ----------------------------------------------------------------------------
static inline int
_py_proc__prefetch_interpreter_state(py_proc_t* self, raddr_t interp) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid process structure");
        FAIL;
    } // GCOV_EXCL_STOP

    // The interpreter state structure is quite large, so we prefetch the
    // chunk that we are more likely to need.
    if (fail(copy_memory(
            self->ref, interp + self->interpreter_state_com.base_offset, self->interpreter_state_com.size,
            self->interpreter_state_com.data
        ))) {
        FAIL;
    }

    SUCCESS;
}

// ----------------------------------------------------------------------------
static int
_py_proc__deref_interp_head(py_proc_t* self) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid process structure");
        FAIL;
    } // GCOV_EXCL_STOP

    if (!(isvalid(self->symbols[DYNSYM_RUNTIME]) || isvalid(self->map.runtime.base))) { // GCOV_EXCL_START
        set_error(OS, "Invalid runtime section");
        FAIL;
    } // GCOV_EXCL_STOP

    V_DESC(self->py_v);

    V_ALLOCA(runtime, runtime);

    raddr_t interp_head_raddr = NULL;

    raddr_t runtime_addr = self->symbols[DYNSYM_RUNTIME];
#if defined PL_LINUX
    const size_t size = getpagesize();
#else
    const size_t size = 0;
#endif

    raddr_t lower = isvalid(runtime_addr) ? runtime_addr : self->map.runtime.base;
    raddr_t upper = isvalid(runtime_addr) ? runtime_addr : lower + size;

#ifdef DEBUG
    if (isvalid(runtime_addr)) {
        log_d("Using runtime state symbol @ %p", runtime_addr);
    } else {
        log_d("Using runtime state section @ %p-%p", lower, upper);
    }
#endif

    for (raddr_t current_addr = lower; current_addr <= upper; current_addr += sizeof(raddr_t)) {
        if (py_proc__copy_v(self, runtime, current_addr, &runtime)) {
            log_d("Cannot copy runtime state structure from remote address %p", current_addr);
            continue; // GCOV_EXCL_LINE
        }

        interp_head_raddr = V_FIELD(raddr_t, runtime, py_runtime, o_interp_head);

        if (fail(_py_proc__prefetch_interpreter_state(self, interp_head_raddr))) {
            log_d("Failed to prefetch interpreter state from runtime state @ %p", interp_head_raddr);
            interp_head_raddr = NULL; // GCOV_EXCL_LINE
            continue;                 // GCOV_EXCL_LINE
        }

        if (fail(_py_proc__check_interp_state(self, interp_head_raddr))) {
            log_d("Interpreter state check failed while dereferencing runtime state");
            interp_head_raddr = NULL;
            continue;
        }
    }

    if (!isvalid(interp_head_raddr)) {
        log_d("Cannot dereference PyInterpreterState head from runtime state");
        FAIL;
    }

    self->istate_raddr = interp_head_raddr;

    SUCCESS;
}

// ----------------------------------------------------------------------------
static inline raddr_t
_py_proc__current_thread_state(py_proc_t* self) {
    raddr_t p_tstate_current = NULL;

    if (self->symbols[DYNSYM_RUNTIME] != NULL) {
        if (self->tstate_current_offset == 0
            || py_proc__get_type(self, self->symbols[DYNSYM_RUNTIME] + self->tstate_current_offset, p_tstate_current))
            return (raddr_t)-1;

        return p_tstate_current;
    }

    return (raddr_t)-1; // GCOV_EXCL_LINE
}

// ----------------------------------------------------------------------------
static int
_py_proc__find_interpreter_state(py_proc_t* self) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid process structure");
        FAIL;
    } // GCOV_EXCL_STOP

    if (fail(_py_proc__init(self)))
        FAIL;

    // Determine and set version
    if (fail(_py_proc__infer_python_version(self)))
        FAIL;

    if (self->sym_loaded || isvalid(self->map.runtime.base)) {
        // Try to resolve the symbols or the runtime section, if we have them

        self->istate_raddr = NULL;

        if (fail(_py_proc__deref_interp_head(self))) {
            log_d("Cannot dereference PyInterpreterState head from symbols (pid: %d)", self->pid);
            FAIL;
        }

        log_d("Interpreter head resolved from symbols");
    } else {
        // Attempt a BSS scan if we don't have symbols
        if (fail(_py_proc__scan_bss(self))) { // GCOV_EXCL_START
            log_d("BSS scan failed (no symbols available)");
            FAIL;
        } // GCOV_EXCL_STOP

        log_d("Interpreter state located from BSS scan (no symbols available)");
    }

    SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_proc__init(py_proc_t* self) {
    bool try_once = self->child;
    bool init     = false;
    int  attempts = 0;

#ifdef DEBUG
    if (!try_once)
        log_d("Start up timeout: %d ms", pargs.timeout / 1000);
    else
        log_d("Single attempt to attach to process %d", self->pid);
#endif

    TIMER_START(pargs.timeout)
    if (try_once && ++attempts > 1) {
        set_error(OS, "Cannot one-shot attach");
        FAIL;
    }

    if (!py_proc__is_running(self)) {
        set_error(OS, "Process is not running");
        FAIL;
    }

    sfree(self->bin_path);
    sfree(self->lib_path);
    self->sym_loaded = false;

    if (success(_py_proc__find_interpreter_state(self))) {
        init = true;

        log_d("Interpreter State de-referenced @ raddr: %p after %d attempts", self->istate_raddr, attempts);

        TIMER_STOP;
    }

    TIMER_END

    if (!init) {
        log_d("Interpreter state search timed out");
        // Nothing more we can do if we don't have a version or permissions
        if (error_is(VERSION) || error_is(PERM))
            FAIL;
        if (!isvalid(self->py_v)) {
            set_error(VERSION, "No valid Python version detected");
            FAIL;
        }
#if defined PL_LINUX
        // This check only applies to Linux, because we don't have permission issues
        // on Windows, and if we got here on MacOS, we are already running with
        // sudo, so this is likely not a Python we can profile.
        if (error_is(PERM))
            // We are likely going to fail a BSS scan so we fail
            FAIL;
#endif

        // Scan the BSS section as a last resort
        if (fail(_py_proc__scan_bss(self))) {
            FAIL;
        }

        log_d("Interpreter state located from BSS scan");
    }

    if (!(isvalid(self->bin_path) || isvalid(self->lib_path)))
        log_w("No Python binary files detected");

    if (self->symbols[DYNSYM_RUNTIME] == NULL && self->gc_state_raddr == NULL)
        log_w("No remote symbol references have been set.");

#ifdef DEBUG
    if (self->bin_path != NULL)
        log_d("Python binary:  %s", self->bin_path);
    if (self->lib_path != NULL)
        log_d("Python library: %s", self->lib_path);
#endif

    self->timestamp = gettime();

#ifdef NATIVE
    self->unwind.as = unw_create_addr_space(&_UPT_accessors, 0);
#endif

    V_DESC(self->py_v);

    size_t page_size = get_page_size();
    if (page_size > env.page_size_cap) {
        log_d("Page size %zu is larger than the configured cap %zu, using cap instead", page_size, env.page_size_cap);
        page_size = env.page_size_cap;
    }

    // Because the structure fields are all of type long, we should not have
    // alignment issues in this computation.
    size_t    com = 0;
    int       n   = (sizeof(py_is_v) - sizeof(ssize_t)) / sizeof(offset_t);
    offset_t* o   = (offset_t*)(((ssize_t*)&py_v->py_is.size) + 1);
    for (register int i = 0; i < n; i++, com += *(o++)) {}
    com /= n;

    self->interpreter_state_com.base_offset = com & ~(page_size - 1);
    self->interpreter_state_com.size        = page_size;
    if (unlikely(self->interpreter_state_com.base_offset + page_size > py_v->py_is.size)) {
        self->interpreter_state_com.size = py_v->py_is.size - self->interpreter_state_com.base_offset;
    }
    self->interpreter_state_com.data = malloc(self->interpreter_state_com.size);

    log_d(
        "Interpreter state CoM(base=%zu, size=%zu, fields=%d)", self->interpreter_state_com.base_offset,
        self->interpreter_state_com.size, n
    );

    log_d("Python process initialization successful");

    SUCCESS;
} /* py_proc__init */

// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
py_proc_t*
py_proc_new(bool child) {
    py_proc_t* py_proc = (py_proc_t*)calloc(1, sizeof(py_proc_t));
    if (!isvalid(py_proc)) // GCOV_EXCL_LINE
        FAIL_PTR;          // GCOV_EXCL_LINE

    py_proc->child          = child;
    py_proc->gc_state_raddr = NULL;
    py_proc->py_v           = NULL;

    _prehash_symbols();

    py_proc->frame_cache = lru_cache_new(MAX_FRAME_CACHE_SIZE, (void (*)(value_t))frame__destroy);
    if (!isvalid(py_proc->frame_cache)) { // GCOV_EXCL_START
        FAIL_GOTO(error);
    } // GCOV_EXCL_STOP
#ifdef DEBUG
    py_proc->frame_cache->name = "frame cache";
#endif

    py_proc->string_cache = lru_cache_new(MAX_STRING_CACHE_SIZE, (void (*)(value_t))cached_string_destroy);
    if (!isvalid(py_proc->string_cache)) { // GCOV_EXCL_START
        FAIL_GOTO(error);
    } // GCOV_EXCL_STOP
#ifdef DEBUG
    py_proc->string_cache->name = "string cache";
#endif

    py_proc->code_cache = lru_cache_new(MAX_CODE_CACHE_SIZE, (void (*)(value_t))code__destroy);
    if (!isvalid(py_proc->code_cache)) { // GCOV_EXCL_START
        FAIL_GOTO(error);
    } // GCOV_EXCL_STOP
#ifdef DEBUG
    py_proc->code_cache->name = "code cache";
#endif

    py_proc->interpreter_state_cache
        = lru_cache_new(MAX_INTERPRETER_STATE_CACHE_SIZE, (void (*)(value_t))interpreter_state__destroy);
    if (!isvalid(py_proc->interpreter_state_cache)) { // GCOV_EXCL_START
        FAIL_GOTO(error);
    } // GCOV_EXCL_STOP
#ifdef DEBUG
    py_proc->interpreter_state_cache->name = "interpreter state cache";
#endif

    py_proc->extra = (proc_extra_info*)calloc(1, sizeof(proc_extra_info));
    if (!isvalid(py_proc->extra)) { // GCOV_EXCL_START
        set_error(MALLOC, "Cannot allocate memory for process extra info");
        FAIL_GOTO(error);
    } // GCOV_EXCL_STOP

    return py_proc;

error: // GCOV_EXCL_START
    free(py_proc);
    return NULL;
} // GCOV_EXCL_STOP

// ----------------------------------------------------------------------------
int
py_proc__attach(py_proc_t* self, pid_t pid) {
    log_d("Attaching to process with PID %d", pid);

#if defined PL_WIN /* WIN */
    self->ref = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (self->ref == INVALID_HANDLE_VALUE) {
        set_error(OS, "Failed to open attach process");
        FAIL;
    }
#endif /* ANY */

    self->pid = pid;

#if defined PL_LINUX /* LINUX */
    self->ref = pid;
#endif

    if (fail(py_proc__init(self))) {
        FAIL;
    }

    SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_proc__start(py_proc_t* self, const char* exec, char* argv[]) {
    log_d("Starting new process using the command: %s", exec);

#ifdef PL_WIN /* WIN */
    PROCESS_INFORMATION piProcInfo;
    STARTUPINFO         siStartInfo;
    SECURITY_ATTRIBUTES saAttr;
    HANDLE              hChildStdInRd  = NULL;
    HANDLE              hChildStdInWr  = NULL;
    HANDLE              hChildStdOutRd = NULL;
    HANDLE              hChildStdOutWr = NULL;

    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));

    saAttr.nLength              = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle       = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    CreatePipe(&hChildStdInRd, &hChildStdInWr, &saAttr, 0);
    CreatePipe(&hChildStdOutRd, &hChildStdOutWr, &saAttr, 0);

    SetHandleInformation(hChildStdInWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStdOutRd, HANDLE_FLAG_INHERIT, 0);

    siStartInfo.cb          = sizeof(STARTUPINFO);
    siStartInfo.hStdInput   = hChildStdInRd;
    siStartInfo.hStdOutput  = GetStdHandle(STD_OUTPUT_HANDLE);
    siStartInfo.hStdError   = GetStdHandle(STD_ERROR_HANDLE);
    siStartInfo.dwFlags    |= STARTF_USESTDHANDLES;

    if (pargs.output_file == stdout) {
        log_d("Redirecting child's STDOUT to a pipe");
        siStartInfo.hStdOutput = hChildStdOutWr;

        // On Windows, Python is normally started by a launcher that duplicates the
        // standard streams, so redirecting to the NULL device causes issues. To
        // support these cases, we spawn a reader thread that reads from the pipe
        // and ensures that the buffer never gets full, stalling STDOUT operations
        // in the child process.
        DWORD dwThreadId;
        self->extra->h_reader_thread = CreateThread(NULL, 0, reader_thread, hChildStdOutRd, 0, &dwThreadId);
        if (self->extra->h_reader_thread == NULL) {
            set_error(OS, "Failed to create stdout reader thread");
            FAIL;
        }
    }

    // Concatenate the command line arguments
    register int cmd_line_size = strlen(exec) + 3; // 1 for ' ' + 2 for potential '"'s
    register int i             = 1;
    while (argv[i])
        cmd_line_size += strlen(argv[i++]) + 3;

    char* cmd_line = malloc(sizeof(char) * cmd_line_size);
    if (!isvalid(cmd_line)) {
        set_error(MALLOC, "Cannot allocate memory for command line");
        FAIL;
    }
    strcpy(cmd_line, exec);

    register int pos = strlen(exec);
    i                = 1;
    while (argv[i]) {
        bool has_space  = isvalid(strchr(argv[i], ' '));
        cmd_line[pos++] = ' ';
        if (has_space)
            cmd_line[pos++] = '"';
        strcpy(cmd_line + pos, argv[i]);
        pos += strlen(argv[i++]);
        if (has_space)
            cmd_line[pos++] = '"';
    }
    cmd_line[pos] = '\0';

    log_t("Computed command line: %s", cmd_line);

    BOOL process_created = CreateProcess(
        NULL, cmd_line, NULL, NULL, TRUE, CREATE_NEW_PROCESS_GROUP, NULL, NULL, &siStartInfo, &piProcInfo
    );

    sfree(cmd_line);

    if (!process_created) {
        set_error(OS, "Failed to create process");
        FAIL;
    }
    self->ref = piProcInfo.hProcess;
    self->pid = (pid_t)piProcInfo.dwProcessId;

    CloseHandle(hChildStdInRd);
    CloseHandle(hChildStdOutWr);

    // Create a job for Austin
    HANDLE hJob = self->extra->h_job = CreateJobObject(NULL, NULL);
    if (!isvalid(hJob)) {
        set_error(OS, "Failed to create job object");
        FAIL;
    }
    // Set job limits to close all processes when Austin terminates
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        set_error(OS, "Failed to set job information");
        FAIL;
    }
    // Assign the child process to the job
    if (!AssignProcessToJobObject(hJob, self->ref)) {
        set_error(OS, "Failed to assign process to job");
        FAIL;
    }

#else  /* UNIX */
    self->pid = fork();
    if (self->pid == 0) {
        // If we are not writing to file we need to ensure the child process is
        // not writing to stdout.
        if (pargs.output_file == stdout) {
            log_d("Redirecting child's STDOUT to " NULL_DEVICE);
            if (freopen(NULL_DEVICE, "w", stdout) == NULL)                       // GCOV_EXCL_LINE
                set_error(IO, "Cannot redirect child's STDOUT to " NULL_DEVICE); // GCOV_EXCL_LINE
        }

        // Create a new process group so that we can send signals to the parent
        // process we spawned without affecting any of our parents.
        setpgid(0, 0);

        execvpe(exec, argv, environ);

        exit(127);
    }
#endif /* ANY */

#if defined PL_LINUX
    self->ref = self->pid;

    // On Linux we need to wait for the forked process or otherwise it will
    // become a zombie and we cannot tell with kill if it has terminated.
    pthread_create(&(self->extra->wait_thread_id), NULL, wait_thread, (void*)self);
    log_d("Wait thread created with ID %x", self->extra->wait_thread_id);
#endif

    log_d("New process created with PID %d", self->pid);

    if (fail(py_proc__init(self))) {
        FAIL;
    }

#ifdef NATIVE
    self->timestamp = gettime();
#endif

    if (self->pid == 0) { // GCOV_EXCL_START
        set_error(OS, "Failed to start process");
        FAIL;
    } // GCOV_EXCL_STOP

    log_d("Python process started successfully");

    SUCCESS;
}

// ----------------------------------------------------------------------------
void
py_proc__wait(py_proc_t* self) {
    log_d("Waiting for process %d to terminate", self->pid);

#if defined PL_LINUX
    if (self->extra->wait_thread_id) {
        pthread_join(self->extra->wait_thread_id, NULL);
    }
#endif

#ifdef PL_WIN /* WIN */
    if (isvalid(self->extra->h_reader_thread)) {
        WaitForSingleObject(self->extra->h_reader_thread, INFINITE);
        CloseHandle(self->extra->h_reader_thread);
    }
    WaitForSingleObject(self->ref, INFINITE);
    CloseHandle(self->ref);
#else /* UNIX */
#ifdef NATIVE
    wait(NULL);
#else
    waitpid(self->pid, 0, 0);
#endif
#endif
}

// ----------------------------------------------------------------------------
#define PYRUNTIMESTATE_SIZE 2048 // We expect _PyRuntimeState to be < 2K.

static inline int
_py_proc__find_current_thread_offset(py_proc_t* self, raddr_t thread_raddr) {
    if (!isvalid(self->symbols[DYNSYM_RUNTIME])) { // GCOV_EXCL_START
        set_error(OS, "Invalid runtime symbol");
        FAIL;
    } // GCOV_EXCL_STOP

    V_DESC(self->py_v);

    V_ALLOCA(runtime, runtime);

    if (py_proc__copy_v(self, runtime, self->symbols[DYNSYM_RUNTIME], &runtime)) // GCOV_EXCL_LINE
        FAIL;                                                                    // GCOV_EXCL_LINE

    // Search offset of current thread in _PyRuntimeState structure
    raddr_t      current_thread_raddr = NULL;
    register int hit_count            = 0;
    for (register raddr_t* raddr = (raddr_t*)self->symbols[DYNSYM_RUNTIME];
         (raddr_t)raddr < (raddr_t)(((char*)self->symbols[DYNSYM_RUNTIME]) + PYRUNTIMESTATE_SIZE); raddr++) {
        py_proc__get_type(self, raddr, current_thread_raddr);
        if (current_thread_raddr == thread_raddr) {
            if (++hit_count == 2) {
                self->tstate_current_offset = (raddr_t)raddr - self->symbols[DYNSYM_RUNTIME];
                log_d("Offset of _PyRuntime.gilstate.tstate_current found at %x", self->tstate_current_offset);
                SUCCESS;
            }
        }
    }

    set_error(OS, "Cannot find current thread offset"); // GCOV_EXCL_LINE
    FAIL;                                               // GCOV_EXCL_LINE
}

// ----------------------------------------------------------------------------
bool
py_proc__is_running(py_proc_t* self) {
#if defined PL_WIN /* WIN */
    DWORD ec = 0;
    return GetExitCodeProcess(self->ref, &ec) ? ec == STILL_ACTIVE : 0;

#elif defined PL_MACOS /* MACOS */
    return success(check_pid(self->pid));

#else /* LINUX */
    return !(kill(self->pid, 0) == -1 && errno == ESRCH);
#endif
}

// ----------------------------------------------------------------------------
bool
py_proc__is_python(py_proc_t* self) {
    return self->istate_raddr != NULL;
}

// ----------------------------------------------------------------------------
static inline ssize_t
_py_proc__get_memory_delta(py_proc_t* self) {
    ssize_t current_memory     = _py_proc__get_resident_memory(self);
    ssize_t delta              = current_memory - self->last_resident_memory;
    self->last_resident_memory = current_memory;

    return delta;
}

// ----------------------------------------------------------------------------
int
py_proc__get_gc_state(py_proc_t* self) {
    if (!isvalid(self->gc_state_raddr))
        return GC_STATE_UNKNOWN; // GCOV_EXCL_LINE

    V_DESC(self->py_v);

    GCRuntimeState gc_state;
    if (fail(py_proc__get_type(self, self->gc_state_raddr, gc_state))) {
        log_d("Failed to get GC runtime state");
        return GC_STATE_UNKNOWN; // GCOV_EXCL_LINE
    }

    return V_FIELD(int, gc_state, py_gc, o_collecting);
}

#ifdef NATIVE
// ----------------------------------------------------------------------------
static int
_py_proc__interrupt_threads(py_proc_t* self, raddr_t tstate_head) {
    py_thread_t py_thread = py_thread__init(self);

    if (fail(py_thread__read_remote(&py_thread, tstate_head))) { // GCOV_EXCL_START
        FAIL;
    } // GCOV_EXCL_STOP

    do {
        if (pargs.kernel && fail(py_thread__save_kernel_stack(&py_thread))) // GCOV_EXCL_LINE
            FAIL;                                                           // GCOV_EXCL_LINE

        // !IMPORTANT! We need to retrieve the idle state *before* trying to
        // interrupt the thread, else it will always be idle!
        if (fail(py_thread__set_idle(&py_thread))) // GCOV_EXCL_LINE
            FAIL;                                  // GCOV_EXCL_LINE

        if (fail(wait_ptrace(PTRACE_INTERRUPT, py_thread.tid, 0, 0))) // GCOV_EXCL_LINE
            FAIL;                                                     // GCOV_EXCL_LINE

        if (fail(py_thread__set_interrupted(&py_thread, true))) { // GCOV_EXCL_START
            if (fail(wait_ptrace(PTRACE_CONT, py_thread.tid, 0, 0))) {
                log_d("ptrace: failed to resume interrupted thread %d (errno: %d)", py_thread.tid, errno);
            }
            FAIL;
        } // GCOV_EXCL_STOP

        log_t("ptrace: thread %d interrupted", py_thread.tid);
    } while (success(py_thread__next(&py_thread)));

    if (!error_is(ITEREND)) // GCOV_EXCL_LINE
        FAIL;               // GCOV_EXCL_LINE

    SUCCESS;
}

// ----------------------------------------------------------------------------
static int
_py_proc__resume_threads(py_proc_t* self, raddr_t tstate_head) {
    py_thread_t py_thread = py_thread__init(self);

    if (fail(py_thread__read_remote(&py_thread, tstate_head))) { // GCOV_EXCL_START
        FAIL;
    } // GCOV_EXCL_STOP

    do {
        if (py_thread__is_interrupted(&py_thread)) {
            if (fail(wait_ptrace(PTRACE_CONT, py_thread.tid, 0, 0))) // GCOV_EXCL_LINE
                FAIL;                                                // GCOV_EXCL_LINE

            log_t("ptrace: thread %d resumed", py_thread.tid);
            if (fail(py_thread__set_interrupted(&py_thread, false))) { // GCOV_EXCL_START
                FAIL;
            } // GCOV_EXCL_STOP
        }
    } while (success(py_thread__next(&py_thread)));

    if (!error_is(ITEREND)) // GCOV_EXCL_LINE
        FAIL;               // GCOV_EXCL_LINE

    SUCCESS;
}
#endif

// ----------------------------------------------------------------------------
static inline int
_py_proc__sample_interpreter(py_proc_t* self, raddr_t interp, microseconds_t time_delta) {
    ssize_t mem_delta      = 0;
    raddr_t current_thread = NULL;

    V_DESC(self->py_v);

    raddr_t tstate_head = NULL;
    if (fail(_py_proc__get_interpreter_state_field(self, interp, tstate_head, tstate_head))) // GCOV_EXCL_LINE
        FAIL;                                                                                // GCOV_EXCL_LINE

    if (!isvalid(tstate_head)) { // GCOV_EXCL_START
        set_error(PYOBJECT, "Invalid thread state head address");
        FAIL;
    } // GCOV_EXCL_STOP

    py_thread_t py_thread = py_thread__init(self);

    if (fail(py_thread__read_remote(&py_thread, tstate_head))) {
        if (is_fatal(austin_errno)) {
            FAIL;
        }
        SUCCESS;
    }

    if (pargs.memory) {
        // Use the current thread to determine which thread is manipulating memory
        if (V_MIN(3, 12)) {
            raddr_t gil_state_raddr = NULL;
            if (fail(_py_proc__get_interpreter_state_field(self, interp, gil_state, gil_state_raddr))) // GCOV_EXCL_LINE
                FAIL;                                                                                  // GCOV_EXCL_LINE

            if (!isvalid(gil_state_raddr)) // GCOV_EXCL_LINE
                SUCCESS;                   // GCOV_EXCL_LINE

            gil_state_t gil_state = {0};
            if (fail(copy_datatype(self->ref, gil_state_raddr, gil_state))) // GCOV_EXCL_LINE
                FAIL;                                                       // GCOV_EXCL_LINE

            current_thread = (raddr_t)gil_state.last_holder._value;
        } else
            current_thread = _py_proc__current_thread_state(self);
    }

    int64_t interp_id = 0;
    if (fail(_py_proc__get_interpreter_state_field(self, interp, id, interp_id))) // GCOV_EXCL_LINE
        FAIL;                                                                     // GCOV_EXCL_LINE

    // In Python 3.14 we can use the code object generation to determine if we
    // need to invalidate the frame cache.
    if (V_MIN(3, 14)) {
        uint64_t code_object_gen = 0;
        if (fail( // GCOV_EXCL_LINE
                _py_proc__get_interpreter_state_field(self, interp, code_object_gen, code_object_gen)
            ))
            FAIL; // GCOV_EXCL_LINE

        key_dt               key                    = interpreter_state_key(interp_id);
        interpreter_state_t* interpreter_state_info = lru_cache__maybe_hit(self->interpreter_state_cache, key);
        if (!isvalid(interpreter_state_info)) {
            interpreter_state_info = interpreter_state_new(interp_id, code_object_gen);
            if (!isvalid(interpreter_state_info)) // GCOV_EXCL_LINE
                FAIL;                             // GCOV_EXCL_LINE

            log_d(
                "Creating new interpreter state info record for interpreter %lx with code object generation %lu",
                interp_id, code_object_gen
            );

            lru_cache__store(self->interpreter_state_cache, key, interpreter_state_info);
        }

        if (code_object_gen != interpreter_state_info->code_object_gen) {
            log_d(
                "Code object generation changed from %lu to %lu, invalidating frame cache",
                interpreter_state_info->code_object_gen, code_object_gen
            );

            // This is the only safe place where we can invalidate the frame
            // cache. Doing it while in the middle of unwinding is dangerous
            // because the frames that are put in the stack are owned by the
            // cache and we might end up with dangling pointers.
            lru_cache__invalidate(self->frame_cache);
            lru_cache__invalidate(self->code_cache);

            interpreter_state_info->code_object_gen = code_object_gen;
        }
    }

    do {
        if (pargs.memory) {
            mem_delta = 0;
            if (V_MAX(3, 11) && self->symbols[DYNSYM_RUNTIME] != NULL && current_thread == (void*)-1) {
                if (_py_proc__find_current_thread_offset(self, py_thread.addr))
                    continue;
                else
                    current_thread = _py_proc__current_thread_state(self);
            }
            if (py_thread.addr == current_thread) {
                mem_delta = _py_proc__get_memory_delta(self);
                log_t("Thread %lx holds the GIL", py_thread.tid);
            }
            if (!pargs.full && mem_delta == 0)
                continue;
        }

        if (mem_delta == 0 && time_delta == 0) // GCOV_EXCL_LINE
            continue;                          // GCOV_EXCL_LINE

        bool is_idle = false;
        if (pargs.full || pargs.cpu || unlikely(pargs.where)) {
            is_idle = py_thread__is_idle(&py_thread);
            if (!pargs.full && is_idle && pargs.cpu) {
                continue;
            }
        }

        gc_state_t gc = GC_STATE_UNKNOWN;
        if (pargs.gc) {
            gc = py_proc__get_gc_state(self);
            if (gc == GC_STATE_COLLECTING) {
                stats_gc_time(time_delta);
            }
        }

        sample_t sample = {
            .pid      = self->pid,
            .tid      = py_thread.tid,
            .iid      = interp_id,
            .time     = time_delta,
            .memory   = mem_delta,
            .is_idle  = is_idle,
            .gc_state = gc,
        };
        event_handler__emit_stack_begin(&sample);

        py_thread__unwind(&py_thread);

#ifdef NATIVE
        if (V_MIN(3, 11) && V_MAX(3, 12)) {
            // We expect a CFrame to sit at the top of the stack
            if (!stack_is_empty() && stack_top() != CFRAME_MAGIC) { // GCOV_EXCL_START
                log_e("Invalid resolved Python stack");
            } // GCOV_EXCL_STOP
        }
#endif

        event_handler__emit_stack_end();
    } while (success(py_thread__next(&py_thread)));

    if (!error_is(ITEREND)) { // GCOV_EXCL_START
        FAIL;
    } // GCOV_EXCL_STOP

    SUCCESS;
} /* _py_proc__sample_interpreter */

// ----------------------------------------------------------------------------
int
py_proc__sample(py_proc_t* self) {
    microseconds_t time_delta     = gettime() - self->timestamp; // Time delta since last sample.
    raddr_t        current_interp = self->istate_raddr;

    V_DESC(self->py_v);

    do {
        if (fail(_py_proc__prefetch_interpreter_state(self, current_interp))) // GCOV_EXCL_LINE
            FAIL;                                                             // GCOV_EXCL_LINE

        raddr_t tstate_head = NULL;
        if (fail( // GCOV_EXCL_LINE
                _py_proc__get_interpreter_state_field(self, current_interp, tstate_head, tstate_head)
            ))
            FAIL; // GCOV_EXCL_LINE

        if (!isvalid(tstate_head))
            // Maybe the interpreter state is in an invalid state. We'll try again
            // unless there is a fatal error.
            SUCCESS;

#ifdef NATIVE
        if (fail(_py_proc__interrupt_threads(self, tstate_head))) // GCOV_EXCL_LINE
            FAIL;                                                 // GCOV_EXCL_LINE

        time_delta = gettime() - self->timestamp;
#endif
        int result = _py_proc__sample_interpreter(self, current_interp, time_delta);

#ifdef NATIVE
        if (fail(_py_proc__resume_threads(self, tstate_head))) // GCOV_EXCL_LINE
            FAIL;                                              // GCOV_EXCL_LINE
#endif

        if (fail(result))
            continue;

        if (fail(_py_proc__get_interpreter_state_field(self, current_interp, next, current_interp))) // GCOV_EXCL_LINE
            FAIL;                                                                                    // GCOV_EXCL_LINE
    } while (isvalid(current_interp));

#ifdef NATIVE
    self->timestamp = gettime();
#else
    self->timestamp += time_delta;
#endif

    SUCCESS;
} /* py_proc__sample */

// ----------------------------------------------------------------------------
void
py_proc__log_version(py_proc_t* self, bool is_parent) {
    int major = self->py_v->major;
    int minor = self->py_v->minor;
    int patch = self->py_v->patch;

    if (is_parent) {
        if (patch == 0xFF) {                                                 // GCOV_EXCL_LINE
            event_handler__emit_metadata("python", "%d.%d.?", major, minor); // GCOV_EXCL_LINE
        } else
            event_handler__emit_metadata("python", "%d.%d.%d", major, minor, patch);
    }

    if (pargs.pipe)
        return;

    log_m("");

    if (pargs.children) {
        if (patch == 0xFF) // GCOV_EXCL_START
            log_m(
                "🐍 %s process [" CYN "%zd" CRESET "] " BOLD "Python" CRESET " version: " BYEL "%d.%d" CRESET,
                is_parent ? "Parent" : "Child", self->pid, major, minor
            );
        else // GCOV_EXCL_STOP
            log_m(
                "🐍 %s process [" CYN "%zd" CRESET "] " BOLD "Python" CRESET " version: " BYEL "%d.%d.%d" CRESET,
                is_parent ? "Parent" : "Child", self->pid, major, minor, patch
            );
    } else {
        if (patch == 0xFF) // GCOV_EXCL_START
            log_m("🐍 " BOLD "Python" CRESET " version: " BYEL "%d.%d" CRESET, major, minor);
        else // GCOV_EXCL_STOP
            log_m("🐍 " BOLD "Python" CRESET " version: " BYEL "%d.%d.%d" CRESET, major, minor, patch);
    }
}

// ----------------------------------------------------------------------------
#if defined PL_WIN
#define SIGTERM 15
#define SIGINT  2
#endif

void
py_proc__signal(py_proc_t* self, int signal) {
#if defined PL_WIN /* WIN */
    log_d("Sending signal %d to process %d", signal, self->pid);
    switch (signal) {
    case SIGINT:
        // The child process will be closed when the parent terminates via
        // the job object.
        if (isvalid(self->extra->h_job)) {
            if (!CloseHandle(self->extra->h_job)) {
                set_error(OS, "Failed to close job handle");
                FAIL_VOID;
            }
            self->extra->h_job = NULL;
        }
    case SIGTERM:
        TerminateProcess(self->ref, signal);
        break;
    default:
        log_e("Cannot send signal %d to process %d", signal, self->pid);
        break;
    }
#else /* UNIX */
    // We send the SIGINT signal to the process group, so that we also
    // interrupt child processes, as if we were sending from a terminal with
    // Ctrl-C.
    log_d("Sending signal %d to process %d", signal, signal == SIGINT ? -getpgid(self->pid) : self->pid);
    kill(signal == SIGINT ? -getpgid(self->pid) : self->pid, signal);
#endif
}

// ----------------------------------------------------------------------------
void
py_proc__terminate(py_proc_t* self) {
    py_proc__signal(self, SIGTERM);
}

// ----------------------------------------------------------------------------
void
py_proc__destroy(py_proc_t* self) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

#ifdef NATIVE
    unw_destroy_addr_space(self->unwind.as);
    vm_range_tree__destroy(self->maps_tree);
    hash_table__destroy(self->base_table);
#endif

#if defined PL_MACOS
    mach_port_deallocate(mach_task_self(), self->ref);
#endif

    sfree(self->bin_path);
    sfree(self->lib_path);
    sfree(self->interpreter_state_com.data);
    sfree(self->extra);

    lru_cache__destroy(self->string_cache);
    lru_cache__destroy(self->frame_cache);
    lru_cache__destroy(self->code_cache);
    lru_cache__destroy(self->interpreter_state_cache);

    free(self);
}
