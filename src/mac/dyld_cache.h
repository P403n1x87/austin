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

// Dyld shared cache loader for macOS native symbol resolution.
//
// On modern macOS (11+), system frameworks (CoreFoundation, Security, etc.)
// no longer exist as individual files on disk.  They live exclusively inside
// the dyld shared cache.  This module memory-maps the cache files, builds a
// unified virtual-address-to-data mapping table, and provides helpers to
// locate an image by path and translate a VM address into a pointer suitable
// for Mach-O parsing.

#pragma once

#include <fcntl.h>
#include <mach-o/dyld_images.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hints.h"
#include "../logging.h"

// -- Dyld shared cache on-disk format -----------------------------------------
// Minimal struct definitions derived from Apple's open-source dyld project
// (apple-oss-distributions/dyld, include/mach-o/dyld_cache_format.h).
// We only define what we need; field offsets are validated against the
// mappingOffset sentinel so we never read past the end of the header.

// Header field byte offsets (little-endian, native on all supported macOS).
#define _DSC_OFF_MAPPING_OFFSET    0x010
#define _DSC_OFF_MAPPING_COUNT     0x014
#define _DSC_OFF_IMAGES_OFFSET_OLD 0x018
#define _DSC_OFF_IMAGES_COUNT_OLD  0x01C
#define _DSC_OFF_SUBCACHE_OFFSET   0x188
#define _DSC_OFF_SUBCACHE_COUNT    0x18C
#define _DSC_OFF_IMAGES_OFFSET     0x1C0
#define _DSC_OFF_IMAGES_COUNT      0x1C4

// Minimum mappingOffset required for each field group to be present.
#define _DSC_MIN_HDR_SUBCACHE_V2 0x1D0
#define _DSC_MIN_HDR_IMAGES_NEW  0x1C8

struct _dsc_mapping_info {
    uint64_t address;
    uint64_t size;
    uint64_t fileOffset;
    uint32_t maxProt;
    uint32_t initProt;
};

struct _dsc_image_info {
    uint64_t address;
    uint64_t modTime;
    uint64_t inode;
    uint32_t pathFileOffset;
    uint32_t pad;
};

// V2 subcache entry (macOS 13+, carries a file-name suffix like ".01").
struct _dsc_subcache_entry {
    uint8_t  uuid[16];
    uint64_t cacheVMOffset;
    char     fileSuffix[32];
};

// -- Runtime state for a loaded cache -----------------------------------------

// One memory-mapped cache file (root or subcache).
typedef struct {
    void*  base;
    size_t size;
} _dsc_file_t;

// A resolved mapping: VM range -> pointer into an mmap'd file.
typedef struct {
    uint64_t vm_addr;
    uint64_t vm_size;
    void*    data; // file->base + mapping.fileOffset
} _dsc_map_t;

// The complete loaded dyld shared cache.
typedef struct {
    _dsc_file_t*            files;
    size_t                  file_count;
    _dsc_map_t*             maps;
    size_t                  map_count;
    struct _dsc_image_info* images;
    uint32_t                image_count;
    void*                   root_base;
} _dsc_t;

static _dsc_t  _dsc_no_cache_sentinel;
static _dsc_t* _dsc_instance = NULL;

// -- Helpers ------------------------------------------------------------------

static inline uint32_t
_dsc_u32(const void* base, size_t off) {
    uint32_t v;
    memcpy(&v, (const char*)base + off, sizeof(v));
    return v;
}

// Translate a virtual address to a pointer in the mmap'd cache data.
// Returns NULL if the address falls outside all known mappings.
static void*
_dsc_translate(const _dsc_t* dsc, uint64_t vmaddr) {
    for (size_t i = 0; i < dsc->map_count; i++) {
        if (vmaddr >= dsc->maps[i].vm_addr && vmaddr < dsc->maps[i].vm_addr + dsc->maps[i].vm_size) {
            return (char*)dsc->maps[i].data + (vmaddr - dsc->maps[i].vm_addr);
        }
    }
    return NULL;
}

// Open and mmap a file read-only.
static int
_dsc_mmap_file(const char* path, _dsc_file_t* out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0x20) {
        close(fd);
        return -1;
    }

    void* base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (base == MAP_FAILED)
        return -1;

    out->base = base;
    out->size = (size_t)st.st_size;
    return 0;
}

