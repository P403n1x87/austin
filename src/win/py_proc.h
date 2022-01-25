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
#include <tlhelp32.h>

#include "../py_proc.h"


#define CHECK_HEAP
#define DEREF_SYM


#define MODULE_CNT                     2
#define SYMBOLS                        2

#define PROC_REF                        ((long long int) self->extra->h_proc)


struct _proc_extra_info {
  HANDLE h_proc;
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
static int
_py_proc__analyze_pe(py_proc_t * self, char * path) {
  HANDLE hFile    = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, 0);
  LPVOID pMapping = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);

  IMAGE_DOS_HEADER     * dos_hdr = (IMAGE_DOS_HEADER *)     pMapping;
  IMAGE_NT_HEADERS     * nt_hdr  = (IMAGE_NT_HEADERS *)     (pMapping + dos_hdr->e_lfanew);
  IMAGE_SECTION_HEADER * s_hdr   = (IMAGE_SECTION_HEADER *) (pMapping + dos_hdr->e_lfanew + sizeof(IMAGE_NT_HEADERS));

  if (nt_hdr->Signature != IMAGE_NT_SIGNATURE)
    self->sym_loaded = 0;

  else {
    void * base = self->map.bss.base;

    // ---- Find the .data section ----
    for (register int i = 0; i < nt_hdr->FileHeader.NumberOfSections; i++) {
      if (strcmp(".data", (const char *) s_hdr[i].Name) == 0) {
        self->map.bss.base += s_hdr[i].VirtualAddress;
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
  }

  UnmapViewOfFile(pMapping);
  CloseHandle(hMapping);
  CloseHandle(hFile);

  return !self->sym_loaded;
}


// ----------------------------------------------------------------------------
static int
_py_proc__get_modules(py_proc_t * self) {
  HANDLE mod_hdl;
  mod_hdl = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, self->pid);
  if (mod_hdl == INVALID_HANDLE_VALUE)
    FAIL;

  MODULEENTRY32 module;
  module.dwSize = sizeof(module);

  self->min_raddr = (void *) -1;
  self->max_raddr = NULL;

  sfree(self->bin_path);
  sfree(self->lib_path);

  BOOL success = Module32First(mod_hdl, &module);
  while (success) {
    if ((void *) module.modBaseAddr < self->min_raddr)
      self->min_raddr = module.modBaseAddr;

    if ((void *) module.modBaseAddr + module.modBaseSize > self->max_raddr)
      self->max_raddr = module.modBaseAddr + module.modBaseSize;

    log_t(
      "%p-%p:  Module %s",
      module.modBaseAddr, module.modBaseAddr + module.modBaseSize,
      module.szModule
    );
    if (
      self->bin_path == NULL \
      && strcmp(module.szModule, "py.exe") \
      && strstr(module.szModule, ".exe") \
    ) {
      log_d("Candidate binary: %s (size %d KB)", module.szModule, module.modBaseSize >> 10);
      self->bin_path = strdup(module.szExePath);
    }
    if (!self->sym_loaded && strstr(module.szModule, ".dll") && module.modBaseSize > (1 << 20)) {
      self->map.bss.base = module.modBaseAddr;  // WARNING: Not the BSS base yet!
      if (success(_py_proc__analyze_pe(self, module.szExePath))) {
        log_d("Candidate library: %s (size %d KB)", module.szModule, module.modBaseSize >> 10);
        sfree(self->lib_path);
        self->lib_path = strdup(module.szExePath);
      }
    }

    if (self->bin_path != NULL && self->lib_path != NULL && self->sym_loaded)
      break;

    success = Module32Next(mod_hdl, &module);
  }

  CloseHandle(mod_hdl);

  return !self->sym_loaded;
}


// ----------------------------------------------------------------------------
static ssize_t _py_proc__get_resident_memory(py_proc_t * self) {
  PROCESS_MEMORY_COUNTERS mem_info;

  return GetProcessMemoryInfo(self->extra->h_proc, &mem_info, sizeof(mem_info))
    ? mem_info.WorkingSetSize
    : -1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__init(py_proc_t * self) {
  if (!isvalid(self))
    FAIL;

  return _py_proc__get_modules(self);
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


// ----------------------------------------------------------------------------
// Forward declaration.
static int
_py_proc__run(py_proc_t *);


// On Windows, if we fail with the parent process we look if it has a single
// child and try to attach to that instead. We keep going until we either find
// a single Python process or more or less than a single child.
static int
_py_proc__try_child_proc(py_proc_t * self) {
  log_d("Process is not Python so we look for a single child Python process");

  HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (h == INVALID_HANDLE_VALUE) {
    log_e("Cannot inspect processes details");
    FAIL;
  }

with_resources;

  HANDLE orig_hproc = self->extra->h_proc;
  pid_t  orig_pid   = self->pid;
  while (TRUE) {
    pid_t parent_pid = self->pid;

    PROCESSENTRY32 pe = { 0 };
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(h, &pe)) {
      pid_t child_pid = 0;
      do {
        if (pe.th32ParentProcessID == parent_pid) {
          if (child_pid) {
            log_d("Process has more than one child");
            NOK;
          }
          child_pid = pe.th32ProcessID;
        }
      } while (Process32Next(h, &pe));

      if (!child_pid) {
        log_d("Process has no children");
        NOK;
      }

      self->pid = child_pid;
      self->extra->h_proc = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, child_pid
      );
      if (self->extra->h_proc == INVALID_HANDLE_VALUE) {
        log_e("Cannot open child process handle");
        NOK;
      }
      if (success(_py_proc__run(self))) {
        log_d("Process has a single Python child with PID %d. We will attach to that", child_pid);
        OK;
      }
      else {
        log_d("Process has a single non-Python child with PID %d. Taking it as new parent", child_pid);
        CloseHandle(self->extra->h_proc);
      }
    }
  }

release:
  CloseHandle(h);
  if (retval) {
    self->pid = orig_pid;
    self->extra->h_proc = orig_hproc;
  }
  
  released;
}

#endif
