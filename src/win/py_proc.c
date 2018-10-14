#include <tlhelp32.h>

#include "../py_proc.h"


#define MODULE_CNT                     2


// ----------------------------------------------------------------------------
// TODO: Optimise by avoiding executing the same code over and over again
static void *
RVAToFileMap(void * bin, DWORD rva) {
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
  HANDLE hMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY,0,0,0);
  LPVOID pMapping = MapViewOfFile(hMapping,FILE_MAP_READ,0,0,0);

  IMAGE_DOS_HEADER     * dos_hdr = (IMAGE_DOS_HEADER *) pMapping;
  IMAGE_NT_HEADERS     * nt_hdr  = (IMAGE_NT_HEADERS *) (pMapping + dos_hdr->e_lfanew);
  IMAGE_SECTION_HEADER * s_hdr   = (IMAGE_SECTION_HEADER *) (pMapping + dos_hdr->e_lfanew + sizeof(IMAGE_NT_HEADERS));

  if (nt_hdr->Signature != IMAGE_NT_SIGNATURE)
    return 1;

  ULONGLONG base = self->map.bss.base;  // nt_hdr->OptionalHeader.ImageBase;

  // ---- Find the .data section ----
  for (register int i = 0; i < nt_hdr->FileHeader.NumberOfSections; i++) {
    if (strcmp(".data", (const char *) s_hdr[i].Name) == 0) {
      self->map.bss.base += s_hdr[i].VirtualAddress;
      self->map.bss.size = s_hdr[i].Misc.VirtualSize;
      break;
    }
  }

  // ---- Search for exports ----
  register int hit_cnt = 0;

  IMAGE_EXPORT_DIRECTORY * e_dir = (IMAGE_EXPORT_DIRECTORY *) RVAToFileMap(pMapping, nt_hdr->OptionalHeader.DataDirectory[0].VirtualAddress);
  if (e_dir != NULL) {
    DWORD * names   = (DWORD *) RVAToFileMap(pMapping, e_dir->AddressOfNames);
    WORD  * idx_tab = (WORD *)  RVAToFileMap(pMapping, e_dir->AddressOfNameOrdinals);
    DWORD * addrs   = (DWORD *) RVAToFileMap(pMapping, e_dir->AddressOfFunctions);
    for (register int i = 0; i < e_dir->NumberOfFunctions; i++) {
      char * sym_name = (char *) RVAToFileMap(pMapping, names[i]);
      // log_d("Symbol: %s", sym_name);
      long hash = string_hash(sym_name);
      for (register int i = 0; i < DYNSYM_COUNT; i++) {
        if (hash == _dynsym_hash_array[i] && strcmp(sym_name, _dynsym_array[i]) == 0) {
          *(&(self->tstate_curr_raddr) + i) = (void *) (addrs[idx_tab[i]] + base);
          hit_cnt++;
          #ifdef DEBUG
          log_d("Symbol %s found at %p", sym_name, addrs[idx_tab[i]] + base);
          #endif
        }
      }
    }
  }

  UnmapViewOfFile(pMapping);
  CloseHandle(hMapping);
  CloseHandle(hFile);

  return hit_cnt;
}


// ----------------------------------------------------------------------------
static int
_py_proc__get_modules(py_proc_t * self) {
  // TODO: https://stackoverflow.com/questions/3313581/runtime-process-memory-patching-for-restoring-state/3313700#3313700
  DWORD pid = GetProcessId((HANDLE) self->pid);

  HANDLE mod_hdl;
  mod_hdl = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (mod_hdl == INVALID_HANDLE_VALUE)
    return 1;

  MODULEENTRY32 module;
  module.dwSize = sizeof(module);

  register int map_cnt = 0;  // TODO: Replace with flags
  // proc_vm_map_block_t * vm_maps = (proc_vm_map_block_t *) &(self->map);
  BOOL success = Module32First(mod_hdl, &module);
  while (success && map_cnt < MODULE_CNT) {
    // log_d("%p-%p  Module: %s", module.modBaseAddr, module.modBaseAddr + module.modBaseSize, module.szModule);
    if (strstr(module.szModule, "python")) {
      if (self->bin_path == NULL && strstr(module.szModule, "python.exe")) {
        self->bin_path = (char *) malloc(strlen(module.szExePath) + 1);
        strcpy(self->bin_path, module.szExePath);
        #ifdef DEBUG
        log_d("Python binary: %s", self->bin_path);
        #endif
      }
      if (strstr(module.szModule, ".dll")) {
        self->map.bss.base = module.modBaseAddr;
        _py_proc__analyze_pe(self, module.szExePath);
      }
      // vm_maps[map_cnt].base = module.modBaseAddr;
      // vm_maps[map_cnt].size = module.modBaseSize;
      // log_d("%p-%p: module %s", module.modBaseAddr, module.modBaseAddr + module.modBaseSize, module.szModule);
      map_cnt++;
    }

    success = Module32Next(mod_hdl, &module);
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
// static int
// _py_proc__is_raddr(py_proc_t * self, void * raddr) {
//   if (self == NULL || raddr == NULL)
//     return 0;
//
//   proc_vm_map_block_t * vm_maps = (proc_vm_map_block_t *) &(self->map);
//   for (register int i = 0; i < MODULE_CNT; i++) {
//     if (raddr >= vm_maps[i].base && raddr < vm_maps[i].base + vm_maps[i].size)
//       return 1;
//   }
//
//   return 0;
// }


// ----------------------------------------------------------------------------
// static int
// _py_proc__scan_module(py_proc_t * self, proc_vm_map_block_t * mod) {
//   if (py_proc__memcpy(self, mod->base, mod->size, self->bss))
//     return 1;
//
//   void * upper_bound = self->bss + mod->size;
//   for (
//     register void ** raddr = (void **) self->bss;
//     (void *) raddr < upper_bound;
//     raddr++
//   ) {
//     if (_py_proc__is_raddr(self, *raddr) && _py_proc__check_interp_state(self, *raddr) == 0) {
//       self->is_raddr = *raddr;
//       return 0;
//     }
//   }
//
//   return 1;
// }


// ----------------------------------------------------------------------------
// static int
// _py_proc__wait_for_interp_state(py_proc_t * self) {
//   if (self == NULL)
//     return 1;
//
//   register int try_cnt = INIT_RETRY_CNT;
//
//   // Wait for memory maps
//   while (--try_cnt) {
//     usleep(INIT_RETRY_SLEEP);
//
//     if (self->sym_loaded == 0) {
//       _py_proc__analyze_bin(self);
//       if (self->sym_loaded == 0)
//         continue;
//     }
//
//     if (!self->version) {
//       self->version = _py_proc__get_version(self);
//       if (!self->version)
//         return 1;
//
//       set_version(self->version);
//       break;
//     }
//   }
//
//   if (try_cnt) {
//     proc_vm_map_block_t * vm_maps = (proc_vm_map_block_t *) &(self->map);
//
//     for (register int i = 0; i < MODULE_CNT; i++) {
//       self->bss = malloc(vm_maps[i].size);
//       if (self->bss == NULL)
//         return 1;
//
//       try_cnt = INIT_RETRY_CNT;
//       while (--try_cnt) {
//         usleep(INIT_RETRY_SLEEP);
//
//         if (_py_proc__scan_module(self, &(vm_maps[i])) == 0)
//           return 0;
//       }
//       free(self->bss);
//     }
//   }
//
//   error = EPROCISTIMEOUT;
//   return 1;
// }
