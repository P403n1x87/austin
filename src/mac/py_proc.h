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

#ifdef PY_PROC_C

#include <libproc.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/machine.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "../hints.h"
#include "../mem.h"
#include "../resources.h"

#define next_lc(cmd)    (cmd = (struct segment_command*)((void*)cmd + cmd->cmdsize));
#define next_lc_64(cmd) (cmd = (struct segment_command_64*)((void*)cmd + cmd->cmdsize));

// ---- Endianness ----
#define ENDIAN(f, v) (f ? __bswap_32(v) : v)

#define bswap_16(value) ((((value) & 0xff) << 8) | ((value) >> 8))

#define bswap_32(value)                                                                                        \
    (((uint32_t)bswap_16((uint16_t)((value) & 0xffff)) << 16) | (uint32_t)bswap_16((uint16_t)((value) >> 16)))

#define sw32(f, v) (f ? bswap_32(v) : v)

struct _proc_extra_info {
    // void
};

// ----------------------------------------------------------------------------
// NOTE: This is inspired by glibc/posix/execvpe.c
extern char** environ;

static void
_maybe_script_execute(const char* file, char* const argv[]) {
    ptrdiff_t argc = 0;
    for (ptrdiff_t argc = 0; argv[argc] != NULL; argc++)
        ;

    char* new_argv[argc > 1 ? 2 + argc : 3];
    new_argv[0] = (char*)"/bin/sh";
    new_argv[1] = (char*)file;
    if (argc > 1)
        memcpy(new_argv + 2, argv + 1, argc * sizeof(char*));
    else
        new_argv[2] = NULL;
    /* Execute the shell.  */
    execve(new_argv[0], new_argv, environ);
}

#define execvpe(file, argv, environ)       \
    {                                      \
        execvp(file, argv);                \
        _maybe_script_execute(file, argv); \
    }

// ----------------------------------------------------------------------------
static bin_attr_t
_py_proc__analyze_macho64(py_proc_t* self, void* base, void* map) {
    bin_attr_t bin_attrs = 0;
    int64_t    offset    = 0;

    struct mach_header_64* hdr = (struct mach_header_64*)map;

    switch (hdr->filetype) {
    case MH_EXECUTE:
        bin_attrs |= BT_EXEC;
        break;
    case MH_DYLIB:
        bin_attrs |= BT_LIB;
        break;
    default:
        return 0;
    }

    int ncmds = hdr->ncmds;
    int s     = (hdr->magic == MH_CIGAM_64);

    int                        cmd_cnt = 0;
    struct segment_command_64* cmd     = map + sizeof(struct mach_header_64);

    for (register int i = 0; cmd_cnt < 2 && i < ncmds; i++) {
        switch (cmd->cmd) {
        case LC_SEGMENT_64:
            if (strcmp(cmd->segname, "__TEXT") == 0) {
                offset = (int64_t)base - cmd->vmaddr;
            } else if (strcmp(cmd->segname, "__DATA") == 0) {
                int                nsects = cmd->nsects;
                struct section_64* sec    = (struct section_64*)((void*)cmd + sizeof(struct segment_command_64));
                self->map.bss.size        = 0;
                for (register int j = 0; j < nsects; j++) {
                    if (strcmp(sec[j].sectname, "__bss") == 0) {
                        self->map.bss.base  = (void*)(offset + sec[j].addr);
                        self->map.bss.size  = sec[j].size;
                        bin_attrs          |= B_BSS;
                        continue;
                    }
                    // This section was added in Python 3.11
                    if (strcmp(sec[j].sectname, "PyRuntime") == 0) {
                        self->map.runtime.base = (void*)(offset + sec[j].addr);
                        self->map.runtime.size = sec[j].size;
                        continue;
                    }
                }
                cmd_cnt++;
            }
            break;

        case LC_SYMTAB:
            for (register int i = 0;
                 self->sym_loaded < DYNSYM_COUNT && i < sw32(s, ((struct symtab_command*)cmd)->nsyms); i++) {
                struct nlist_64* sym_tab = (struct nlist_64*)(map + sw32(s, ((struct symtab_command*)cmd)->symoff));
                void*            str_tab = (void*)(map + sw32(s, ((struct symtab_command*)cmd)->stroff));

                if ((sym_tab[i].n_type & N_EXT) == 0)
                    continue;

                char* sym_name = (char*)(str_tab + sym_tab[i].n_un.n_strx);
                if (_py_proc__check_sym(self, sym_name, (void*)(offset + sym_tab[i].n_value))) {
                    self->sym_loaded++;
                }
            }
            cmd_cnt++;
        } // switch

        next_lc_64(cmd);
    }

    if (self->sym_loaded > 0)
        bin_attrs |= B_SYMBOLS;

    return bin_attrs;
} // _py_proc__analyze_macho64

