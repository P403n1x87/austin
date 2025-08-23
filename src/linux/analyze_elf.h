// ----------------------------------------------------------------------------
static Elf64_Addr
_get_base_64(Elf64_Ehdr* ehdr, void* elf_map) {
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        Elf64_Phdr* phdr = (Elf64_Phdr*)(elf_map + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type == PT_LOAD)
            return phdr->p_vaddr - phdr->p_vaddr % phdr->p_align;
    }
    return UINT64_MAX;
} /* _get_base_64 */

static int
_py_proc__analyze_elf64(py_proc_t* self, void* elf_map, void* elf_base, proc_vm_map_block_t* bss) {
    register int symbols = 0;

    Elf64_Ehdr* ehdr = elf_map;

    // Section header must be read from binary as it is not loaded into memory
    Elf64_Xword sht_size     = ehdr->e_shnum * ehdr->e_shentsize;
    Elf64_Off   elf_map_size = ehdr->e_shoff + sht_size;
    Elf64_Shdr* p_shdr;

    Elf64_Shdr* p_shstrtab   = elf_map + ELF_SH_OFF(ehdr, ehdr->e_shstrndx);
    char*       sh_name_base = elf_map + p_shstrtab->sh_offset;
    Elf64_Shdr* p_dynsym     = NULL;
    Elf64_Addr  base         = _get_base_64(ehdr, elf_map);

    void*  bss_base = NULL;
    size_t bss_size = 0;

    if (base != UINT64_MAX) {
        log_d("ELF base @ %p", base);

        for (Elf64_Off sh_off = ehdr->e_shoff; sh_off < elf_map_size; sh_off += ehdr->e_shentsize) {
            p_shdr = (Elf64_Shdr*)(elf_map + sh_off);

            if (p_shdr->sh_type == SHT_DYNSYM && strcmp(sh_name_base + p_shdr->sh_name, ".dynsym") == 0) {
                p_dynsym = p_shdr;
            } else if (strcmp(sh_name_base + p_shdr->sh_name, ".bss") == 0) {
                bss_base = elf_base + (p_shdr->sh_addr - base);
                bss_size = p_shdr->sh_size;
            } else if (strcmp(sh_name_base + p_shdr->sh_name, ".PyRuntime") == 0) {
                self->map.runtime.base = elf_base + (p_shdr->sh_addr - base);
                self->map.runtime.size = p_shdr->sh_size;
            }
        }

        if (isvalid(p_dynsym)) {
            if (p_dynsym->sh_offset != 0) {
                Elf64_Shdr* p_strtabsh = (Elf64_Shdr*)(elf_map + ELF_SH_OFF(ehdr, p_dynsym->sh_link));

                // Search for dynamic symbols
                for (Elf64_Off tab_off  = p_dynsym->sh_offset; tab_off < p_dynsym->sh_offset + p_dynsym->sh_size;
                     tab_off           += p_dynsym->sh_entsize) {
                    Elf64_Sym* sym      = (Elf64_Sym*)(elf_map + tab_off);
                    char*      sym_name = (char*)(elf_map + p_strtabsh->sh_offset + sym->st_name);
                    void*      value    = elf_base + (sym->st_value - base);
                    if ((symbols += _py_proc__check_sym(self, sym_name, value)) >= DYNSYM_COUNT) {
                        // We have found all the symbols. No need to look further
                        break;
                    }
                }
            }
        }
    }

    if (symbols < DYNSYM_MANDATORY) {
        set_error(BINARY, "Not all required symbols found");
        FAIL;
    }

    // Communicate BSS data back to the caller
    bss->base = bss_base;
    bss->size = bss_size;
    log_d("BSS @ %p (size %x, offset %x)", bss_base, bss_size, bss_base - elf_base);

    SUCCESS;
} /* _py_proc__analyze_elf64 */
