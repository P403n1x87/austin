// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2022 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

// This source has been adapted from
// https://github.com/bminor/binutils-gdb/blob/ce230579c65b9e04c830f35cb78ff33206e65db1/binutils/addr2line.c

#include <bfd.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_LIBERTY
#include <libiberty/demangle.h>
#endif

#include "../logging.h"
#include "../stack.h"

static asymbol **syms; /* Symbol table.  */

static void slurp_symtab(bfd *);
static void find_address_in_section(bfd *, asection *, void *);

#define string__startswith(str, head) (strncmp(head, str, strlen(head)) == 0)

// TODO: This is incomplete or plain incorrect
static inline char *
demangle_cython(char *function)
{
    if (!isvalid(function))
        return NULL;

    char *f = function;

    if (string__startswith(f, "__pyx_pw_") || string__startswith(f, "__pyx_pf_"))
        return function;

    if (string__startswith(function, "__pyx_pymod_"))
        return strchr(f + 12, '_') + 1;

    if (string__startswith(f, "__pyx_fuse_"))
        function = strstr(f + 12, "__pyx_") + 12;

    while (!isdigit(*f))
    {
        if (*(f++) == '\0')
            return function;
    }

    int n = 0;
    while (*f != '\0')
    {
        puts(f);
        char c = *(f++);
        if (isdigit(c))
            n = n * 10 + (c - '0');
        else
        {
            f += n;
            n = 0;
            if (!isdigit(*f))
                return f;
        }
    }

    return function;
}

/* Read in the symbol table.  */

static void
slurp_symtab(bfd *abfd)
{
    long storage;
    long symcount;
    bool dynamic = false;

    if ((bfd_get_file_flags(abfd) & HAS_SYMS) == 0)
        return;

    storage = bfd_get_symtab_upper_bound(abfd);
    if (storage == 0)
    {
        storage = bfd_get_dynamic_symtab_upper_bound(abfd);
        dynamic = true;
    }
    if (storage < 0)
        return;

    syms = (asymbol **)malloc(storage);
    if (dynamic)
        symcount = bfd_canonicalize_dynamic_symtab(abfd, syms);
    else
        symcount = bfd_canonicalize_symtab(abfd, syms);
    if (symcount < 0)
        return;

    /* If there are no symbols left after canonicalization and
     we have not tried the dynamic symbols then give them a go.  */
    if (symcount == 0 && !dynamic && (storage = bfd_get_dynamic_symtab_upper_bound(abfd)) > 0)
    {
        free(syms);
        syms = (asymbol **)malloc(storage);
        symcount = bfd_canonicalize_dynamic_symtab(abfd, syms);
    }

    /* PR 17512: file: 2a1d3b5b.
     Do not pretend that we have some symbols when we don't.  */
    if (symcount <= 0)
    {
        free(syms);
        syms = NULL;
    }
}

static bfd_vma pc;
static const char *filename;
static const char *functionname;
static unsigned int line;
static unsigned int discriminator;

/* Look for an address in a section.  This is called via
   bfd_map_over_sections.  */

static void
find_address_in_section(bfd *abfd, asection *section, void *data ATTRIBUTE_UNUSED)
{
    bfd_vma vma;
    bfd_size_type size;

    if ((bfd_section_flags(section) & SEC_ALLOC) == 0)
        return;

    vma = bfd_section_vma(section);
    if (pc < vma)
        return;

    size = bfd_section_size(section);
    if (pc >= vma + size)
        return;

    bfd_find_nearest_line_discriminator(abfd, section, syms, pc - vma,
                                        &filename, &functionname,
                                        &line, &discriminator);
}

static inline frame_t *
get_native_frame(const char *file_name, bfd_vma addr)
{
    bfd *abfd;
    char **matching;

    // TODO: This would be much cheaper if we could read directly from memory.
    abfd = bfd_openr(file_name, NULL);
    if (abfd == NULL)
    {
        log_e("Failed to open %s", file_name);
        return NULL;
    }

    /* Decompress sections.  */
    abfd->flags |= BFD_DECOMPRESS;

    if (bfd_check_format(abfd, bfd_archive))
    {
        log_e("BFD format check failed");
        return NULL;
    }

    if (!bfd_check_format_matches(abfd, bfd_object, &matching))
    {
        free(matching);
        log_d("BFC format matches check failed.");
        return NULL;
    }

    slurp_symtab(abfd);

    // Reset global state for a new lookup
    filename = functionname = NULL;
    line = discriminator = 0;
    pc = addr;

    bfd_map_over_sections(abfd, find_address_in_section, NULL);

    const char *name;
    char *alloc = NULL;

    name = functionname;
    if (name == NULL || *name == '\0')
        name = "<unnamed>";
#ifdef HAVE_LIBERTY
    else
    {
        alloc = bfd_demangle(abfd, name, DMGL_PARAMS | DMGL_ANSI);
        if (alloc != NULL)
            name = alloc;
    }
#endif

    free(syms);
    syms = NULL;

    frame_t *frame = isvalid(filename) && isvalid(name)
                         ? frame_new(strdup(filename), strdup(name), line)
                         : NULL;

    bfd_close(abfd);

    return frame;
}
