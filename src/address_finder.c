
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <elf.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "address_finder.h"

// Resolve the file offset (st_value) of a symbol in an ELF's .dynsym.
// Returns 0 on failure.
static uintptr_t find_symbol_offset(const char *lib_path, const char *sym_name) {
    printf("[*] Opening: %s\n", lib_path);

    int fd = open(lib_path, O_RDONLY);
    if (fd < 0) {
        printf("[-] open() failed for %s\n", lib_path);
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        printf("[-] fstat() failed\n");
        close(fd);
        return 0;
    }
    printf("[*] File size: %ld bytes\n", (long) st.st_size);

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        printf("[-] mmap() failed\n");
        return 0;
    }

    unsigned char *base = (unsigned char *) map;
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *) base;

    // sanity: ELF magic
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        printf("[-] Not an ELF file\n");
        munmap(map, st.st_size);
        return 0;
    }
    printf("[*] ELF OK. Section headers at offset %lu, count %d\n",
           (unsigned long) ehdr->e_shoff, ehdr->e_shnum);

    Elf64_Shdr *shdrs = (Elf64_Shdr *) (base + ehdr->e_shoff);

    // find .dynsym and its linked .dynstr
    Elf64_Shdr *dynsym = NULL, *dynstr = NULL;
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM) {
            dynsym = &shdrs[i];
            // sh_link points to the associated string table section
            dynstr = &shdrs[shdrs[i].sh_link];
            break;
        }
    }
    if (!dynsym || !dynstr) {
        printf("[-] .dynsym/.dynstr not found\n");
        munmap(map, st.st_size);
        return 0;
    }
    printf("[*] .dynsym found: offset=%lu size=%lu entsize=%lu\n",
           (unsigned long) dynsym->sh_offset,
           (unsigned long) dynsym->sh_size,
           (unsigned long) dynsym->sh_entsize);

    Elf64_Sym *syms = (Elf64_Sym *) (base + dynsym->sh_offset);
    const char *strtab = (const char *) (base + dynstr->sh_offset);
    int count = dynsym->sh_size / dynsym->sh_entsize;
    printf("[*] Scanning %d symbols for '%s'...\n", count, sym_name);

    unsigned long result = 0;
    for (int i = 0; i < count; i++) {
        const char *name = strtab + syms[i].st_name;
        if (strcmp(name, sym_name) == 0) {
            result = (unsigned long) syms[i].st_value;
            printf("[+] FOUND '%s' -> st_value (offset) = 0x%lx (%lu)\n",
                   sym_name, result, result);
            break;
        }
    }
    if (!result) {
        printf("[-] Symbol '%s' not found in .dynsym\n", sym_name);
    }

    munmap(map, st.st_size);
    return result;
}

uintptr_t find_created_vms_offset() {
    const char *libart_path = "/apex/com.android.art/lib64/libart.so";
    printf("\n--- libart / JNI_GetCreatedJavaVMs ---\n");
    uintptr_t vms_off = find_symbol_offset(libart_path, "JNI_GetCreatedJavaVMs");
    printf("libart VMs off : 0x%lx\n", vms_off);
    return vms_off;
}

uintptr_t find_dlopen_offset() {
    const char *linker_path = "/system/bin/linker64";
    printf("--- linker64 / dlopen ---\n");
    uintptr_t dlopen_off = find_symbol_offset(linker_path, "__loader_dlopen");
    if (!dlopen_off) {
        printf("[*] retrying with '__loader_android_dlopen_ext'\n");
        dlopen_off = find_symbol_offset(linker_path, "__loader_android_dlopen_ext");
        printf("\n=== RESULTS ===\n");
        printf("dlopen offset  : 0x%lx\n", dlopen_off);
    }

    return dlopen_off;
}


uintptr_t find_module_base(int pid, const char *module_name) {
    char maps_path[64];

    if (pid == 0) {
        snprintf(maps_path, sizeof(maps_path), "/proc/self/maps");
    } else {
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    }

    FILE *fp = fopen(maps_path, "r");
    if (!fp) {
        perror("fopen maps");
        return 0;
    }

    char line[512];
    uintptr_t base = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, module_name)) {
            uintptr_t start = strtoull(line, NULL, 16);
            if (base == 0 || start < base) {
                base = start;
            }
        }
    }

    fclose(fp);
    return base;
}