// ----------------------------------------------------------------------------
static bin_attr_t
_py_proc__analyze_macho32(py_proc_t* self, void* base, void* map) {
    bin_attr_t bin_attrs = 0;
    int64_t    offset    = 0;

    struct mach_header* hdr = (struct mach_header*)map;

    switch (hdr->filetype) {
    case MH_EXECUTE:
        bin_attrs |= BT_EXEC;
        break;
    case MH_DYLIB:
        bin_attrs |= BT_LIB;
        break;
    default:
        return 0;
    }

    int ncmds = hdr->ncmds;
    int s     = (hdr->magic == MH_CIGAM);

    int                     cmd_cnt = 0;
    struct segment_command* cmd     = map + sizeof(struct mach_header);

    for (register int i = 0; cmd_cnt < 2 && i < ncmds; i++) {
        switch (cmd->cmd) {
        case LC_SEGMENT:
            if (strcmp(cmd->segname, "__TEXT") == 0) {
                offset = (int64_t)base - cmd->vmaddr;
            } else if (strcmp(cmd->segname, "__DATA") == 0) {
                int             nsects = cmd->nsects;
                struct section* sec    = (struct section*)((void*)cmd + sizeof(struct segment_command));
                self->map.bss.size     = 0;
                for (register int j = 0; j < nsects; j++) {
                    if (strcmp(sec[j].sectname, "__bss") == 0) {
                        self->map.bss.base  = (void*)(offset + sec[j].addr);
                        self->map.bss.size  = sec[j].size;
                        bin_attrs          |= B_BSS;
                        continue;
                    }
                    // This section was added in Python 3.11
                    if (strcmp(sec[j].sectname, "PyRuntime") == 0) {
                        self->map.runtime.base = (void*)(offset + sec[j].addr);
                        self->map.runtime.size = sec[j].size;
                        continue;
                    }
                }
                cmd_cnt++;
            }
            break;

        case LC_SYMTAB:
            for (register int i = 0;
                 self->sym_loaded < DYNSYM_COUNT && i < sw32(s, ((struct symtab_command*)cmd)->nsyms); i++) {
                struct nlist* sym_tab = (struct nlist*)(map + sw32(s, ((struct symtab_command*)cmd)->symoff));
                void*         str_tab = (void*)(map + sw32(s, ((struct symtab_command*)cmd)->stroff));

                if ((sym_tab[i].n_type & N_EXT) == 0)
                    continue;

                char* sym_name = (char*)(str_tab + sym_tab[i].n_un.n_strx);
                if (_py_proc__check_sym(self, sym_name, (void*)(offset + sym_tab[i].n_value))) {
                    self->sym_loaded++;
                }
            }
            cmd_cnt++;
        } // switch

        next_lc(cmd);
    }

    if (self->sym_loaded)
        bin_attrs |= B_SYMBOLS;

    return bin_attrs;
} // _py_proc__analyze_macho32

