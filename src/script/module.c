/* Script modules: the game's compiled scripts, loaded at runtime.
 *
 * The editor compiles Assets/Scripts with its built-in C compiler into one relocatable ELF object
 * (scripts.lxo). This file loads such an object into memory, links it against the engine API and
 * a small C library (script_api.c), and runs it. No system compiler, linker or dynamic loader is
 * involved, so the same object format works on Windows, Linux and Android (x86_64 and arm64). */
#include "../lx_internal.h"
#include <string.h>
#include <stdio.h>

#if LX_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define LX_MODULE_MACHINE 62 /* EM_X86_64 */
#elif defined(__aarch64__) || defined(_M_ARM64)
#define LX_MODULE_MACHINE 183 /* EM_AARCH64 */
#else
#define LX_MODULE_MACHINE 0
#endif

/* ---- the bits of ELF64 we need ---- */
typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf64Ehdr;
typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} Elf64Shdr;
typedef struct {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} Elf64Sym;
typedef struct {
    uint64_t offset, info;
    int64_t addend;
} Elf64Rela;

enum { SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOBITS = 8, SHT_INIT_ARRAY = 14 };
enum { SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4 };
enum { SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2 };
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };

struct LxModule {
    uint8_t *mem;
    size_t size, exec_size;
    const uint8_t *file;    /* only during loading */
    size_t file_size;
    Elf64Shdr *sh;
    int shnum;
    uint64_t *sec_addr;     /* load address of each section (0 = not loaded) */
    const Elf64Sym *syms;
    int nsyms;
    const char *strtab;
    size_t strtab_size;
    uint64_t *sym_addr;
    uint8_t *got, *stubs;
    char *err;
    size_t err_cap;
    /* exported symbols, kept after loading */
    char **names;
    uint64_t *addrs;
    int count;
};

static size_t page_size(void) {
#if LX_OS_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? si.dwPageSize : 4096;
#else
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096;
#endif
}

static uint8_t *mem_alloc(size_t size) {
#if LX_OS_WINDOWS
    return (uint8_t *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : (uint8_t *)p;
#endif
}

static bool mem_make_executable(uint8_t *p, size_t size) {
    if (!size) return true;
#if LX_OS_WINDOWS
    DWORD old;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), p, size);
    return true;
#else
#if defined(__aarch64__) || defined(__arm__)
    __builtin___clear_cache((char *)p, (char *)p + size);
#endif
    return mprotect(p, size, PROT_READ | PROT_EXEC) == 0;
#endif
}

