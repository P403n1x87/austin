#include <tlhelp32.h>

#include "../py_proc.h"


#define MODULE_CNT                     2

// ----------------------------------------------------------------------------
static int
_py_proc__get_modules(py_proc_t * self) {
  // TODO: https://stackoverflow.com/questions/3313581/runtime-process-memory-patching-for-restoring-state/3313700#3313700
  DWORD pid = GetProcessId((HANDLE) self->pid);

  HANDLE mod_hdl;
  mod_hdl = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (mod_hdl == INVALID_HANDLE_VALUE)
    return 1;

  MODULEENTRY32 lpme;
  lpme.dwSize = sizeof(lpme);

  register int map_cnt = 0;
  proc_vm_map_block_t * vm_maps = (proc_vm_map_block_t *) &(self->map);
  BOOL success = Module32First(mod_hdl, &lpme);
  while (success && map_cnt < MODULE_CNT) {
    // log_d("%p-%p  Module: %s", lpme.modBaseAddr, lpme.modBaseAddr + lpme.modBaseSize, lpme.szModule);
    if (strstr(lpme.szModule, "python")) {
      if (self->bin_path == NULL && strstr(lpme.szModule, "python.exe")) {
        self->bin_path = (char *) malloc(strlen(lpme.szExePath) + 1);
        strcpy(self->bin_path, lpme.szExePath);
        #ifdef DEBUG
        log_d("Python binary: %s", self->bin_path);
        #endif
      }
      vm_maps[map_cnt].base = lpme.modBaseAddr;
      vm_maps[map_cnt].size = lpme.modBaseSize;
      // log_d("%p-%p: module %s", lpme.modBaseAddr, lpme.modBaseAddr + lpme.modBaseSize, lpme.szModule);
      map_cnt++;
    }

    success = Module32Next(mod_hdl, &lpme);
  }

  CloseHandle(mod_hdl);

  return map_cnt == MODULE_CNT ? 0 : 1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_bin(py_proc_t * self) {
  if (self == NULL)
    return 1;

  if (self->maps_loaded == 0) {
    self->maps_loaded = 1 - _py_proc__get_modules(self);

    if (self->maps_loaded == 0)
      return 1;
  }

  // Not loading simbols on Windows
  self->sym_loaded = 1;

  return 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__is_raddr(py_proc_t * self, void * raddr) {
  if (self == NULL || raddr == NULL)
    return 0;

  proc_vm_map_block_t * vm_maps = (proc_vm_map_block_t *) &(self->map);
  for (register int i = 0; i < MODULE_CNT; i++) {
    if (raddr >= vm_maps[i].base && raddr < vm_maps[i].base + vm_maps[i].size)
      return 1;
  }

  return 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__scan_module(py_proc_t * self, proc_vm_map_block_t * mod) {
  if (py_proc__memcpy(self, mod->base, mod->size, self->bss))
    return 1;

  void * upper_bound = self->bss + mod->size;
  for (
    register void ** raddr = (void **) self->bss;
    (void *) raddr < upper_bound;
    raddr++
  ) {
    if (_py_proc__is_raddr(self, *raddr) && _py_proc__check_interp_state(self, *raddr) == 0) {
      self->is_raddr = *raddr;
      return 0;
    }
  }

  return 1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__wait_for_interp_state(py_proc_t * self) {
  if (self == NULL)
    return 1;

  register int try_cnt = INIT_RETRY_CNT;

  // Wait for memory maps
  while (--try_cnt) {
    usleep(INIT_RETRY_SLEEP);

    if (self->sym_loaded == 0) {
      _py_proc__analyze_bin(self);
      if (self->sym_loaded == 0)
        continue;
    }

    if (!self->version) {
      self->version = _py_proc__get_version(self);
      if (!self->version)
        return 1;

      set_version(self->version);
      break;
    }
  }

  if (try_cnt) {
    proc_vm_map_block_t * vm_maps = (proc_vm_map_block_t *) &(self->map);

    for (register int i = 0; i < MODULE_CNT; i++) {
      self->bss = malloc(vm_maps[i].size);
      if (self->bss == NULL)
        return 1;

      try_cnt = INIT_RETRY_CNT;
      while (--try_cnt) {
        usleep(INIT_RETRY_SLEEP);

        if (_py_proc__scan_module(self, &(vm_maps[i])) == 0)
          return 0;
      }
      free(self->bss);
    }
  }

  error = EPROCISTIMEOUT;
  return 1;
}
