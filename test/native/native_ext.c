// Python C extension used by Austin's test suite to start OS threads that
// have no PyThreadState, exercising the non-Python thread sampling path.
//
// API
// ---
//   token = native_ext.start_threads(n)   -- spawn n native threads
//   native_ext.join_threads(token)        -- signal stop and join all

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdio.h>

// ---- Platform thread primitives --------------------------------------------

#ifdef _WIN32

#include <windows.h>

typedef HANDLE thread_handle_t;

typedef struct {
    volatile LONG* running;
    int            index;
} worker_arg_t;

static DWORD WINAPI
_worker(LPVOID raw) {
    worker_arg_t* a = (worker_arg_t*)raw;
    wchar_t       name[32];
    swprintf(name, 32, L"Native-%d", a->index);
    SetThreadDescription(GetCurrentThread(), name);
    while (InterlockedCompareExchange(a->running, 1, 1))
        Sleep(1);
    return 0;
}

static int
_thread_create(thread_handle_t* out, worker_arg_t* arg) {
    *out = CreateThread(NULL, 0, _worker, (LPVOID)arg, 0, NULL);
    return *out ? 0 : -1;
}

static void
_thread_join(thread_handle_t h) {
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}

#else /* POSIX */

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

typedef pthread_t thread_handle_t;

typedef struct {
    atomic_int* running;
    int         index;
} worker_arg_t;

// Linux caps thread names at 15 characters (TASK_COMM_LEN - 1).
// macOS allows up to 63.  Use the stricter limit everywhere so the name
// is identical on all platforms and never silently truncated by the kernel.
#define THREAD_NAME_MAX 15

static void*
_worker(void* raw) {
    worker_arg_t* a = (worker_arg_t*)raw;
    char          name[THREAD_NAME_MAX + 1];
    snprintf(name, sizeof(name), "Native-%d", a->index);
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
    struct timespec ts = {0, 500000}; // 0.5 ms
    while (atomic_load(a->running))
        nanosleep(&ts, NULL);
    return NULL;
}

static int
_thread_create(thread_handle_t* out, worker_arg_t* arg) {
    return pthread_create(out, NULL, _worker, arg);
}

static void
_thread_join(thread_handle_t h) {
    pthread_join(h, NULL);
}

#endif /* _WIN32 */

// ---- Thread group ----------------------------------------------------------

typedef struct {
    int              n;
    thread_handle_t* handles;
    worker_arg_t*    args;
#ifdef _WIN32
    volatile LONG running;
#else
    atomic_int running;
#endif
} thread_group_t;

static void
_thread_group_stop_and_free(thread_group_t* g) {
    if (!g)
        return;
#ifdef _WIN32
    InterlockedExchange(&g->running, 0);
#else
    atomic_store(&g->running, 0);
#endif
    for (int i = 0; i < g->n; i++)
        _thread_join(g->handles[i]);
    PyMem_Free(g->args);
    PyMem_Free(g->handles);
    PyMem_Free(g);
}

// Sentinel stored in a capsule after join_threads() has consumed it.
// PyCapsule_SetPointer rejects NULL, so we use a static address instead.
static char _joined;

// PyCapsule destructor — called when the token is garbage collected without
// an explicit join_threads() call.
static void
_capsule_destructor(PyObject* cap) {
    void* ptr = PyCapsule_GetPointer(cap, "native_ext.thread_group");
    if (!ptr || ptr == &_joined)
        return;
    _thread_group_stop_and_free((thread_group_t*)ptr);
}

// ---- Python API ------------------------------------------------------------

static PyObject*
start_threads(PyObject* self, PyObject* args) {
    (void)self;
    int n = 1;
    if (!PyArg_ParseTuple(args, "i", &n))
        return NULL;
    if (n < 1) {
        PyErr_SetString(PyExc_ValueError, "n must be >= 1");
        return NULL;
    }

    thread_group_t* g = (thread_group_t*)PyMem_Calloc(1, sizeof(thread_group_t));
    if (!g)
        return PyErr_NoMemory();

    g->handles = (thread_handle_t*)PyMem_Calloc(n, sizeof(thread_handle_t));
    g->args    = (worker_arg_t*)PyMem_Calloc(n, sizeof(worker_arg_t));
    if (!g->handles || !g->args) {
        PyMem_Free(g->handles);
        PyMem_Free(g->args);
        PyMem_Free(g);
        return PyErr_NoMemory();
    }

#ifdef _WIN32
    InterlockedExchange(&g->running, 1);
#else
    atomic_store(&g->running, 1);
#endif

    for (int i = 0; i < n; i++) {
        g->args[i].running = &g->running;
        g->args[i].index   = i;
        if (_thread_create(&g->handles[i], &g->args[i]) != 0) {
            // Signal already-started threads to stop before returning an error.
            g->n = i;
            _thread_group_stop_and_free(g);
            PyErr_SetString(PyExc_RuntimeError, "failed to create thread");
            return NULL;
        }
    }
    g->n = n;

    return PyCapsule_New(g, "native_ext.thread_group", _capsule_destructor);
}

static PyObject*
join_threads(PyObject* self, PyObject* args) {
    (void)self;
    PyObject* cap;
    if (!PyArg_ParseTuple(args, "O", &cap))
        return NULL;

    void* ptr = PyCapsule_GetPointer(cap, "native_ext.thread_group");
    if (!ptr)
        return NULL; // PyCapsule_GetPointer sets TypeError on mismatch
    if (ptr == &_joined) {
        PyErr_SetString(PyExc_RuntimeError, "join_threads: token already consumed");
        return NULL;
    }

    _thread_group_stop_and_free((thread_group_t*)ptr);

    // Mark capsule consumed so a second join_threads() call or the GC
    // destructor does not attempt to free already-freed memory.
    if (PyCapsule_SetPointer(cap, &_joined) < 0)
        return NULL;

    Py_RETURN_NONE;
}

// ---- Module definition -----------------------------------------------------

static PyMethodDef _methods[] = {
    {"start_threads", start_threads, METH_VARARGS,
     "start_threads(n) -> token\n\n"
     "Spawn n OS threads named 'Native-<i>' with no PyThreadState.\n"
     "Returns an opaque token to pass to join_threads()."                  },
    {"join_threads",  join_threads,  METH_VARARGS,
     "join_threads(token)\n\n"
     "Signal all threads in the group to stop and wait for them to finish."},
    {NULL,            NULL,          0,            NULL                    },
};

static struct PyModuleDef _module = {
    PyModuleDef_HEAD_INIT, "native_ext", NULL, -1, _methods,
};

PyMODINIT_FUNC
PyInit_native_ext(void) {
    return PyModule_Create(&_module);
}