static void mem_free(uint8_t *p, size_t size) {
    if (!p) return;
#if LX_OS_WINDOWS
    LX_UNUSED(size);
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

static bool fail(LxModule *m, const char *fmt, const char *arg) {
    if (m->err && m->err_cap) snprintf(m->err, m->err_cap, fmt, arg ? arg : "");
    return false;
}

static size_t align_up(size_t v, size_t a) { return a > 1 ? (v + a - 1) / a * a : v; }

static const char *sym_name(LxModule *m, const Elf64Sym *s) {
    return s->name < m->strtab_size ? m->strtab + s->name : "";
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* A jump to an absolute address, for calls into the engine (which may be far away). */
static uint64_t make_stub(LxModule *m, int sym) {
    uint8_t *s = m->stubs + (size_t)sym * 16;
    uint64_t target = m->sym_addr[sym];
#if LX_MODULE_MACHINE == 62
    static const uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0}; /* jmp [rip+0] */
    memcpy(s, jmp, 6);
    put64(s + 6, target);
#elif LX_MODULE_MACHINE == 183
    put32(s, 0x58000050u);     /* ldr x16, #8 */
    put32(s + 4, 0xd61f0200u); /* br x16 */
    put64(s + 8, target);
#endif
    return (uint64_t)(uintptr_t)s;
}

static uint64_t got_slot(LxModule *m, int sym) {
    uint8_t *g = m->got + (size_t)sym * 8;
    put64(g, m->sym_addr[sym]);
    return (uint64_t)(uintptr_t)g;
}

static bool fits_i32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

static bool apply(LxModule *m, uint8_t *p, uint32_t type, int sym, int64_t addend) {
    uint64_t P = (uint64_t)(uintptr_t)p;
    uint64_t S = sym ? m->sym_addr[sym] : 0;
    bool external = sym && m->syms[sym].shndx == SHN_UNDEF;
    const char *name = sym ? sym_name(m, &m->syms[sym]) : "";
#if LX_MODULE_MACHINE == 62
    switch (type) {
    case 0: return true;                                   /* R_X86_64_NONE */
    case 1: put64(p, S + (uint64_t)addend); return true;    /* R_X86_64_64 */
    case 24: put64(p, S + (uint64_t)addend - P); return true; /* R_X86_64_PC64 */
    case 2: case 4: {                                      /* R_X86_64_PC32, R_X86_64_PLT32 */
        if (external && S) S = make_stub(m, sym);
        int64_t v = (int64_t)(S + (uint64_t)addend - P);
        if (!fits_i32(v)) return fail(m, "Reference to '%s' is out of range", name);
        put32(p, (uint32_t)(int32_t)v);
        return true;
    }
    case 9: case 41: case 42: {                            /* GOTPCREL, GOTPCRELX, REX_GOTPCRELX */
        int64_t v = (int64_t)(got_slot(m, sym) + (uint64_t)addend - P);
        if (!fits_i32(v)) return fail(m, "GOT reference to '%s' is out of range", name);
        put32(p, (uint32_t)(int32_t)v);
        return true;
    }
    case 10: case 11: {                                    /* R_X86_64_32, R_X86_64_32S */
        uint64_t v = S + (uint64_t)addend;
        if (type == 10 ? v > UINT32_MAX : !fits_i32((int64_t)v)) return fail(m, "Absolute 32-bit reference to '%s'", name);
        put32(p, (uint32_t)v);
        return true;
    }
    }
#elif LX_MODULE_MACHINE == 183
    uint32_t ins = get32(p);
    switch (type) {
    case 0: case 256: return true;
    case 257: put64(p, S + (uint64_t)addend); return true;              /* ABS64 */
    case 258: put32(p, (uint32_t)(S + (uint64_t)addend)); return true;  /* ABS32 */
    case 260: put64(p, S + (uint64_t)addend - P); return true;          /* PREL64 */
    case 261: put32(p, (uint32_t)(S + (uint64_t)addend - P)); return true; /* PREL32 */
    case 282: case 283: {                                               /* JUMP26, CALL26 */
        int64_t off = (int64_t)(S + (uint64_t)addend - P);
        if (external && S && (off < -(1 << 27) || off >= (1 << 27))) off = (int64_t)(make_stub(m, sym) + (uint64_t)addend - P);
        if (off < -(1 << 27) || off >= (1 << 27)) return fail(m, "Call to '%s' is out of range", name);
        put32(p, (ins & 0xfc000000u) | ((uint32_t)(off >> 2) & 0x03ffffffu));
        return true;
    }
    case 275: case 276: case 311: {                       /* ADR_PREL_PG_HI21(_NC), ADR_GOT_PAGE */
        uint64_t target = type == 311 ? got_slot(m, sym) : S + (uint64_t)addend;
        int64_t pages = (int64_t)(target >> 12) - (int64_t)(P >> 12);
        if (type != 311 && external && (pages < -(1 << 20) || pages >= (1 << 20))) {
            target = make_stub(m, sym) + (uint64_t)addend;   /* a function's address: use its stub */
            pages = (int64_t)(target >> 12) - (int64_t)(P >> 12);
        }
        if (pages < -(1 << 20) || pages >= (1 << 20)) return fail(m, "Page reference to '%s' is out of range", name);
        uint32_t imm = (uint32_t)pages & 0x1fffffu;
        put32(p, (ins & 0x9f00001fu) | ((imm & 3) << 29) | ((imm >> 2) << 5));
        return true;
    }
    case 277: case 278: case 284: case 285: case 286: case 299: case 312: {
        uint64_t target = type == 312 ? got_slot(m, sym) : S + (uint64_t)addend;
        if (type != 312 && external) {
            /* pairs with an ADR_PREL_PG_HI21 that may have switched to the stub */
            int64_t pages = (int64_t)(target >> 12) - (int64_t)(P >> 12);
            if (pages < -(1 << 20) || pages >= (1 << 20)) target = make_stub(m, sym) + (uint64_t)addend;
        }
        uint32_t lo = (uint32_t)(target & 0xfff);
        int shift = type == 284 ? 1 : type == 285 ? 2 : (type == 286 || type == 312) ? 3 : type == 299 ? 4 : 0;
        put32(p, (ins & 0xffc003ffu) | (((lo >> shift) & 0xfffu) << 10));
        return true;
    }
    case 263: case 264: case 265: case 266: case 267: case 268: case 269: { /* MOVW_UABS_G0..G3 */
        int part = (int)(type - 263 + 1) / 2;
        uint64_t v = S + (uint64_t)addend;
        put32(p, (ins & 0xffe0001fu) | ((uint32_t)((v >> (16 * part)) & 0xffff) << 5));
        return true;
    }
    }
    LX_UNUSED(ins);
#else
    LX_UNUSED(P); LX_UNUSED(S); LX_UNUSED(external); LX_UNUSED(p); LX_UNUSED(addend);
#endif
    char what[96];
    snprintf(what, sizeof what, "%u (symbol '%.60s')", type, name);
    return fail(m, "Unsupported relocation type %s", what);
}

static bool load(LxModule *m) {
    const Elf64Ehdr *eh = (const Elf64Ehdr *)m->file;
    if (m->file_size < sizeof *eh || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1)
        return fail(m, "Not a compiled script module%s", NULL);
    if (eh->type != 1) return fail(m, "Not a relocatable object%s", NULL);
    if (!LX_MODULE_MACHINE || eh->machine != LX_MODULE_MACHINE)
        return fail(m, "The scripts were compiled for a different CPU - rebuild the game%s", NULL);
    if (eh->shentsize != sizeof(Elf64Shdr) || eh->shoff > m->file_size ||
        (uint64_t)eh->shnum * sizeof(Elf64Shdr) > m->file_size - eh->shoff)
        return fail(m, "Damaged script module%s", NULL);
    m->shnum = eh->shnum;
    m->sh = lx_calloc((size_t)m->shnum, sizeof(Elf64Shdr));
    memcpy(m->sh, m->file + eh->shoff, (size_t)m->shnum * sizeof(Elf64Shdr));
    m->sec_addr = lx_calloc((size_t)m->shnum, sizeof(uint64_t));
    for (int i = 0; i < m->shnum; i++) {
        const Elf64Shdr *s = &m->sh[i];
        if (s->type != SHT_NOBITS && s->type != 0 && (s->offset > m->file_size || s->size > m->file_size - s->offset))
            return fail(m, "Damaged script module%s", NULL);
        if (s->type == SHT_SYMTAB) {
            if (s->entsize != sizeof(Elf64Sym) || s->link >= (uint32_t)m->shnum) return fail(m, "Damaged symbol table%s", NULL);
            m->syms = (const Elf64Sym *)(m->file + s->offset);
            m->nsyms = (int)(s->size / sizeof(Elf64Sym));
            const Elf64Shdr *st = &m->sh[s->link];
            if (st->offset > m->file_size || st->size > m->file_size - st->offset) return fail(m, "Damaged string table%s", NULL);
            m->strtab = (const char *)(m->file + st->offset);
            m->strtab_size = st->size;
            if (!st->size || m->strtab[st->size - 1]) return fail(m, "Damaged string table%s", NULL);
        }
    }
    if (!m->syms) return fail(m, "The script module has no symbols%s", NULL);

    /* layout: [code][stubs] | page | [data][got][common] */
    size_t pg = page_size(), off = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            off = align_up(off, 16);
            off += (size_t)m->nsyms * 16; /* stubs */
            m->exec_size = off = align_up(off, pg);
        }
        for (int i = 0; i < m->shnum; i++) {
            const Elf64Shdr *s = &m->sh[i];
            if (!(s->flags & SHF_ALLOC) || !s->size) continue;
            if (((s->flags & SHF_EXECINSTR) != 0) != (pass == 0)) continue;
            off = align_up(off, s->addralign ? (size_t)s->addralign : 1);
            m->sec_addr[i] = off; /* relative for now */
            off += (size_t)s->size;
        }
    }
    off = align_up(off, 16);
    size_t got_off = off;
    off += (size_t)m->nsyms * 8;
    size_t common_off = off;
    for (int i = 0; i < m->nsyms; i++)
        if (m->syms[i].shndx == SHN_COMMON) off = align_up(off, m->syms[i].value ? (size_t)m->syms[i].value : 8) + (size_t)m->syms[i].size;
    m->size = align_up(off ? off : 1, pg);
    m->mem = mem_alloc(m->size);
    if (!m->mem) return fail(m, "Out of memory loading scripts%s", NULL);
    /* the stubs start right after the code */
    size_t code_end = 0;
    for (int i = 0; i < m->shnum; i++)
        if ((m->sh[i].flags & SHF_ALLOC) && (m->sh[i].flags & SHF_EXECINSTR) && m->sh[i].size)
            if (m->sec_addr[i] + m->sh[i].size > code_end) code_end = (size_t)(m->sec_addr[i] + m->sh[i].size);
    m->stubs = m->mem + align_up(code_end, 16);
    m->got = m->mem + got_off;
    for (int i = 0; i < m->shnum; i++) {
        const Elf64Shdr *s = &m->sh[i];
        if (!(s->flags & SHF_ALLOC) || !s->size) continue;
        uint8_t *dst = m->mem + m->sec_addr[i];
        if (s->type != SHT_NOBITS) memcpy(dst, m->file + s->offset, (size_t)s->size);
        m->sec_addr[i] = (uint64_t)(uintptr_t)dst;
    }

    /* symbols */
    m->sym_addr = lx_calloc((size_t)m->nsyms, sizeof(uint64_t));
    size_t common = common_off;
    for (int i = 1; i < m->nsyms; i++) {
        const Elf64Sym *s = &m->syms[i];
        int bind = s->info >> 4;
        if (s->shndx == SHN_UNDEF) {
            const char *name = sym_name(m, s);
            if (!name[0]) continue;
            const void *addr = lx_script_api_symbol(name);
            if (!addr && bind != STB_WEAK) return fail(m, "Scripts use '%s', which isn't part of the Luxora script API", name);
            m->sym_addr[i] = (uint64_t)(uintptr_t)addr;
        } else if (s->shndx == SHN_ABS) {
            m->sym_addr[i] = s->value;
        } else if (s->shndx == SHN_COMMON) {
            common = align_up(common, s->value ? (size_t)s->value : 8);
            m->sym_addr[i] = (uint64_t)(uintptr_t)(m->mem + common);
            common += (size_t)s->size;
        } else if (s->shndx < m->shnum) {
            m->sym_addr[i] = m->sec_addr[s->shndx] ? m->sec_addr[s->shndx] + s->value : 0;
        }
    }

    /* relocations */
    for (int i = 0; i < m->shnum; i++) {
        const Elf64Shdr *rs = &m->sh[i];
        if (rs->type != SHT_RELA || rs->info >= (uint32_t)m->shnum || !m->sec_addr[rs->info]) continue;
        const Elf64Shdr *target = &m->sh[rs->info];
        size_t n = (size_t)(rs->size / sizeof(Elf64Rela));
        for (size_t r = 0; r < n; r++) {
            Elf64Rela rel;
            memcpy(&rel, m->file + rs->offset + r * sizeof rel, sizeof rel);
            int sym = (int)(rel.info >> 32);
            uint32_t type = (uint32_t)rel.info;
            if (sym >= m->nsyms || rel.offset + 4 > target->size) return fail(m, "Damaged relocation%s", NULL);
            if (!apply(m, (uint8_t *)(uintptr_t)(m->sec_addr[rs->info] + rel.offset), type, sym, rel.addend)) return false;
        }
    }
    if (!mem_make_executable(m->mem, m->exec_size)) return fail(m, "Could not make the scripts executable%s", NULL);

    /* exports */
    for (int i = 1; i < m->nsyms; i++) {
        const Elf64Sym *s = &m->syms[i];
        int bind = s->info >> 4;
        if (bind == STB_LOCAL || s->shndx == SHN_UNDEF || !sym_name(m, s)[0]) continue;
        m->names = lx_realloc(m->names, sizeof(char *) * (size_t)(m->count + 1));
        m->addrs = lx_realloc(m->addrs, sizeof(uint64_t) * (size_t)(m->count + 1));
        m->names[m->count] = lx_strdup(sym_name(m, s));
        m->addrs[m->count] = m->sym_addr[i];
        m->count++;
    }

    /* constructors */
    for (int i = 0; i < m->shnum; i++) {
        if (m->sh[i].type != SHT_INIT_ARRAY || !m->sec_addr[i]) continue;
        size_t n = (size_t)(m->sh[i].size / sizeof(void *));
        for (size_t k = 0; k < n; k++) {
            void (*fn)(void);
            memcpy(&fn, (void *)(uintptr_t)(m->sec_addr[i] + k * sizeof(void *)), sizeof fn);
            if (fn) fn();
        }
    }
    return true;
}