// Append the mapping table from one cache file to the resolved-maps array.
// Returns the new write index.
static size_t
_dsc_collect_mappings(const _dsc_file_t* file, _dsc_map_t* maps, size_t idx) {
    uint32_t off   = _dsc_u32(file->base, _DSC_OFF_MAPPING_OFFSET);
    uint32_t count = _dsc_u32(file->base, _DSC_OFF_MAPPING_COUNT);

    const struct _dsc_mapping_info* mi = (const struct _dsc_mapping_info*)((const char*)file->base + off);

    for (uint32_t m = 0; m < count; m++) {
        maps[idx].vm_addr = mi[m].address;
        maps[idx].vm_size = mi[m].size;
        maps[idx].data    = (char*)file->base + mi[m].fileOffset;
        idx++;
    }
    return idx;
}

// -- Cache loader -------------------------------------------------------------

static _dsc_t*
_dsc_load(void) {
#if defined(__arm64__) || defined(__aarch64__)
    const char* arch = "arm64e";
#elif defined(__x86_64__)
    const char* arch = "x86_64";
#else
    return NULL;
#endif

    // Try macOS 13+ Cryptex path first, fall back to the legacy location.
    char        root_path[MAXPATHLEN];
    _dsc_file_t root = {0};

    snprintf(
        root_path, sizeof(root_path),
        "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/"
        "dyld_shared_cache_%s",
        arch
    );
    if (_dsc_mmap_file(root_path, &root) != 0) {
        snprintf(root_path, sizeof(root_path), "/System/Library/dyld/dyld_shared_cache_%s", arch);
        if (_dsc_mmap_file(root_path, &root) != 0) {
            log_d("dyld_cache: cannot open shared cache");
            return NULL;
        }
    }

    // Validate magic ("dyld_v1  " prefix).
    if (memcmp(root.base, "dyld_v1", 7) != 0) {
        munmap(root.base, root.size);
        log_d("dyld_cache: bad magic");
        return NULL;
    }

    uint32_t mappingOffset = _dsc_u32(root.base, _DSC_OFF_MAPPING_OFFSET);
    uint32_t rootMapCount  = _dsc_u32(root.base, _DSC_OFF_MAPPING_COUNT);

    // ---- Subcache discovery (V2 format, macOS 13+) ----
    uint32_t                          subCount   = 0;
    const struct _dsc_subcache_entry* subEntries = NULL;

    if (mappingOffset >= _DSC_MIN_HDR_SUBCACHE_V2) {
        uint32_t subOff = _dsc_u32(root.base, _DSC_OFF_SUBCACHE_OFFSET);
        subCount        = _dsc_u32(root.base, _DSC_OFF_SUBCACHE_COUNT);
        if (subCount > 0 && subOff > 0)
            subEntries = (const struct _dsc_subcache_entry*)((const char*)root.base + subOff);
        else
            subCount = 0;
    }

    // ---- Image list ----
    uint32_t imagesOffset, imagesCount;
    if (mappingOffset >= _DSC_MIN_HDR_IMAGES_NEW) {
        imagesOffset = _dsc_u32(root.base, _DSC_OFF_IMAGES_OFFSET);
        imagesCount  = _dsc_u32(root.base, _DSC_OFF_IMAGES_COUNT);
    } else {
        imagesOffset = _dsc_u32(root.base, _DSC_OFF_IMAGES_OFFSET_OLD);
        imagesCount  = _dsc_u32(root.base, _DSC_OFF_IMAGES_COUNT_OLD);
    }

    log_d("dyld_cache: %s — %u images, %u subcaches, %u root mappings", root_path, imagesCount, subCount, rootMapCount);

    // ---- Allocate file array (root + subcaches) ----
    size_t       file_count = 1 + (size_t)subCount;
    _dsc_file_t* files      = (_dsc_file_t*)calloc(file_count, sizeof(_dsc_file_t));
    if (!files) {
        munmap(root.base, root.size);
        return NULL;
    }
    files[0] = root;

    // Count total mappings (root + all subcaches) so we can allocate once.
    size_t total_maps = rootMapCount;

    for (uint32_t s = 0; s < subCount; s++) {
        char sub_path[MAXPATHLEN];
        snprintf(sub_path, sizeof(sub_path), "%s%s", root_path, subEntries[s].fileSuffix);

        if (_dsc_mmap_file(sub_path, &files[1 + s]) != 0) {
            log_d("dyld_cache: failed to mmap subcache %s", sub_path);
            continue;
        }
        total_maps += _dsc_u32(files[1 + s].base, _DSC_OFF_MAPPING_COUNT);
    }

    // ---- Build resolved mapping table ----
    _dsc_map_t* maps = (_dsc_map_t*)calloc(total_maps, sizeof(_dsc_map_t));
    if (!maps) {
        for (size_t i = 0; i < file_count; i++)
            if (files[i].base)
                munmap(files[i].base, files[i].size);
        free(files);
        return NULL;
    }

    size_t map_idx = _dsc_collect_mappings(&files[0], maps, 0);
    for (uint32_t s = 0; s < subCount; s++) {
        if (files[1 + s].base)
            map_idx = _dsc_collect_mappings(&files[1 + s], maps, map_idx);
    }

    // ---- Assemble the cache descriptor ----
    _dsc_t* dsc = (_dsc_t*)malloc(sizeof(_dsc_t));
    if (!dsc) {
        free(maps);
        for (size_t i = 0; i < file_count; i++)
            if (files[i].base)
                munmap(files[i].base, files[i].size);
        free(files);
        return NULL;
    }

    dsc->files       = files;
    dsc->file_count  = file_count;
    dsc->maps        = maps;
    dsc->map_count   = map_idx;
    dsc->images      = (struct _dsc_image_info*)((char*)root.base + imagesOffset);
    dsc->image_count = imagesCount;
    dsc->root_base   = root.base;

    log_d("dyld_cache: loaded %zu mappings across %zu files", map_idx, file_count);
    return dsc;
}

