#ifndef ULIB_ELF_H
#define ULIB_ELF_H

// The ELF file format's types and constants -- the System V gABI and the
// x86-64 psABI, with glibc's names, so code that reads ELF (a loader, an
// unwinder, a debugger, <link.h>) compiles unchanged. Types and numbers
// only: nothing here is a function.
//
// The 32-bit types are here because ported code names them; SaunaOS
// runs only ELFCLASS64 / EM_X86_64 programs.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;
typedef uint64_t Elf32_Xword;
typedef int64_t  Elf32_Sxword;
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Section;
typedef uint16_t Elf32_Versym;

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Section;
typedef uint16_t Elf64_Versym;

// --- the file header --------------------------------------------------

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type, e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off  e_phoff, e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type, e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff, e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6
#define EI_OSABI      7
#define EI_ABIVERSION 8
#define EI_PAD        9

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFMAG  "\177ELF"
#define SELFMAG 4

#define ELFCLASSNONE 0
#define ELFCLASS32   1
#define ELFCLASS64   2

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

#define EV_NONE    0
#define EV_CURRENT 1

#define ELFOSABI_NONE  0
#define ELFOSABI_SYSV  0
#define ELFOSABI_GNU   3
#define ELFOSABI_LINUX ELFOSABI_GNU

#define ET_NONE 0
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3
#define ET_CORE 4

#define EM_NONE   0
#define EM_386    3
#define EM_X86_64 62

// --- program headers --------------------------------------------------

typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr, p_paddr;
    Elf32_Word p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;

typedef struct {
    Elf64_Word  p_type, p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr, p_paddr;
    Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7
#define PT_LOOS         0x60000000
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552
#define PT_GNU_PROPERTY 0x6474e553
#define PT_HIOS         0x6fffffff

#define PF_X 1
#define PF_W 2
#define PF_R 4

// --- section headers --------------------------------------------------

typedef struct {
    Elf32_Word sh_name, sh_type, sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr;

typedef struct {
    Elf64_Word  sh_name, sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link, sh_info;
    Elf64_Xword sh_addralign, sh_entsize;
} Elf64_Shdr;

#define SHN_UNDEF  0
#define SHN_ABS    0xfff1
#define SHN_COMMON 0xfff2
#define SHN_XINDEX 0xffff

#define SHT_NULL          0
#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_STRTAB        3
#define SHT_RELA          4
#define SHT_HASH          5
#define SHT_DYNAMIC       6
#define SHT_NOTE          7
#define SHT_NOBITS        8
#define SHT_REL           9
#define SHT_DYNSYM        11
#define SHT_INIT_ARRAY    14
#define SHT_FINI_ARRAY    15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP         17
#define SHT_GNU_HASH      0x6ffffff6

#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_MERGE     0x10
#define SHF_STRINGS   0x20
#define SHF_TLS       0x400

// --- symbols ------------------------------------------------------------

typedef struct {
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info, st_other;
    Elf32_Section st_shndx;
} Elf32_Sym;

typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info, st_other;
    Elf64_Section st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

#define ELF32_ST_BIND(i)       ((i) >> 4)
#define ELF32_ST_TYPE(i)       ((i) & 0xf)
#define ELF32_ST_INFO(b, t)    (((b) << 4) + ((t) & 0xf))
#define ELF64_ST_BIND(i)       ELF32_ST_BIND(i)
#define ELF64_ST_TYPE(i)       ELF32_ST_TYPE(i)
#define ELF64_ST_INFO(b, t)    ELF32_ST_INFO((b), (t))
#define ELF32_ST_VISIBILITY(o) ((o) & 0x3)
#define ELF64_ST_VISIBILITY(o) ELF32_ST_VISIBILITY(o)

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4
#define STT_COMMON  5
#define STT_TLS     6

#define STV_DEFAULT   0
#define STV_INTERNAL  1
#define STV_HIDDEN    2
#define STV_PROTECTED 3

// --- relocations --------------------------------------------------------

typedef struct { Elf32_Addr r_offset; Elf32_Word r_info; } Elf32_Rel;
typedef struct { Elf32_Addr r_offset; Elf32_Word r_info; Elf32_Sword r_addend; } Elf32_Rela;
typedef struct { Elf64_Addr r_offset; Elf64_Xword r_info; } Elf64_Rel;
typedef struct { Elf64_Addr r_offset; Elf64_Xword r_info; Elf64_Sxword r_addend; } Elf64_Rela;

#define ELF32_R_SYM(i)     ((i) >> 8)
#define ELF32_R_TYPE(i)    ((i) & 0xff)
#define ELF32_R_INFO(s, t) (((s) << 8) + ((t) & 0xff))
#define ELF64_R_SYM(i)     ((i) >> 32)
#define ELF64_R_TYPE(i)    ((i) & 0xffffffff)
#define ELF64_R_INFO(s, t) ((((Elf64_Xword)(s)) << 32) + (t))

#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_PC32      2
#define R_X86_64_GOT32     3
#define R_X86_64_PLT32     4
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define R_X86_64_GOTPCREL  9
#define R_X86_64_32        10
#define R_X86_64_32S       11
#define R_X86_64_DTPMOD64  16
#define R_X86_64_DTPOFF64  17
#define R_X86_64_TPOFF64   18
#define R_X86_64_IRELATIVE 37

// --- the dynamic section ------------------------------------------------

typedef struct { Elf32_Sword d_tag; union { Elf32_Word d_val; Elf32_Addr d_ptr; } d_un; } Elf32_Dyn;
typedef struct { Elf64_Sxword d_tag; union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un; } Elf64_Dyn;

#define DT_NULL            0
#define DT_NEEDED          1
#define DT_PLTRELSZ        2
#define DT_PLTGOT          3
#define DT_HASH            4
#define DT_STRTAB          5
#define DT_SYMTAB          6
#define DT_RELA            7
#define DT_RELASZ          8
#define DT_RELAENT         9
#define DT_STRSZ           10
#define DT_SYMENT          11
#define DT_INIT            12
#define DT_FINI            13
#define DT_SONAME          14
#define DT_RPATH           15
#define DT_SYMBOLIC        16
#define DT_REL             17
#define DT_RELSZ           18
#define DT_RELENT          19
#define DT_PLTREL          20
#define DT_DEBUG           21
#define DT_TEXTREL         22
#define DT_JMPREL          23
#define DT_BIND_NOW        24
#define DT_INIT_ARRAY      25
#define DT_FINI_ARRAY      26
#define DT_INIT_ARRAYSZ    27
#define DT_FINI_ARRAYSZ    28
#define DT_RUNPATH         29
#define DT_FLAGS           30
#define DT_PREINIT_ARRAY   32
#define DT_PREINIT_ARRAYSZ 33
#define DT_GNU_HASH        0x6ffffef5
#define DT_FLAGS_1         0x6ffffffb

// --- notes and the auxiliary vector -------------------------------------

typedef struct { Elf32_Word n_namesz, n_descsz, n_type; } Elf32_Nhdr;
typedef struct { Elf64_Word n_namesz, n_descsz, n_type; } Elf64_Nhdr;

#define NT_GNU_ABI_TAG      1
#define NT_GNU_BUILD_ID     3
#define NT_GNU_PROPERTY_TYPE_0 5

// What the kernel places after envp (abi/auxv.h has which of these
// SaunaOS passes, and to which programs).
typedef struct { uint64_t a_type; union { uint64_t a_val; } a_un; } Elf64_auxv_t;

#define AT_NULL   0
#define AT_IGNORE 1
#define AT_EXECFD 2
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9

#ifdef __cplusplus
}
#endif

#endif