LxModule *lx_module_load(const void *data, size_t size, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    LxModule *m = lx_calloc(1, sizeof *m);
    m->file = data;
    m->file_size = size;
    m->err = err;
    m->err_cap = err_cap;
    bool ok = load(m);
    lx_free(m->sh);
    lx_free(m->sec_addr);
    lx_free(m->sym_addr);
    m->sh = NULL; m->sec_addr = NULL; m->sym_addr = NULL;
    m->syms = NULL; m->strtab = NULL; m->file = NULL; m->err = NULL;
    if (!ok) { lx_module_unload(m); return NULL; }
    return m;
}

void *lx_module_symbol(const LxModule *m, const char *name) {
    if (!m) return NULL;
    for (int i = 0; i < m->count; i++)
        if (!strcmp(m->names[i], name)) return (void *)(uintptr_t)m->addrs[i];
    return NULL;
}

bool lx_module_contains(const LxModule *m, const void *p) {
    return m && m->mem && (const uint8_t *)p >= m->mem && (const uint8_t *)p < m->mem + m->size;
}

void lx_module_unload(LxModule *m) {
    if (!m) return;
    mem_free(m->mem, m->size);
    for (int i = 0; i < m->count; i++) lx_free(m->names[i]);
    lx_free(m->names);
    lx_free(m->addrs);
    lx_free(m);
}

