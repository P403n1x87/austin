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

#include <psapi.h>
#include <string.h>
#include <tlhelp32.h>

#include "../py_proc.h"
#include "../resources.h"


#define MODULE_CNT                     2
#define SYMBOLS                        2


struct _proc_extra_info {
  HANDLE h_reader_thread;
};


// ----------------------------------------------------------------------------
// TODO: Optimise by avoiding executing the same code over and over again
static void *
map_addr_from_rva(void * bin, DWORD rva) {
  IMAGE_DOS_HEADER     * dos_hdr = (IMAGE_DOS_HEADER *) bin;
  IMAGE_NT_HEADERS     * nt_hdr  = (IMAGE_NT_HEADERS *) (bin + dos_hdr->e_lfanew);
  IMAGE_SECTION_HEADER * s_hdr   = (IMAGE_SECTION_HEADER *) (bin + dos_hdr->e_lfanew + sizeof(IMAGE_NT_HEADERS));

  for (register int i = 0; i < nt_hdr->FileHeader.NumberOfSections; i++) {
    if (rva >= s_hdr[i].VirtualAddress && rva < s_hdr[i].VirtualAddress + s_hdr[i].SizeOfRawData)
      return bin + s_hdr[i].PointerToRawData + (rva - s_hdr[i].VirtualAddress);
  }

  return NULL;
}