// ----------------------------------------------------------------------------
static bin_attr_t
_py_proc__analyze_fat(py_proc_t* self, void* base, void* map) {
    cpu_type_t cpu;
    int        is_abi64;
    size_t     cpu_size = sizeof(cpu), abi64_size = sizeof(is_abi64);

    sysctlbyname("hw.cputype", &cpu, &cpu_size, NULL, 0);
    sysctlbyname("hw.cpu64bit_capable", &is_abi64, &abi64_size, NULL, 0);

    cpu |= is_abi64 * CPU_ARCH_ABI64;

    // Look up corresponding part from universal binary
    struct fat_header* fat_hdr = (struct fat_header*)map;

    int              fs   = fat_hdr->magic == FAT_CIGAM;
    struct fat_arch* arch = (struct fat_arch*)(map + sizeof(struct fat_header));

    uint32_t narchs = sw32(fs, fat_hdr->nfat_arch);
    log_d("Found %d architectures in fat binary", narchs);

    for (register int i = 0; i < narchs; i++) {
        log_d("- architecture %x (expected %x)", sw32(fs, arch[i].cputype), cpu);

        if (sw32(fs, arch[i].cputype) == cpu) {
            struct mach_header_64* hdr = (struct mach_header_64*)(map + sw32(fs, arch[i].offset));

            switch (hdr->magic) {
            case MH_MAGIC:
            case MH_CIGAM:
                log_d("Using the 32-bit image of the fat binary");
                return _py_proc__analyze_macho32(self, base, (void*)hdr);

            case MH_MAGIC_64:
            case MH_CIGAM_64:
                log_d("Using the 64-bit image of the fat binary");
                return _py_proc__analyze_macho64(self, base, (void*)hdr);

            default:
                log_e("Unexpected Mach-O magic");
                return INVALID_ATTR;
            }

            break;
        }
    }

    log_e("Fat binary has no matching architectures");

    return INVALID_ATTR;
}

// ----------------------------------------------------------------------------
static bin_attr_t
_py_proc__analyze_macho(py_proc_t* self, char* path, void* base, mach_vm_size_t size) {
    cu_fd fd = open(path, O_RDONLY);
    if (fd == -1) {
        log_e("Cannot open binary %s", path);
        return INVALID_ATTR;
    }

    log_d("Analysing binary %s with base %p", path, base);

    // This would cause problem if allocated in the stack frame
    cu_void*     fs_buffer = malloc(sizeof(struct stat));
    struct stat* fs        = (struct stat*)fs_buffer;
    cu_map_t*    map       = NULL;
    if (fstat(fd, fs) == -1) { // Get file size
        set_error(IO, "Cannot determine size of binary file");
        FAIL; // cppcheck-suppress [memleak]
    }

    map = map_new(fd, fs->st_size, MAP_SHARED);
    if (!isvalid(map)) {
        FAIL; // cppcheck-suppress [memleak]
    }

    void* map_addr = map->addr;
    log_t("Local Mach-O file mapping %p-%p\n", map_addr, map_addr + size);

    struct mach_header_64* hdr = (struct mach_header_64*)map_addr;
    switch (hdr->magic) {
    case MH_MAGIC:
    case MH_CIGAM:
        log_d("Binary is Mach-O 32");
        return _py_proc__analyze_macho32(self, base, map_addr);

    case MH_MAGIC_64:
    case MH_CIGAM_64:
        log_d("Binary is Mach-O 64");
        return _py_proc__analyze_macho64(self, base, map_addr);

    case FAT_MAGIC:
    case FAT_CIGAM:
        log_d("Binary is fat");
        return _py_proc__analyze_fat(self, base, map_addr);

    default:
        self->sym_loaded = false;
    }

    return 0;
} // _py_proc__analyze_macho

// ----------------------------------------------------------------------------
static int
check_pid(pid_t pid) {
    // The best way of checking would probably be with a call to proc_find, but
    // this seems to require the Kernel framework.
    struct proc_bsdinfo proc;
    proc.pbi_status = SIDL;

    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &proc, PROC_PIDTBSDINFO_SIZE) == -1) {
        set_error(OS, "Cannot retrieve process information");
        FAIL;
    }

    log_t("check_pid :: %d", proc.pbi_status);

    if (proc.pbi_status == SIDL || proc.pbi_status == 32767) {
        set_error(OS, "Process has unexpected status");
        FAIL;
    }

    SUCCESS;
}