// -- Public helpers -----------------------------------------------------------

// Get the singleton cache instance (lazy-loaded, process lifetime).
// Returns NULL if the cache could not be loaded.
static _dsc_t*
_dsc_get(void) {
    if (_dsc_instance == NULL) {
        _dsc_t* loaded = _dsc_load();
        _dsc_instance  = loaded ? loaded : &_dsc_no_cache_sentinel;
    }
    return _dsc_instance == &_dsc_no_cache_sentinel ? NULL : _dsc_instance;
}

// Find a cached image by its install path (e.g.
// "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation").
// Returns the un-slid VM address of the image's Mach-O header, or 0.
static uint64_t
_dsc_find_image(const _dsc_t* dsc, const char* path) {
    for (uint32_t i = 0; i < dsc->image_count; i++) {
        const char* img_path = (const char*)dsc->root_base + dsc->images[i].pathFileOffset;
        if (strcmp(img_path, path) == 0)
            return dsc->images[i].address;
    }
    return 0;
}

// -- Sorted image table for PC-to-image lookup --------------------------------

// Entry in the sorted lookup table: (un-slid address, index into dsc->images).
typedef struct {
    uint64_t address;
    uint32_t index;
} _dsc_image_entry_t;

static _dsc_image_entry_t* _dsc_sorted_images = NULL;
static uint32_t            _dsc_sorted_count  = 0;

static int
_dsc_image_entry_cmp(const void* a, const void* b) {
    const _dsc_image_entry_t* ea = (const _dsc_image_entry_t*)a;
    const _dsc_image_entry_t* eb = (const _dsc_image_entry_t*)b;
    return (ea->address > eb->address) - (ea->address < eb->address);
}

// Build the sorted image table (once, on first use).
static void
_dsc_build_sorted_images(const _dsc_t* dsc) {
    if (_dsc_sorted_images)
        return;

    _dsc_sorted_images = (_dsc_image_entry_t*)malloc(dsc->image_count * sizeof(_dsc_image_entry_t));
    if (!_dsc_sorted_images)
        return;

    for (uint32_t i = 0; i < dsc->image_count; i++) {
        _dsc_sorted_images[i].address = dsc->images[i].address;
        _dsc_sorted_images[i].index   = i;
    }

    qsort(_dsc_sorted_images, dsc->image_count, sizeof(_dsc_image_entry_t), _dsc_image_entry_cmp);
    _dsc_sorted_count = dsc->image_count;

    log_d("dyld_cache: built sorted image table (%u entries)", _dsc_sorted_count);
}

// -- Shared cache slide -------------------------------------------------------

static uintptr_t _dsc_slide       = 0;
static bool      _dsc_slide_valid = false;

