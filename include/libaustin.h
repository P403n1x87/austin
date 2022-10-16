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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <stdint.h>


/**
 * Austin stack callback.
 * 
 * This function is called back once the unwinding of thread stack has been
 * completed, and frames are ready to be retrieved with austin_pop_frame().
 * The callback is called with the following arguments:
 * 
 * @param  pid_t  the PID of the process the thread belongs to.
 * @param  pid_t  the TID of the thread.
*/
typedef void (*austin_callback_t)(pid_t, pid_t);


/**
 * Austin process handle.
 * 
 * This is a handle to an attached process that is generally required to perfom
 * unwinding operations.
*/
typedef struct _py_proc_t * austin_handle_t;


/**
 * The Austin frame structure.
 * 
 * This structure is used to return frame stack information to the user.
*/
typedef struct {
  uintptr_t      key;         // a key that uniquely identifies the frame.
  char         * filename;    // the file name of the source containing the code.
  char         * scope;       // the name of the scope, e.g. the function name
  unsigned int   line;        // the line number.
  unsigned int   line_end;    // the end line number.
  unsigned int   column;      // the column number.
  unsigned int   column_end;  // the end column number.
} austin_frame_t;


/**
 * Initialise the Austin library.
 * 
 * This function must be called before any other function in the library.
*/
extern int
austin_up();


/**
 * Finalise the Austin library.
 * 
 * This function should be called once the Austin library is no longer needed,
 * to free up resources.
*/
extern void
austin_down();


/**
 * Attach to a Python process.
 * 
 * This function tries to attach to a running Python process, identified by its
 * PID. If the process exists and is a valid Python process, a non-NULL handle
 * is returned to the caller, to be used for further operations.
 * 
 * Note that attaching to a Python process is a lightweight operation that does
 * not interfere with the execution of the process in any way.
 * 
 * @param  pid_t  The PID of the process to attach to.
 * 
 * @return a handle to the process, or NULL if the process is not a valid
 *         Python process.
*/
extern austin_handle_t
austin_attach(pid_t);


/**
 * Detach from a Python process.
 * 
 * This function detaches from a Python process, identified by its handle.
*/
extern void
austin_detach(austin_handle_t);


/**
 * Sample an attached Python process.
 * 
 * This function samples the call stack of all the threads within the attached
 * process. The passed callback function is called after every thread has been
 * sampled. Frames are available to be retrieved with austin_pop_frame().
 * 
 * @param  austin_handle_t    the handle to the process to sample.
 * @param  austin_callback_t  the callback function to call after sampling.
 * 
 * @return 0 if the sampling was successful.
*/
extern int
austin_sample(austin_handle_t, austin_callback_t);


/**
 * Sample a single thread.
 * 
 * This function samples the call stack of a single thread within the attached
 * process.
 * 
 * @param  austin_handle_t  the handle to the process to sample.
 * @param  pid_t            the TID of the thread to sample.
 * 
 * @return 0 if the sampling was successful.
*/
extern int
austin_sample_thread(austin_handle_t, pid_t);


/**
 * Pop a frame from the stack.
 * 
 * This function pops a frame from the stack of the last sampled thread. This
 * function should be called iteratively until it returns NULL, to retrieve all
 * the frames in the stack.
 * 
 * @return a valid reference to a frame structure, or NULL otherwise.
*/
extern austin_frame_t *
austin_pop_frame();


/**
 * Read a single frame from the attached process.
 * 
 * This function reads a single frame from an attached process, at the given
 * remote memory location. This is useful if one is intercepting calls to, e.g.
 * _PyEval_EvalFrameDefault and has access to the frame pointer (the second
 * argument). This function can then be used to resolve the frame details.
 * 
 * @param  austin_handle_t  the handle to the process.
 * @param  void *           the remote memory location of the frame.
 * 
 * @return a valid reference to a frame structure, or NULL otherwise.
*/
extern austin_frame_t *
austin_read_frame(austin_handle_t, void *);


#ifdef __cplusplus
}
#endif