/* ---- the game's scripts ---- */
static LxModule *g_scripts;

void lx_scripts_unload(void) {
    if (!g_scripts) return;
    lx_behaviours_drop_module(g_scripts);
    lx_module_unload(g_scripts);
    g_scripts = NULL;
    lx_register_builtin_behaviours(); /* in case a script had replaced one */
}

bool lx_scripts_load_memory(const void *data, size_t size, char *err, size_t err_cap) {
    lx_scripts_unload();
    LxModule *m = lx_module_load(data, size, err, err_cap);
    if (!m) return false;
    const char *abi = lx_module_symbol(m, "lx_module_abi");
    if (!abi || strcmp(abi, LX_MODULE_ABI)) {
        if (err && err_cap) snprintf(err, err_cap, "The scripts were compiled for %s, this is %s - rebuild the game",
                                     abi ? abi : "an unknown platform", LX_MODULE_ABI);
        lx_module_unload(m);
        return false;
    }
    void (*reg)(void) = (void (*)(void))(uintptr_t)lx_module_symbol(m, "lx_game_register_scripts");
    if (!reg) {
        if (err && err_cap) snprintf(err, err_cap, "The script module has no registration function");
        lx_module_unload(m);
        return false;
    }
    g_scripts = m;
    reg();
    return true;
}

bool lx_scripts_load(const char *path) {
    size_t size;
    void *data = lx_file_read(path, &size);
    if (!data) return false;
    char err[256];
    bool ok = lx_scripts_load_memory(data, size, err, sizeof err);
    lx_free(data);
    if (!ok) LX_ERROR("Could not load the game's scripts: %s", err);
    else LX_INFO("Loaded game scripts (%s)", lx_path_filename(path));
    return ok;
}

bool lx_scripts_loaded(void) { return g_scripts != NULL; }