// ----------------------------------------------------------------------------
// Helper to compare file paths that might differ in e.g. the case. Here we use
// the file information to reliably determine if two paths refer to the same
// file.
static int
pathcmp(char * a, char * b) {
  cu_HANDLE ha = CreateFile(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  cu_HANDLE hb = CreateFile(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

  BY_HANDLE_FILE_INFORMATION ia, ib;
  GetFileInformationByHandle(ha, &ia);
  GetFileInformationByHandle(hb, &ib);

  return !(
    ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber
    && ia.nFileIndexLow     == ib.nFileIndexLow
    && ia.nFileIndexHigh    == ib.nFileIndexHigh
  );
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_pe(py_proc_t * self, char * path, void * base) {
  cu_HANDLE hFile    = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  cu_HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, 0);
  cu_VOF    pMapping = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);

  IMAGE_DOS_HEADER     * dos_hdr = (IMAGE_DOS_HEADER *)     pMapping;
  IMAGE_NT_HEADERS     * nt_hdr  = (IMAGE_NT_HEADERS *)     (pMapping + dos_hdr->e_lfanew);
  IMAGE_SECTION_HEADER * s_hdr   = (IMAGE_SECTION_HEADER *) (pMapping + dos_hdr->e_lfanew + sizeof(IMAGE_NT_HEADERS));

  if (nt_hdr->Signature != IMAGE_NT_SIGNATURE) {
    set_error(EPROC);
    FAIL;
  }

  // ---- Find the .data section ----
  for (register int i = 0; i < nt_hdr->FileHeader.NumberOfSections; i++) {
    if (strcmp(".data", (const char *) s_hdr[i].Name) == 0) {
      self->map.bss.base = base + s_hdr[i].VirtualAddress;
      self->map.bss.size = s_hdr[i].Misc.VirtualSize;
      break;
    }
  }

  // ---- Search for exports ----
  self->sym_loaded = 0;

  IMAGE_EXPORT_DIRECTORY * e_dir = (IMAGE_EXPORT_DIRECTORY *) map_addr_from_rva(
    pMapping, nt_hdr->OptionalHeader.DataDirectory[0].VirtualAddress
  );
  if (e_dir != NULL) {
    DWORD * names   = (DWORD *) map_addr_from_rva(pMapping, e_dir->AddressOfNames);
    WORD  * idx_tab = (WORD *)  map_addr_from_rva(pMapping, e_dir->AddressOfNameOrdinals);
    DWORD * addrs   = (DWORD *) map_addr_from_rva(pMapping, e_dir->AddressOfFunctions);
    for (
      register int i = 0;
      self->sym_loaded < SYMBOLS && i < e_dir->NumberOfNames;
      i++
    ) {
      char * sym_name = (char *) map_addr_from_rva(pMapping, names[i]);
      self->sym_loaded += _py_proc__check_sym(self, sym_name, addrs[idx_tab[i]] + base);
    }
  }

  if (!self->sym_loaded) {
    set_error(EPROC);
    FAIL;
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
// Forward declaration.
static int
_py_proc__run(py_proc_t *);


// On Windows, if we fail with the parent process we look if it has a single
// child and try to attach to that instead. We keep going until we either find
// a single Python process or more or less than a single child.
static int
_py_proc__try_child_proc(py_proc_t * self) {
  cu_HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (h == INVALID_HANDLE_VALUE) {
    log_e("Cannot inspect processes details");
    set_error(EPROC);
    FAIL;
  }

  HANDLE orig_hproc = self->proc_ref;
  pid_t  orig_pid   = self->pid;

  for(;;) {
    pid_t parent_pid = self->pid;

    PROCESSENTRY32 pe = { 0 };
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(h, &pe)) {
      pid_t child_pid = 0;
      do {
        if (pe.th32ParentProcessID == parent_pid) {
          if (child_pid) {
            log_d("Process has more than one child");
            goto rollback;
          }
          child_pid = pe.th32ProcessID;
        }
      } while (Process32Next(h, &pe));

      if (!child_pid) {
        log_d("Process has no children");
        goto rollback;
      }

      self->pid = child_pid;
      self->proc_ref = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, child_pid
      );
      if (self->proc_ref == INVALID_HANDLE_VALUE) {
        log_e("Cannot open child process handle");
        goto rollback;
      }
      if (success(_py_proc__run(self))) {
        log_d("Process has a single Python child with PID %d. We will attach to that", child_pid);
        SUCCESS;
      }
      else {
        log_d("Process has a single non-Python child with PID %d. Taking it as new parent", child_pid);
        CloseHandle(self->proc_ref);
      }
    }
  }

rollback:
  self->pid      = orig_pid;
  self->proc_ref = orig_hproc;
  
  set_error(EPROC);
  FAIL;
}


// ----------------------------------------------------------------------------
static int
_py_proc__get_modules(py_proc_t * self) {
  cu_HANDLE mod_hdl;
  mod_hdl = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, self->pid);
  if (mod_hdl == INVALID_HANDLE_VALUE) {
    set_error(EPROC);
    FAIL;
  }

  MODULEENTRY32 module;
  module.dwSize = sizeof(module);

  self->min_raddr = (void *) -1;
  self->max_raddr = NULL;

  sfree(self->bin_path);
  sfree(self->lib_path);

  cu_void * pd_mem = calloc(1, sizeof(struct proc_desc));
  if (!isvalid(pd_mem)) {
    log_ie("Cannot allocate memory for proc_desc");
    set_error(EPROC);
    FAIL;
  }
  struct proc_desc * pd          = pd_mem;
  cu_char          * prev_path   = NULL;
  cu_char          * needle_path = NULL;
  struct vm_map    * map         = NULL;

  if (GetModuleFileNameEx(self->proc_ref, NULL, pd->exe_path, sizeof(pd->exe_path)) == 0) {
    log_ie("Cannot get executable path");
    set_error(EPROC);
    FAIL;
  }
  log_d("Executable path: %s", pd->exe_path);

  if (!Module32First(mod_hdl, &module)) {
    log_ie("Cannot get first module");
    set_error(EPROC);
    FAIL;
  }
  do {
    if ((void *) module.modBaseAddr < self->min_raddr)
      self->min_raddr = module.modBaseAddr;

    if ((void *) module.modBaseAddr + module.modBaseSize > self->max_raddr)
      self->max_raddr = module.modBaseAddr + module.modBaseSize;

    if (isvalid(prev_path) && strcmp(module.szExePath, prev_path) == 0) { // Avoid analysing a binary multiple times
      continue;
    }
    
    sfree(prev_path);
    prev_path = strdup(module.szExePath);
    if (!isvalid(prev_path)) {
      log_ie("Cannot duplicate path name");
      set_error(EPROC);
      FAIL;
    }

    log_t(
      "%p-%p:  Module %s",
      module.modBaseAddr, module.modBaseAddr + module.modBaseSize,
      module.szExePath
    );

    // The first memory map of the executable
    if (!isvalid(pd->maps[MAP_BIN].path) && pathcmp(pd->exe_path, module.szExePath) == 0) {
      map = &(pd->maps[MAP_BIN]);
      map->path = strdup(module.szExePath);
      if (!isvalid(map->path)) {
        log_ie("Cannot duplicate path name");
        set_error(EPROC);
        FAIL;
      }
      map->file_size = module.modBaseSize;
      map->base = (void *) module.modBaseAddr;
      map->size = module.modBaseSize;
      map->has_symbols = success(_py_proc__analyze_pe(self, module.szExePath, (void *) module.modBaseAddr));
      if (map->has_symbols) {
        map->bss_base = self->map.bss.base;
        map->bss_size = self->map.bss.size;
      }
      log_d("Binary map: %s (symbols %d)", map->path, map->has_symbols);
      continue;
    }

    // The first memory map of the shared library (if any)
    char * needle = strstr(module.szExePath, "python");
    if (!isvalid(pd->maps[MAP_LIBSYM].path) && isvalid(needle)) {
      int has_symbols = success(_py_proc__analyze_pe(self, module.szExePath, (void *) module.modBaseAddr));
      if (has_symbols) {
        map = &(pd->maps[MAP_LIBSYM]);
        map->path = strdup(module.szExePath);
        if (!isvalid(map->path)) {
          log_ie("Cannot duplicate path name");
          set_error(EPROC);
          FAIL;
        }
        map->file_size = module.modBaseSize;
        map->base = (void *) module.modBaseAddr;
        map->size = module.modBaseSize;
        map->has_symbols = TRUE;
        map->bss_base = self->map.bss.base;
        map->bss_size = self->map.bss.size;

        log_d("Library map: %s (with symbols)", map->path);

        continue;
      }
      
      // The first memory map of a binary that contains "pythonX.Y" in its name
      if (!isvalid(pd->maps[MAP_LIBNEEDLE].path)) {
        if (isvalid(needle)) {
          unsigned int v;
          if (sscanf(needle, "python%u.%u", &v, &v) == 2) {
            map = &(pd->maps[MAP_LIBNEEDLE]);
            map->path = needle_path = strdup(module.szExePath);
            if (!isvalid(map->path)) {
              log_ie("Cannot duplicate path name");
              set_error(EPROC);
              FAIL;
            }
            map->file_size = module.modBaseSize;
            map->base = (void *) module.modBaseAddr;
            map->size = module.modBaseSize;
            map->has_symbols = FALSE;
            log_d("Library map: %s (needle)", map->path);
            continue;
          }
        }
      }
    }
  } while (Module32Next(mod_hdl, &module));

  // If the library map is not valid, use the needle map
  if (!isvalid(pd->maps[MAP_LIBSYM].path)) {
    pd->maps[MAP_LIBSYM] = pd->maps[MAP_LIBNEEDLE];
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
      break;
    }
  }

  // Work out BSS map
  int map_index = isvalid(pd->maps[MAP_LIBSYM].path) ? MAP_LIBSYM : MAP_BIN;
  self->map.bss.base = pd->maps[map_index].bss_base;
  self->map.bss.size = pd->maps[map_index].bss_size;
  
  log_d("BSS map %d from %s @ %p", map_index, pd->maps[map_index].path, self->map.bss.base);
  log_d("VM maps parsing result: bin=%s lib=%s symbols=%d", self->bin_path, self->lib_path, self->sym_loaded);

  if (!self->sym_loaded) {
    set_error(EPROC);
    FAIL;
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
static ssize_t _py_proc__get_resident_memory(py_proc_t * self) {
  PROCESS_MEMORY_COUNTERS mem_info;

  return GetProcessMemoryInfo(self->proc_ref, &mem_info, sizeof(mem_info))
    ? mem_info.WorkingSetSize
    : -1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__init(py_proc_t * self) {
  if (!isvalid(self)) {
    set_error(EPROC);
    FAIL;
  }

  if (fail(_py_proc__get_modules(self))) {
    log_d("Process does not seem to be Python; look for single child process");
    return _py_proc__try_child_proc(self);
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
// The default stream buffer size should be 4KB, so this chunk size should be
// enough to avoid blocking while keeping the number of reads to a minimum.
#define STDOUT_CHUNK_SIZE (1 << 10)

DWORD WINAPI
reader_thread(LPVOID lpParam) {
  char buffer[STDOUT_CHUNK_SIZE];
  while (ReadFile(lpParam, &buffer, STDOUT_CHUNK_SIZE, NULL, NULL));
  return 0;
}

#endif