// ----------------------------------------------------------------------------
static mach_port_t
pid_to_task(pid_t pid) {
    mach_port_t   task;
    kern_return_t result;

    if (fail(check_pid(pid))) {
        log_d("Process ID %d is not valid", pid);
        return 0;
    }

    result = task_for_pid(mach_task_self(), pid, &task);
    if (result != KERN_SUCCESS) {
        set_error(PERM, "Cannot obtain task for PID");
        log_location();
        return 0;
    }
    return task;
}

// ----------------------------------------------------------------------------
static int
_py_proc__get_maps(py_proc_t* self) {
    mach_vm_address_t              address = 0;
    mach_vm_size_t                 size    = 0;
    mach_msg_type_number_t         count   = sizeof(vm_region_basic_info_data_64_t);
    vm_region_basic_info_data_64_t region_info;
    mach_port_t                    object_name;

    // NOTE: Mac OS X kernel bug. This also gives time to the VM maps to
    // stabilise.
    usleep(50000); // 50ms

    cu_char* prev_path   = NULL;
    cu_char* needle_path = NULL;
    cu_char* path        = (char*)calloc(MAXPATHLEN + 1, sizeof(char));
    if (!isvalid(path)) {
        set_error(MALLOC, "Cannot allocate memory for map path");
        FAIL;
    }

    sfree(self->bin_path);
    sfree(self->lib_path);

    cu_void* pd_mem = calloc(1, sizeof(struct proc_desc));
    if (!isvalid(pd_mem)) {
        set_error(MALLOC, "Cannot allocate memory for proc_desc");
        FAIL; // cppcheck-suppress [memleak]
    }
    struct proc_desc* pd = pd_mem;

    if (proc_pidpath(self->pid, pd->exe_path, sizeof(pd->exe_path)) <= 0) {
        log_w("Cannot get executable path for process %d: %s", self->pid, strerror(errno));
    }
    if (strlen(pd->exe_path) == 0) {
        set_error(OS, "Invalid process executable path");
        FAIL; // cppcheck-suppress [memleak]
    }
    log_d("Executable path: '%s'", pd->exe_path);

    self->ref = pid_to_task(self->pid);
    if (self->ref == 0) {
        FAIL; // cppcheck-suppress [memleak]
    }

    struct vm_map* map = NULL;

    while (mach_vm_region(
               self->ref, &address, &size, VM_REGION_BASIC_INFO_64,
               (vm_region_info_t)&region_info, // cppcheck-suppress [uninitvar]
               &count, &object_name
           )
           == KERN_SUCCESS) {
        int path_len = proc_regionfilename(self->pid, address, path, MAXPATHLEN);

        // We assume that the first segment is the executable part. Under this
        // assumption, the address of this map will be the base address.
        if (!(region_info.protection & VM_PROT_EXECUTE)) {
            address += size;
            continue;
        }

        if (isvalid(prev_path) && strcmp(path, prev_path) == 0) { // Avoid analysing a binary multiple times
            goto next;
        }

        sfree(prev_path);
        prev_path = strndup(path, path_len);
        if (!isvalid(prev_path)) {
            set_error(MALLOC, "Cannot duplicate path name");
            FAIL;
        }

        // The first memory map of the shared library (if any)
        char* needle = strstr(path, "ython");

        // The first memory map of the executable
        if (!isvalid(pd->maps[MAP_BIN].path) && strcmp(pd->exe_path, path) == 0) {
            map       = &(pd->maps[MAP_BIN]);
            map->path = strndup(path, strlen(path));
            if (!isvalid(map->path)) {
                set_error(MALLOC, "Cannot duplicate executable path name");
                FAIL;
            }

            bin_attr_t bin_attrs = _py_proc__analyze_macho(self, path, (void*)address, size);

            map->file_size   = size;
            map->base        = (void*)address;
            map->size        = size;
            map->has_symbols = !!(bin_attrs & B_SYMBOLS);
            if (map->has_symbols && bin_attrs & B_BSS) {
                map->bss_base = self->map.bss.base;
                map->bss_size = self->map.bss.size;
            }

            log_d("Binary map: %s (symbols %d)", map->path, map->has_symbols);
        }

        else if (!isvalid(pd->maps[MAP_LIBSYM].path) && isvalid(needle)) {
            bin_attr_t bin_attrs = _py_proc__analyze_macho(self, path, (void*)address, size);
            if (bin_attrs & B_SYMBOLS) {
                map       = &(pd->maps[MAP_LIBSYM]);
                map->path = strndup(path, strlen(path));
                if (!isvalid(map->path)) {
                    set_error(MALLOC, "Cannot duplicate library path name");
                    FAIL;
                }
                map->file_size   = size;
                map->base        = (void*)address;
                map->size        = size;
                map->has_symbols = true;
                map->bss_base    = self->map.bss.base;
                map->bss_size    = self->map.bss.size;

                log_d("Library map: %s (with symbols)", map->path);
            }

            // The first memory map of a binary that contains "ythonX.Y" or "ython"
            // and "/X.Y"in its name.
            else if (!isvalid(pd->maps[MAP_LIBNEEDLE].path)) {
                if (isvalid(needle)) {
                    unsigned int v;
                    if (sscanf(needle, "ython%u.%u", &v, &v) == 2
                        || (isvalid(strstr(path, "/3.")) && sscanf(strstr(path, "/3."), "/3.%u", &v) == 1)
                        || (isvalid(strstr(path, "/2.")) && sscanf(strstr(path, "/2."), "/2.%u", &v) == 1)) {
                        map       = &(pd->maps[MAP_LIBNEEDLE]);
                        map->path = needle_path = strndup(path, strlen(path));
                        if (!isvalid(map->path)) {
                            set_error(MALLOC, "Cannot duplicate library (needle) path name");
                            FAIL;
                        }
                        map->file_size   = size;
                        map->base        = (void*)address;
                        map->size        = size;
                        map->has_symbols = false;
                        log_d("Library map: %s (needle)", map->path);
                    }
                }
            }
        }

    next:
        address += size;
    }

    log_d("BSS bounds  [%p - %p]", self->map.bss.base, self->map.bss.base + self->map.bss.size);

    // If the library map is not valid, use the needle map
    if (!isvalid(pd->maps[MAP_LIBSYM].path)) {
        pd->maps[MAP_LIBSYM]         = pd->maps[MAP_LIBNEEDLE];
        pd->maps[MAP_LIBNEEDLE].path = needle_path = NULL;
    }

    // Work out paths
    self->bin_path = pd->maps[MAP_BIN].path;
    self->lib_path = pd->maps[MAP_LIBSYM].path;

    // Work out binary map
    for (int i = 0; i < MAP_COUNT; i++) {
        map = &(pd->maps[i]);
        if (map->has_symbols) {
            self->map.exe.base = map->base;
            self->map.exe.size = map->size;
            self->sym_loaded   = true;
            break;
        }
    }

    // Work out BSS map
    int map_index      = isvalid(pd->maps[MAP_LIBSYM].path) ? MAP_LIBSYM : MAP_BIN;
    self->map.bss.base = pd->maps[map_index].bss_base;
    self->map.bss.size = pd->maps[map_index].bss_size;

    if (!self->sym_loaded) {
        set_error(BINARY, "Cannot detect symbols in library");
        FAIL;
    }

    SUCCESS;
} // _py_proc__get_maps

// ----------------------------------------------------------------------------
static ssize_t
_py_proc__get_resident_memory(py_proc_t* self) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;

    return task_info(
               self->ref, MACH_TASK_BASIC_INFO, (task_info_t)&info, &count // cppcheck-suppress [uninitvar]
           ) == KERN_SUCCESS
             ? info.resident_size
             : -1;
} // _py_proc__get_resident_memory

// ----------------------------------------------------------------------------
static int
_py_proc__init(py_proc_t* self) {
    log_t("macOS: py_proc init");
    if (!isvalid(self)) {
        set_error(NULL, "Invalid process structure");
        FAIL;
    }

    return _py_proc__get_maps(self);
} // _py_proc__init

#endif