// Read the dyld shared cache ASLR slide from a target process via its task port.
static uintptr_t
_dsc_get_slide(mach_port_t task) {
    if (_dsc_slide_valid)
        return _dsc_slide;

    task_dyld_info_data_t  dyld_info = {0};
    mach_msg_type_number_t count     = TASK_DYLD_INFO_COUNT;

    if (task_info(task, TASK_DYLD_INFO, (task_info_t)&dyld_info, &count) != KERN_SUCCESS) {
        log_d("dyld_cache: task_info(TASK_DYLD_INFO) failed");
        return 0;
    }

    // Read just the fields we need from dyld_all_image_infos.
    // sharedCacheSlide is at a known offset (version >= 12).
    struct {
        uint32_t version;
        uint32_t infoArrayCount;
        uint64_t infoArray;
        uint64_t notification;
        uint8_t  processDetachedFromSharedRegion;
        uint8_t  libSystemInitialized;
        uint8_t  _pad[6];
        uint64_t dyldImageLoadAddress;
        uint64_t jitInfo;
        uint64_t dyldVersion;
        uint64_t errorMessage;
        uint64_t terminationFlags;
        uint64_t coreSymbolicationShmPage;
        uint64_t systemOrderFlag;
        uint64_t uuidArrayCount;
        uint64_t uuidArray;
        uint64_t dyldAllImageInfosAddress;
        uint64_t initialImageCount;
        uint64_t errorKind;
        uint64_t errorClientOfDylibPath;
        uint64_t errorTargetDylibPath;
        uint64_t errorSymbol;
        uint64_t sharedCacheSlide;
    } infos = {0};

    mach_vm_size_t sz = sizeof(infos);
    if (mach_vm_read_overwrite(
            task, (mach_vm_address_t)dyld_info.all_image_info_addr, sz, (mach_vm_address_t)&infos, &sz
        )
        != KERN_SUCCESS) {
        log_d("dyld_cache: failed to read dyld_all_image_infos");
        return 0;
    }

    if (infos.version < 12) {
        log_d("dyld_cache: dyld_all_image_infos version %u too old (need >= 12)", infos.version);
        return 0;
    }

    _dsc_slide       = (uintptr_t)infos.sharedCacheSlide;
    _dsc_slide_valid = true;

    log_d("dyld_cache: shared cache slide = 0x%" PRIxPTR, _dsc_slide);
    return _dsc_slide;
}

// -- Fast range check ---------------------------------------------------------

// Cached runtime address range of the shared cache (computed once).
static uint64_t _dsc_runtime_lo  = 0;
static uint64_t _dsc_runtime_hi  = 0;
static bool     _dsc_range_valid = false;

// O(1) check: does this runtime PC fall inside the dyld shared cache?
// The first call triggers the (one-time) slide read; after that it's a
// simple comparison, avoiding the expensive proc_regionfilename() Mach trap
// that always fails for shared-cache addresses.
static bool
_dsc_pc_in_cache(mach_port_t task, uintptr_t pc) {
    if (unlikely(!_dsc_range_valid)) {
        _dsc_t* dsc = _dsc_get();
        if (!dsc || dsc->map_count == 0)
            return false;

        uintptr_t slide = _dsc_get_slide(task);

        uint64_t lo = dsc->maps[0].vm_addr;
        uint64_t hi = lo;
        for (size_t i = 0; i < dsc->map_count; i++) {
            if (dsc->maps[i].vm_addr < lo)
                lo = dsc->maps[i].vm_addr;
            uint64_t end = dsc->maps[i].vm_addr + dsc->maps[i].vm_size;
            if (end > hi)
                hi = end;
        }

        _dsc_runtime_lo  = lo + slide;
        _dsc_runtime_hi  = hi + slide;
        _dsc_range_valid = true;

        log_d("dyld_cache: runtime range 0x%" PRIx64 " – 0x%" PRIx64, _dsc_runtime_lo, _dsc_runtime_hi);
    }

    return (uint64_t)pc >= _dsc_runtime_lo && (uint64_t)pc < _dsc_runtime_hi;
}

// -- PC-to-image path resolution ----------------------------------------------

// Given a runtime PC that falls in the shared cache, return the image path.
// Returns NULL if the PC is not in any cached image.
// If image_va_out is non-NULL, stores the un-slid image address there.
static const char*
_dsc_path_for_pc(mach_port_t task, uintptr_t pc, uint64_t* image_va_out) {
    _dsc_t* dsc = _dsc_get();
    if (!dsc)
        return NULL;

    uintptr_t slide = _dsc_get_slide(task);
    if (slide == 0 && !_dsc_slide_valid)
        return NULL;

    _dsc_build_sorted_images(dsc);
    if (!_dsc_sorted_images)
        return NULL;

    // un-slid PC
    uint64_t cache_pc = (uint64_t)(pc - slide);

    // Binary search: find the greatest entry whose address <= cache_pc.
    uint32_t lo = 0, hi = _dsc_sorted_count;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (_dsc_sorted_images[mid].address <= cache_pc)
            lo = mid;
        else
            hi = mid;
    }

    if (_dsc_sorted_images[lo].address > cache_pc)
        return NULL;

    uint32_t    img_idx = _dsc_sorted_images[lo].index;
    const char* path    = (const char*)dsc->root_base + dsc->images[img_idx].pathFileOffset;

    if (image_va_out)
        *image_va_out = dsc->images[img_idx].address;

    return path;
}
