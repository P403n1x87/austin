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

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_info.h>
#include <sys/param.h>

#include "../error.h"
#include "../hints.h"
#include "../logging.h"

// ----------------------------------------------------------------------------
// Find the Mach thread port for the thread whose pthread_t value equals
// pthread_id.  The caller owns the returned port (one send right).  All other
// ports obtained from task_threads() are released here.
// Returns MACH_PORT_NULL on failure.
static inline thread_act_t
_find_thread_port(mach_port_t task, uintptr_t pthread_id) {
    thread_act_array_t     threads;
    mach_msg_type_number_t thread_count;

    if (task_threads(task, &threads, &thread_count) != KERN_SUCCESS) {
        set_error(OS, "task_threads failed");
        return MACH_PORT_NULL;
    }

    thread_act_t result = MACH_PORT_NULL;
    for (mach_msg_type_number_t i = 0; i < thread_count; i++) {
        thread_identifier_info_data_t info  = {0};
        mach_msg_type_number_t        count = THREAD_IDENTIFIER_INFO_COUNT;

        if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS
            && info.thread_handle == (uint64_t)pthread_id) {
            result     = threads[i];     // keep this port
            threads[i] = MACH_PORT_NULL; // prevent deallocation below
        }
    }

    for (mach_msg_type_number_t i = 0; i < thread_count; i++) {
        if (threads[i] != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, thread_count * sizeof(thread_act_t));

    if (result == MACH_PORT_NULL) {
        log_d("mac: no Mach thread port found for pthread_t %p", (void*)pthread_id);
    }

    return result;
}
