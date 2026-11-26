/* SPDX-License-Identifier: GPL-2.0-only */
#include "fs_installer.h"
#include "fs_discover.h"
#include "runtime_templates.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>

#define CAVE_MAIN     0x24d100ULL
#define CAVE_CTRL     0x24d800ULL
#define CAVE_PAGE     0x24d000ULL
#define NOTIFIER_OFF  0x1201b0ULL
#define GATEWAY_MAX   1024u

static inline uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static inline void put_u32(uint8_t* b, size_t off, uint32_t v) {
    memcpy(b + off, &v, 4);
}

static inline void put_u64(uint8_t* b, size_t off, uint64_t v) {
    memcpy(b + off, &v, 8);
}

static inline uint32_t get_u32(const uint8_t* b, size_t off) {
    uint32_t v = 0;
    memcpy(&v, b + off, 4);
    return v;
}

static uint32_t encode_b(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)(to - from);
    return (uint32_t)(0x14000000u | (uint32_t)((d / 4) & 0x3ffffff));
}

static int64_t branch_target(uint32_t word, uint64_t at, int bits, int shift) {
    int64_t n = (int64_t)((word >> shift) & ((1u << bits) - 1u));
    if (n >= (1LL << (bits - 1))) n -= (1LL << bits);
    return (int64_t)at + 4 * n;
}

static void ilog(FILE* f, const char* fmt, ...) {
    if (!f) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    fprintf(f, "\n");
    fflush(f);
    va_end(a);
}

static void ifail(FILE* f, char* err, size_t err_len, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(err, err_len, fmt, a);
    va_end(a);
    ilog(f, "[ERROR] %s", err);
}

/* --- Temporary notifier bridge (exact C port of bridge.cjs) --- */

typedef struct {
    VesperKaihenCtx* kh;
    VesperFsDiscovery* disc;
    FILE* logf;
    uint64_t cave_off;
    uint64_t cave_addr;
    uint8_t orig_notifier[16];
    uint8_t hook_notifier[16];
    uint8_t cave_prev[GATEWAY_MAX];
    size_t cave_len;
    bool hooked;
    bool page_readable;
} FsBridge;

static Result probe_notifier_cmd0(Handle notifier) {
    Service s = {0};
    serviceCreate(&s, notifier);
    return serviceDispatch(&s, 0);
}

static Result bridge_stage(FsBridge* b, const uint8_t* bytes, size_t len) {
    if (len > b->cave_len) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    uint8_t next[GATEWAY_MAX];
    memset(next, 0, b->cave_len);
    if (bytes && len) memcpy(next, bytes, len);
    for (size_t i = 0; i < b->cave_len; i += 16) {
        if (memcmp(next + i, b->cave_prev + i, 16) != 0) {
            Result rc = vesper_fs_patch(b->kh, b->cave_addr + i, b->cave_prev + i, next + i, 16);
            if (R_FAILED(rc)) return rc;
        }
    }
    memcpy(b->cave_prev, next, b->cave_len);
    return 0;
}

static Result bridge_detach(FsBridge* b) {
    if (!b->hooked) return 0;
    Result rc = vesper_fs_patch(b->kh, b->disc->fs_base + NOTIFIER_OFF, b->hook_notifier, b->orig_notifier, 16);
    if (R_SUCCEEDED(rc)) b->hooked = false;
    return rc;
}

static Result bridge_switch_to_ctrl_cave(FsBridge* b) {
    Result rc = bridge_stage(b, NULL, 0);
    if (R_FAILED(rc)) return rc;
    uint8_t zeros[256] = {0}, cur[256] = {0};
    rc = vesper_fs_read(b->kh, b->disc->fs_base + CAVE_CTRL, cur, sizeof(cur));
    if (R_FAILED(rc) || memcmp(cur, zeros, sizeof(cur)) != 0) return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    b->cave_off = CAVE_CTRL;
    b->cave_addr = b->disc->fs_base + CAVE_CTRL;
    b->cave_len = 256;
    memset(b->cave_prev, 0, sizeof(b->cave_prev));
    put_u32(b->hook_notifier, 0, encode_b(b->disc->fs_base + NOTIFIER_OFF, b->cave_addr));
    for (int i = 4; i < 16; i += 4) put_u32(b->hook_notifier, i, 0xd503201fu);
    return 0;
}

static Result bridge_invoke(FsBridge* b, const uint64_t* args, int argc, uint64_t op_addr,
                            bool is_publish, bool clear_alloc, uint64_t* out_res) {
    const uint8_t* tmpl = NULL;
    size_t tsize = 0;
    size_t off_owner = 0, off_op = 0, off_res = 0, off_pass = 0;
    size_t off_args[6] = {0};

    if (is_publish) {
        tmpl = VESPER_STUB_PUBLISH; tsize = VESPER_STUB_PUBLISH_SIZE;
        off_owner = VESPER_STUB_PUBLISH_OWNER; off_op = VESPER_STUB_PUBLISH_OPERATION;
        off_res = VESPER_STUB_PUBLISH_RESULT; off_pass = VESPER_STUB_PUBLISH_PASSTHROUGH;
        off_args[0] = VESPER_STUB_PUBLISH_ARG0; off_args[1] = VESPER_STUB_PUBLISH_ARG1;
    } else if (clear_alloc) {
        tmpl = VESPER_STUB_ALLOCATE; tsize = VESPER_STUB_ALLOCATE_SIZE;
        off_owner = VESPER_STUB_ALLOCATE_OWNER; off_op = VESPER_STUB_ALLOCATE_OPERATION;
        off_res = VESPER_STUB_ALLOCATE_RESULT; off_pass = VESPER_STUB_ALLOCATE_PASSTHROUGH;
        off_args[0] = VESPER_STUB_ALLOCATE_ARG0;
    } else {
        switch (argc) {
            case 0: tmpl=VESPER_STUB_CALL0; tsize=VESPER_STUB_CALL0_SIZE; off_owner=VESPER_STUB_CALL0_OWNER; off_op=VESPER_STUB_CALL0_OPERATION; off_res=VESPER_STUB_CALL0_RESULT; off_pass=VESPER_STUB_CALL0_PASSTHROUGH; break;
            case 1: tmpl=VESPER_STUB_CALL1; tsize=VESPER_STUB_CALL1_SIZE; off_owner=VESPER_STUB_CALL1_OWNER; off_op=VESPER_STUB_CALL1_OPERATION; off_res=VESPER_STUB_CALL1_RESULT; off_pass=VESPER_STUB_CALL1_PASSTHROUGH; off_args[0]=VESPER_STUB_CALL1_ARG0; break;
            case 2: tmpl=VESPER_STUB_CALL2; tsize=VESPER_STUB_CALL2_SIZE; off_owner=VESPER_STUB_CALL2_OWNER; off_op=VESPER_STUB_CALL2_OPERATION; off_res=VESPER_STUB_CALL2_RESULT; off_pass=VESPER_STUB_CALL2_PASSTHROUGH; off_args[0]=VESPER_STUB_CALL2_ARG0; off_args[1]=VESPER_STUB_CALL2_ARG1; break;
            case 3: tmpl=VESPER_STUB_CALL3; tsize=VESPER_STUB_CALL3_SIZE; off_owner=VESPER_STUB_CALL3_OWNER; off_op=VESPER_STUB_CALL3_OPERATION; off_res=VESPER_STUB_CALL3_RESULT; off_pass=VESPER_STUB_CALL3_PASSTHROUGH; off_args[0]=VESPER_STUB_CALL3_ARG0; off_args[1]=VESPER_STUB_CALL3_ARG1; off_args[2]=VESPER_STUB_CALL3_ARG2; break;
            default: return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
    }

    uint8_t stub[256];
    memcpy(stub, tmpl, tsize);
    uint64_t res_addr = b->disc->notifier_E + 8ULL;
    put_u64(stub, off_owner, b->disc->notifier_E);
    put_u64(stub, off_op, op_addr);
    put_u64(stub, off_res, res_addr);
    for (int i = 0; i < argc; i++) put_u64(stub, off_args[i], args[i]);
    /* Patch passthrough B instruction to original_tail = fs_base + NOTIFIER_OFF + 16 */
    put_u32(stub, off_pass, encode_b(b->cave_addr + off_pass, b->disc->fs_base + NOTIFIER_OFF + 16ULL));

    Result rc = bridge_stage(b, stub, tsize);
    if (R_FAILED(rc)) return rc;

    rc = vesper_fs_patch(b->kh, b->disc->fs_base + NOTIFIER_OFF, b->orig_notifier, b->hook_notifier, 16);
    if (R_FAILED(rc)) return rc;
    b->hooked = true;

    Result probe_rc = probe_notifier_cmd0(b->disc->notifier_handle);
    uint64_t val = 0;
    vesper_fs_read(b->kh, res_addr, &val, 8);
    if (val != b->disc->notifier_B) {
        vesper_fs_write(b->kh, res_addr, &val, &b->disc->notifier_B, 8);
    }
    Result det_rc = bridge_detach(b);
    if (R_FAILED(det_rc)) return det_rc;
    if (probe_rc != 0x6dfeu) {
        ilog(b->logf, "probe_notifier_cmd0 returned 0x%x (expected 0x6dfe)", probe_rc);
        return probe_rc ? probe_rc : MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    if (out_res) *out_res = val;
    return 0;
}

/* Copies `len` bytes from local buffer `src` into FS Normal memory `dst_normal`
 * using a 4 KB CodeMutable staging buffer `stage_cm` and FS's memcpy via lfs_copy
 * or our inline 4-instruction copy loop. */
static Result copy_to_normal_fs(FsBridge* b, uint64_t stage_cm, uint64_t dst_normal,
                                const uint8_t* src, size_t len) {
    /* First zero-read stage_cm baseline (we know its current bytes locally) */
    static uint8_t cur_stage[0x1000];
    Result rc = vesper_fs_read(b->kh, stage_cm, cur_stage, 0x1000);
    if (R_FAILED(rc)) return rc;

    /* Tiny AArch64 memcpy function staged at cave_addr + 0x100:
     *   void copy_fn(u64 dst=x0, u64 src=x1, u64 n=x2):
     *     0: cbz x2, +16
     *     4: ldr x3, [x1], #8
     *     8: str x3, [x0], #8
     *    12: subs x2, x2, #8
     *    16: b.ne -12
     *    20: mov x0, #0
     *    24: ret
     */
    static const uint32_t COPY_INSNS[8] = {
        0xb40000a2u, /* cbz x2, +20 */
        0xf8408423u, /* ldr x3, [x1], #8 */
        0xf8008403u, /* str x3, [x0], #8 */
        0xf1002042u, /* subs x2, x2, #8 */
        0x54ffffa1u, /* b.ne -12 */
        0xd2800000u, /* mov x0, #0 */
        0xd65f03c0u, /* ret */
        0xd503201fu, /* nop */
    };
    uint64_t copy_fn_addr = b->disc->fs_base + CAVE_MAIN + 0x300ULL;
    uint8_t zero32[32] = {0};
    rc = vesper_fs_patch(b->kh, copy_fn_addr, zero32, COPY_INSNS, 16);
    if (R_FAILED(rc)) return rc;
    rc = vesper_fs_patch(b->kh, copy_fn_addr + 16, zero32, ((const uint8_t*)COPY_INSNS) + 16, 16);
    if (R_FAILED(rc)) return rc;

    for (size_t off = 0; off < len; off += 0x1000) {
        size_t chunk = (len - off < 0x1000) ? (len - off) : 0x1000;
        rc = vesper_fs_write(b->kh, stage_cm, cur_stage, src + off, chunk);
        if (R_FAILED(rc)) break;
        memcpy(cur_stage, src + off, chunk);

        uint64_t args[3] = {dst_normal + off, stage_cm, (uint64_t)chunk};
        uint64_t res = 0;
        rc = bridge_invoke(b, args, 3, copy_fn_addr, false, false, &res);
        if (R_FAILED(rc)) break;
    }

    /* Clean up copy_fn_addr back to zeros */
    vesper_fs_patch(b->kh, copy_fn_addr, COPY_INSNS, zero32, 16);
    vesper_fs_patch(b->kh, copy_fn_addr + 16, ((const uint8_t*)COPY_INSNS) + 16, zero32, 16);
    return rc;
}

/* --- Gateway & Hook Builder (exact C port of native.cjs) --- */

typedef struct {
    uint64_t offset;
    uint8_t orig[16];
    uint8_t repl[16];
} FsHook;

typedef struct {
    uint8_t gateways[GATEWAY_MAX];
    size_t gateway_len;
    FsHook hooks[24];
    size_t hook_count;
    uint64_t orig_open, orig_dir, orig_type, orig_mount, orig_size;
    uint64_t orig_sdk[8];
    uint64_t orig_lazy[3];
} BuiltGateways;

static bool build_gateways(VesperKaihenCtx* kh, uint64_t fs_base, uint64_t rt_addr,
                           BuiltGateways* out, FILE* logf) {
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < GATEWAY_MAX; i += 4) put_u32(out->gateways, i, 0xd503201fu);
    size_t cur = 0;

    /* Helper: emit 16-byte gateway (ldr x16, +8; br x16; .quad target) */
    #define EMIT_GW(sym_off, dst_var) do { \
        uint64_t _at = cur; cur += 16; \
        put_u32(out->gateways, _at, 0x58000050u); \
        put_u32(out->gateways, _at + 4, 0xd61f0200u); \
        put_u64(out->gateways, _at + 8, rt_addr + (sym_off)); \
        (dst_var) = CAVE_MAIN + _at; \
    } while (0)

    uint64_t gw_read, gw_size, gw_open, gw_dir, gw_type, gw_mount, gw_range, gw_batch;
    EMIT_GW(VESPER_RT_OFF_LFS_READ_ENTRY, gw_read);
    EMIT_GW(VESPER_RT_OFF_LFS_SIZE_ENTRY, gw_size);
    EMIT_GW(VESPER_RT_OFF_LFS_NAMESPACE_OPEN_ENTRY, gw_open);
    EMIT_GW(VESPER_RT_OFF_LFS_NAMESPACE_DIRECTORY_ENTRY, gw_dir);
    EMIT_GW(VESPER_RT_OFF_LFS_NAMESPACE_TYPE_ENTRY, gw_type);
    EMIT_GW(VESPER_RT_OFF_LFS_NAMESPACE_MOUNT_ENTRY, gw_mount);
    EMIT_GW(VESPER_RT_OFF_LFS_STORAGE_RANGE, gw_range);
    EMIT_GW(VESPER_RT_OFF_LFS_STORAGE_BATCH, gw_batch);

    /* Helper: emit prologue replay for 16-byte displaced prologue at `off` */
    uint64_t replay_fn(uint64_t off) {
        uint8_t orig[16];
        if (R_FAILED(vesper_fs_read(kh, fs_base + off, orig, 16))) return 0;
        uint64_t at = cur;
        put_u32(out->gateways, cur, 0xd503245fu); cur += 4; /* bti c */
        for (int i = 0; i < 4; i++) {
            uint32_t w = get_u32(orig, i * 4);
            uint64_t from = off + i * 4;
            int bits = 0, shift = 5;
            uint32_t invert = 0, mask = 0;
            if ((w & 0x7e000000u) == 0x36000000u)      { bits = 14; invert = 0x01000000u; mask = 0x7ffe0u; }
            else if ((w & 0x7e000000u) == 0x34000000u) { bits = 19; invert = 0x01000000u; mask = 0xffffe0u; }
            else if ((w & 0xff000010u) == 0x54000000u) { bits = 19; invert = 1u;          mask = 0xffffe0u; }
            if (bits) {
                int64_t dst = branch_target(w, from, bits, shift);
                put_u32(out->gateways, cur, ((w ^ invert) & ~mask) | (2u << shift)); cur += 4;
                put_u32(out->gateways, cur, encode_b(fs_base + CAVE_MAIN + cur, fs_base + (uint64_t)dst)); cur += 4;
            } else {
                put_u32(out->gateways, cur, w); cur += 4;
            }
        }
        put_u32(out->gateways, cur, encode_b(fs_base + CAVE_MAIN + cur, fs_base + off + 16));
        cur = (size_t)align_up(cur + 4, 16);
        return CAVE_MAIN + at;
    }

    out->orig_open  = replay_fn(0x1045b0ULL);
    out->orig_dir   = replay_fn(0x1048c0ULL);
    out->orig_type  = replay_fn(0x1042e0ULL);
    out->orig_mount = replay_fn(0xefa00ULL);
    out->orig_size  = replay_fn(0x107600ULL);

    static const uint64_t SDK_OFFS[8] = {
        0x104ef0ULL, 0x105480ULL, 0x104e10ULL, 0x103390ULL,
        0x107490ULL, 0x105800ULL, 0x1058f0ULL, 0x105720ULL
    };
    static const uint32_t SDK_SYMS[8] = {
        VESPER_RT_OFF_LFS_FILE_READ, VESPER_RT_OFF_LFS_FILE_SIZE,
        VESPER_RT_OFF_LFS_FILE_RELEASE, VESPER_RT_OFF_LFS_FILESYSTEM_RELEASE,
        VESPER_RT_OFF_LFS_STORAGE_RELEASE, VESPER_RT_OFF_LFS_DIRECTORY_READ,
        VESPER_RT_OFF_LFS_DIRECTORY_COUNT, VESPER_RT_OFF_LFS_DIRECTORY_RELEASE
    };
    uint64_t gw_sdk[8];
    for (int i = 0; i < 8; i++) {
        EMIT_GW(SDK_SYMS[i], gw_sdk[i]);
        out->orig_sdk[i] = replay_fn(SDK_OFFS[i]);
    }

    static const uint64_t LAZY_OFFS[3] = {0xedb60ULL, 0xedbe0ULL, 0xedc60ULL};
    static const uint32_t LAZY_SYMS[3] = {
        VESPER_RT_OFF_LFS_LAZY_MOUNT_CURRENT,
        VESPER_RT_OFF_LFS_LAZY_MOUNT_PROGRAM,
        VESPER_RT_OFF_LFS_LAZY_MOUNT_DATA
    };
    uint64_t gw_lazy[3];
    for (int i = 0; i < 3; i++) {
        EMIT_GW(LAZY_SYMS[i], gw_lazy[i]);
        out->orig_lazy[i] = replay_fn(LAZY_OFFS[i]);
    }

    if (cur > GATEWAY_MAX) {
        ilog(logf, "Gateway overflow: %zu > %u", cur, GATEWAY_MAX);
        return false;
    }
    out->gateway_len = cur;

    /* Helper: register 16-byte hook */
    #define ADD_HOOK(off, dst) do { \
        FsHook* _h = &out->hooks[out->hook_count++]; \
        _h->offset = (off); \
        if (R_FAILED(vesper_fs_read(kh, fs_base + (off), _h->orig, 16))) return false; \
        put_u32(_h->repl, 0, 0xd503245fu); \
        put_u32(_h->repl, 4, encode_b(fs_base + (off) + 4, fs_base + (dst))); \
        put_u32(_h->repl, 8, 0xd503201fu); \
        put_u32(_h->repl, 12, 0xd503201fu); \
    } while (0)

    ADD_HOOK(0x1045b0ULL, gw_open);
    ADD_HOOK(0x1048c0ULL, gw_dir);
    ADD_HOOK(0x1042e0ULL, gw_type);
    ADD_HOOK(0xefa00ULL,  gw_mount);
    ADD_HOOK(0x107570ULL, gw_read);
    ADD_HOOK(0x107600ULL, gw_size);
    ADD_HOOK(0x107650ULL, gw_range);
    ADD_HOOK(0x11fed0ULL, gw_batch);
    for (int i = 0; i < 8; i++) ADD_HOOK(SDK_OFFS[i], gw_sdk[i]);
    for (int i = 0; i < 3; i++) ADD_HOOK(LAZY_OFFS[i], gw_lazy[i]);

    #undef EMIT_GW
    #undef ADD_HOOK
    return true;
}

/* --- NS2LFS02 Context Builder & Hot-Swap Region (exact C port of lazy-context.cjs) --- */

#define CTX_TOTAL_SIZE   0x10000ULL  /* 64 KiB CodeMutable RW */
#define CTX_HEADER_OFF   0u
#define CTX_NS_OFF       368u
#define CTX_LAZY_OFF     480u
#define CTX_GAMES_OFF    680u        /* 480 + 200 */
#define CTX_HOTSWAP_MAX  0x4000u     /* 16 KiB reserved for games[0] + files[] + strings */

#define MAX_HOTSWAP_RECS 160

typedef struct {
    char name[VESPER_MAX_PATH];
    char sd_path[VESPER_MAX_PATH];
    bool has_sd_path;
    uint64_t size;
    uint32_t kind; /* 0=dir, 1=romfs file, 2=exefs file, 3=exefs stub, 4=romfs.bin, 5=exefs.nsp, 6=ips */
} HotswapRec;

static bool collect_loose_romfs(const char* mount_dir, const char* rel_sd_dir, const char* vpath,
                                HotswapRec* recs, size_t* rec_count, size_t max_recs) {
    DIR* d = opendir(mount_dir);
    if (!d) return false;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (*rec_count >= max_recs) { closedir(d); return false; }

        char cm[VESPER_MAX_PATH], cs[VESPER_MAX_PATH], cv[VESPER_MAX_PATH];
        snprintf(cm, sizeof(cm), "%s/%s", mount_dir, ent->d_name);
        snprintf(cs, sizeof(cs), "%s/%s", rel_sd_dir, ent->d_name);
        snprintf(cv, sizeof(cv), "%s/%s", vpath, ent->d_name);

        struct stat st;
        if (stat(cm, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            HotswapRec* r = &recs[(*rec_count)++];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "%s", cv);
            r->has_sd_path = false;
            r->kind = 0; /* LAZY_DIRECTORY */
            if (!collect_loose_romfs(cm, cs, cv, recs, rec_count, max_recs)) {
                closedir(d);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            HotswapRec* r = &recs[(*rec_count)++];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "%s", cv);
            snprintf(r->sd_path, sizeof(r->sd_path), "%s", cs);
            r->has_sd_path = true;
            r->size = (uint64_t)st.st_size;
            r->kind = 1; /* LAZY_ROMFS */
        }
    }
    closedir(d);
    return true;
}

static bool exefs_has_module(const VesperProfile* prof, const char* name) {
    for (size_t i = 0; i < prof->exefs_file_count; i++) {
        if (strcmp(prof->exefs_files[i].name, name) == 0) return true;
    }
    return false;
}

static bool build_hotswap_region(uint64_t host_tid, const VesperProfile* prof,
                                 uint64_t ctx_addr, uint8_t* region_out, size_t region_cap,
                                 size_t* out_used) {
    HotswapRec* recs = (HotswapRec*)calloc(MAX_HOTSWAP_RECS, sizeof(HotswapRec));
    if (!recs) return false;
    size_t rec_count = 0;

    /* 1. Put records WITH sd_path first so files[0].source.path identifies the active profile */
    if (prof->has_romfs_bin && rec_count < MAX_HOTSWAP_RECS) {
        HotswapRec* r = &recs[rec_count++];
        snprintf(r->name, sizeof(r->name), "romfs.bin");
        snprintf(r->sd_path, sizeof(r->sd_path), "%s", prof->romfs_bin_path);
        r->has_sd_path = true;
        r->size = prof->romfs_bin_size;
        r->kind = 4; /* LAZY_ROMFS_IMAGE */
    }
    if (prof->has_exefs_nsp && rec_count < MAX_HOTSWAP_RECS) {
        HotswapRec* r = &recs[rec_count++];
        snprintf(r->name, sizeof(r->name), "exefs.nsp");
        snprintf(r->sd_path, sizeof(r->sd_path), "%s", prof->exefs_nsp_path);
        r->has_sd_path = true;
        r->size = prof->exefs_nsp_size;
        r->kind = 5; /* LAZY_EXEFS_IMAGE */
    } else if (prof->has_exefs_dir && prof->exefs_file_count > 0) {
        /* Loose unencrypted ExeFS NSOs (Switch 1 or Switch 2):
         * Keep Host's signed main.npdm intact, overlay all target NSOs via LAZY_EXEFS. */
        for (size_t i = 0; i < prof->exefs_file_count && rec_count < MAX_HOTSWAP_RECS; i++) {
            HotswapRec* r = &recs[rec_count++];
            snprintf(r->name, sizeof(r->name), "%s", prof->exefs_files[i].name);
            snprintf(r->sd_path, sizeof(r->sd_path), "%s", prof->exefs_files[i].sd_path);
            r->has_sd_path = true;
            r->size = prof->exefs_files[i].size;
            r->kind = 2; /* LAZY_EXEFS */
        }
    }
    for (size_t i = 0; i < prof->patch_count && rec_count < MAX_HOTSWAP_RECS; i++) {
        HotswapRec* r = &recs[rec_count++];
        snprintf(r->name, sizeof(r->name), "%s", prof->patches[i].name);
        snprintf(r->sd_path, sizeof(r->sd_path), "%s", prof->patches[i].sd_path);
        r->has_sd_path = true;
        r->size = prof->patches[i].size;
        r->kind = 6; /* LAZY_PATCH */
    }

    /* 2. Small mod-only loose romfs/ overlay (when no romfs.bin was packed) */
    if (!prof->has_romfs_bin && prof->has_romfs_dir) {
        size_t before = rec_count;
        if (!collect_loose_romfs(prof->romfs_dir_mount, prof->romfs_dir_rel, "", recs, &rec_count, MAX_HOTSWAP_RECS)) {
            free(recs);
            return false;
        }
        /* Ensure at least one sd_path entry comes first if files[0] would be LAZY_DIRECTORY */
        if (before == 0 && rec_count > 1 && !recs[0].has_sd_path) {
            for (size_t i = 1; i < rec_count; i++) {
                if (recs[i].has_sd_path) {
                    HotswapRec tmp = recs[0];
                    recs[0] = recs[i];
                    recs[i] = tmp;
                    break;
                }
            }
        }
    }

    /* 3. For full loose ExeFS replacement (contains "main"), stub out unused Host subsdk/rtld/sdk
     * so leftover Host modules cannot collide with the loaded Switch 1 / Switch 2 game. */
    if (!prof->has_exefs_nsp && prof->has_exefs_dir && exefs_has_module(prof, "main")) {
        static const char* STD_MODS[] = {
            "rtld", "sdk",
            "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
            "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9"
        };
        for (size_t i = 0; i < sizeof(STD_MODS) / sizeof(STD_MODS[0]) && rec_count < MAX_HOTSWAP_RECS; i++) {
            if (!exefs_has_module(prof, STD_MODS[i])) {
                HotswapRec* r = &recs[rec_count++];
                snprintf(r->name, sizeof(r->name), "%s", STD_MODS[i]);
                r->has_sd_path = false;
                r->kind = 3; /* LAZY_STUB */
            }
        }
    }

    const size_t GAME = 24, FILE_SZ = 80;
    size_t files_rel   = (size_t)align_up(GAME, 16);
    size_t strings_rel = files_rel + rec_count * FILE_SZ;

    size_t str_bytes = 0;
    for (size_t i = 0; i < rec_count; i++) {
        str_bytes += strlen(recs[i].name) + 1;
        if (recs[i].has_sd_path) str_bytes += strlen(recs[i].sd_path) + 1;
    }
    size_t needed = (size_t)align_up(strings_rel + str_bytes, 16);
    if (needed > region_cap) {
        free(recs);
        return false;
    }

    memset(region_out, 0, region_cap);

    /* Game record at offset 0 of region (= ctx_addr + CTX_GAMES_OFF) */
    put_u64(region_out + 0,  0, host_tid);
    put_u64(region_out + 8,  0, rec_count);
    put_u64(region_out + 16, 0, ctx_addr + CTX_GAMES_OFF + files_rel);

    size_t str_at = strings_rel;
    for (size_t i = 0; i < rec_count; i++) {
        size_t f_off = files_rel + i * FILE_SZ;
        uint64_t name_ptr = ctx_addr + CTX_GAMES_OFF + str_at;
        size_t nl = strlen(recs[i].name) + 1;
        memcpy(region_out + str_at, recs[i].name, nl);
        str_at += nl;

        uint64_t sd_ptr = 0;
        if (recs[i].has_sd_path) {
            sd_ptr = ctx_addr + CTX_GAMES_OFF + str_at;
            size_t sl = strlen(recs[i].sd_path) + 1;
            memcpy(region_out + str_at, recs[i].sd_path, sl);
            str_at += sl;
        }

        put_u64(region_out, f_off + 0,  name_ptr);
        put_u64(region_out, f_off + 8,  sd_ptr);
        put_u64(region_out, f_off + 64, recs[i].size);
        put_u32(region_out, f_off + 72, recs[i].kind);
    }

    free(recs);
    if (out_used) *out_used = needed;
    return true;
}

static uint8_t* build_ns2lfs02_context(uint64_t host_tid, const VesperProfile* prof,
                                       const VesperFsDiscovery* disc, const BuiltGateways* gw,
                                       uint64_t ctx_addr, size_t* out_size) {
    uint8_t* buf = (uint8_t*)calloc(1, (size_t)CTX_TOTAL_SIZE);
    if (!buf) return NULL;

    uint64_t base = disc->fs_base;
    memcpy(buf, "NS2LFS02", 8);
    put_u32(buf, 8, 0); /* active=0 until lfs_activate */
    put_u64(buf, 40, disc->sd_fs_object);
    put_u64(buf, 48, base + gw->orig_sdk[0]);
    put_u64(buf, 56, ctx_addr + CTX_NS_OFF);
    put_u64(buf, 104, base + 0x694c0ULL);
    put_u64(buf, 112, base + 0x1077b0ULL);
    put_u64(buf, 120, base + gw->orig_size);
    put_u64(buf, 128, base + gw->orig_open);
    put_u64(buf, 136, base + 0x69580ULL);
    for (int i = 0; i < 8; i++) put_u64(buf, 304 + i * 8, base + gw->orig_sdk[i]);

    /* Namespace header at CTX_NS_OFF (368) */
    size_t c = CTX_NS_OFF;
    put_u32(buf, c, 0);
    put_u32(buf, c + 4, 0x34465343u);
    put_u64(buf, c + 8,  base + gw->orig_open);
    put_u64(buf, c + 16, base + gw->orig_dir);
    put_u64(buf, c + 24, base + gw->orig_type);
    put_u64(buf, c + 32, disc->sd_fs_object + 8ULL);
    put_u64(buf, c + 40, base + gw->orig_sdk[0]);
    put_u64(buf, c + 48, base + gw->orig_sdk[1]);
    put_u64(buf, c + 56, base + gw->orig_sdk[2]);

    /* Lazy config at CTX_LAZY_OFF (480) */
    size_t a = CTX_LAZY_OFF;
    put_u64(buf, a + 0,   disc->program_registry);
    put_u64(buf, a + 8,   base + 0x121400ULL);
    put_u64(buf, a + 16,  base + 0x24a7a0ULL);
    put_u64(buf, a + 24,  base + 0x11ee960ULL);
    put_u64(buf, a + 32,  base + 0x215620ULL);
    put_u64(buf, a + 40,  base + 0x215800ULL);
    put_u64(buf, a + 48,  base + gw->orig_lazy[0]);
    put_u64(buf, a + 56,  base + gw->orig_lazy[1]);
    put_u64(buf, a + 64,  base + gw->orig_lazy[2]);
    put_u64(buf, a + 72,  1ULL); /* 1 game */
    put_u64(buf, a + 80,  ctx_addr + CTX_GAMES_OFF);
    put_u64(buf, a + 88,  0ULL); /* 0 global patches */
    put_u64(buf, a + 96,  ctx_addr + CTX_GAMES_OFF + CTX_HOTSWAP_MAX);
    put_u64(buf, a + 184, base + 0x107bc0ULL);
    put_u64(buf, a + 192, base + 0x107cd0ULL);

    if (!build_hotswap_region(host_tid, prof, ctx_addr, buf + CTX_GAMES_OFF, CTX_HOTSWAP_MAX, NULL)) {
        free(buf);
        return NULL;
    }

    *out_size = (size_t)CTX_TOTAL_SIZE;
    return buf;
}

static uint64_t find_fs_text_base(VesperKaihenCtx* kh) {
    uint64_t at = 0, cs_start = 0, cs_size = 0;
    for (int iter = 0; iter < 0x10000; iter++) {
        VesperMemInfo q = {0};
        if (R_FAILED(vesper_fs_query(kh, at, &q)) || q.size == 0) break;
        if (q.type == 3 && (q.perm == 4 || q.perm == 5)) {
            if (cs_start && q.address == cs_start + cs_size) cs_size += q.size;
            else { cs_start = q.address; cs_size = q.size; }
            if (cs_size == 0x24e000ULL) return cs_start;
        } else {
            cs_start = 0;
            cs_size = 0;
        }
        uint64_t next = q.address + q.size;
        if (next <= at) break;
        at = next;
    }
    return 0;
}

bool vesper_fs_query_resident(VesperKaihenCtx* kh, VesperResidentState* out) {
    memset(out, 0, sizeof(*out));
    if (!kh || !kh->open || !kh->has_fs_helpers) return false;

    uint64_t fs_base = find_fs_text_base(kh);
    if (!fs_base) return false;
    out->fs_base = fs_base;

    /* Check first 16 bytes at CAVE_MAIN: gateway #0 is ldr x16,+8; br x16; .quad rt_dest + LFS_READ_ENTRY */
    uint8_t gw0[16] = {0};
    if (R_FAILED(vesper_fs_read(kh, fs_base + CAVE_MAIN, gw0, 16))) return false;
    if (get_u32(gw0, 0) != 0x58000050u || get_u32(gw0, 4) != 0xd61f0200u) return false;

    uint64_t rt_entry = 0;
    memcpy(&rt_entry, gw0 + 8, 8);
    if (rt_entry <= VESPER_RT_OFF_LFS_READ_ENTRY) return false;
    out->rt_dest = rt_entry - VESPER_RT_OFF_LFS_READ_ENTRY;

    uint64_t ctx_addr = 0;
    if (R_FAILED(vesper_fs_read(kh, out->rt_dest + VESPER_RT_OFF_RESIDENT_CONTEXT, &ctx_addr, 8))) return false;
    if (ctx_addr < 0x100000000ULL) return false;

    uint8_t hdr[CTX_GAMES_OFF + 32];
    if (R_FAILED(vesper_fs_read(kh, ctx_addr, hdr, sizeof(hdr)))) return false;
    if (memcmp(hdr, "NS2LFS02", 8) != 0) return false;

    out->installed = true;
    out->ctx_addr = ctx_addr;

    uint64_t live_objs = 0, rom_active = 0, ns_active = 0, game_count = 0, tid = 0, files_ptr = 0, fcount = 0;
    uint32_t plan_lock = 0;
    memcpy(&live_objs,  hdr + 64, 8);
    memcpy(&rom_active, hdr + 96, 8);
    memcpy(&ns_active,  hdr + CTX_NS_OFF + 104, 8);
    memcpy(&game_count, hdr + CTX_LAZY_OFF + 72, 8);
    memcpy(&plan_lock,  hdr + CTX_LAZY_OFF + 144, 4);
    memcpy(&tid,        hdr + CTX_GAMES_OFF + 0, 8);
    memcpy(&fcount,     hdr + CTX_GAMES_OFF + 8, 8);
    memcpy(&files_ptr,  hdr + CTX_GAMES_OFF + 16, 8);

    out->live_objects = live_objs;
    out->in_flight_io = rom_active + ns_active + plan_lock;
    out->active_count = game_count;
    out->active_title_id = tid;

    /* Read first file's sd_path so the UI can show which profile is active in FS */
    if (game_count > 0 && fcount > 0 && files_ptr >= ctx_addr && files_ptr < ctx_addr + CTX_TOTAL_SIZE - 80) {
        uint64_t sd_ptr = 0;
        if (R_SUCCEEDED(vesper_fs_read(kh, files_ptr + 8, &sd_ptr, 8)) &&
            sd_ptr >= ctx_addr && sd_ptr < ctx_addr + CTX_TOTAL_SIZE - 2) {
            size_t avail = (size_t)((ctx_addr + CTX_TOTAL_SIZE) - sd_ptr);
            if (avail > sizeof(out->active_sd_path) - 1) avail = sizeof(out->active_sd_path) - 1;
            vesper_fs_read(kh, sd_ptr, out->active_sd_path, avail);
            out->active_sd_path[avail] = '\0';
        }
    }
    return true;
}

bool vesper_fs_hot_disable(VesperKaihenCtx* kh, char* err_buf, size_t err_len) {
    VesperResidentState st;
    if (!vesper_fs_query_resident(kh, &st) || !st.installed) {
        snprintf(err_buf, err_len, "FS runtime is not installed yet.");
        return false;
    }
    if (st.in_flight_io != 0) {
        snprintf(err_buf, err_len, "FS is currently busy (active=%llu). Try again in a second.",
                 (unsigned long long)st.in_flight_io);
        return false;
    }
    uint64_t zero = 0;
    Result rc = vesper_fs_write(kh, st.ctx_addr + CTX_LAZY_OFF + 72, &st.active_count, &zero, 8);
    if (R_FAILED(rc)) {
        snprintf(err_buf, err_len, "Hot-disable write failed: 0x%x", rc);
        return false;
    }
    return true;
}

static bool vesper_fs_hotswap_profile(VesperKaihenCtx* kh, const VesperResidentState* st,
                                      uint64_t host_tid, const char* host_tid_str,
                                      const VesperProfile* prof,
                                      char* err_buf, size_t err_len) {
    FILE* logf = fopen("sdmc:/vesper-install.log", "w");
    ilog(logf, "=== Vesper HOT-SWAP (Same kaiHEN Session, NO REBOOT) ===");
    ilog(logf, "ctx=0x%llx rt=0x%llx -> Host=%s Profile=%s",
         (unsigned long long)st->ctx_addr, (unsigned long long)st->rt_dest,
         host_tid_str, prof->name);

    if (st->in_flight_io != 0) {
        ifail(logf, err_buf, err_len, "FS is currently busy (active=%llu). Try again in a second.",
              (unsigned long long)st->in_flight_io);
        if (logf) fclose(logf);
        return false;
    }

    uint8_t* new_region = (uint8_t*)calloc(1, CTX_HOTSWAP_MAX);
    uint8_t* cur_region = (uint8_t*)calloc(1, CTX_HOTSWAP_MAX);
    if (!new_region || !cur_region) {
        free(new_region); free(cur_region);
        ifail(logf, err_buf, err_len, "Out of memory during hot-swap.");
        if (logf) fclose(logf);
        return false;
    }

    size_t used = 0;
    if (!build_hotswap_region(host_tid, prof, st->ctx_addr, new_region, CTX_HOTSWAP_MAX, &used)) {
        free(new_region); free(cur_region);
        ifail(logf, err_buf, err_len, "Profile manifest exceeds 16 KiB hot-swap buffer.");
        if (logf) fclose(logf);
        return false;
    }

    /* Step 1: Atomically set lazy_config.count = 0 while swapping */
    uint64_t zero = 0, one = 1;
    uint64_t count_addr = st->ctx_addr + CTX_LAZY_OFF + 72;
    if (st->active_count != 0) {
        Result rc = vesper_fs_write(kh, count_addr, &st->active_count, &zero, 8);
        if (R_FAILED(rc)) {
            free(new_region); free(cur_region);
            ifail(logf, err_buf, err_len, "Failed to pause count=0: 0x%x", rc);
            if (logf) fclose(logf);
            return false;
        }
    }

    /* Step 2: Overwrite games[0] + files[] + strings at ctx_addr + CTX_GAMES_OFF */
    Result rc = vesper_fs_read(kh, st->ctx_addr + CTX_GAMES_OFF, cur_region, CTX_HOTSWAP_MAX);
    if (R_SUCCEEDED(rc)) {
        rc = vesper_fs_write(kh, st->ctx_addr + CTX_GAMES_OFF, cur_region, new_region, CTX_HOTSWAP_MAX);
    }
    free(new_region);
    free(cur_region);
    if (R_FAILED(rc)) {
        ifail(logf, err_buf, err_len, "Hot-swap manifest write failed: 0x%x", rc);
        if (logf) fclose(logf);
        return false;
    }

    /* Step 3: Atomically re-enable lazy_config.count = 1 */
    rc = vesper_fs_write(kh, count_addr, &zero, &one, 8);
    if (R_FAILED(rc)) {
        ifail(logf, err_buf, err_len, "Failed to re-enable count=1: 0x%x", rc);
        if (logf) fclose(logf);
        return false;
    }

    ilog(logf, "=== HOT-SWAP COMPLETE (0x%zx bytes updated, ZERO REBOOT!) ===", used);
    if (logf) fclose(logf);
    return true;
}

bool vesper_fs_install_or_swap(VesperKaihenCtx* kh,
                               uint64_t host_tid,
                               const char* host_tid_str,
                               const VesperProfile* prof,
                               bool* out_was_hotswap,
                               char* err_buf,
                               size_t err_len) {
    if (out_was_hotswap) *out_was_hotswap = false;

    /* Check if our resident runtime is ALREADY active in this kaiHEN boot -> HOT-SWAP! */
    VesperResidentState st;
    if (vesper_fs_query_resident(kh, &st) && st.installed) {
        if (out_was_hotswap) *out_was_hotswap = true;
        return vesper_fs_hotswap_profile(kh, &st, host_tid, host_tid_str, prof, err_buf, err_len);
    }

    /* Otherwise perform the one-time cold install for this boot */
    FILE* logf = fopen("sdmc:/vesper-install.log", "w");
    ilog(logf, "=== Vesper On-Console FS Installer (Cold Boot Install) ===");
    ilog(logf, "Host: %s  Profile: %s", host_tid_str, prof->name);

    VesperFsDiscovery disc;
    if (!vesper_fs_discover(kh, &disc, true)) {
        ifail(logf, err_buf, err_len, "Discovery failed: %s", disc.error);
        if (logf) fclose(logf);
        return false;
    }
    if (!disc.abi_verified || !disc.program_registry) {
        ifail(logf, err_buf, err_len, "FS ABI or program registry unavailable.");
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }

    FsBridge br = {0};
    br.kh = kh;
    br.disc = &disc;
    br.logf = logf;
    br.cave_off = CAVE_MAIN;
    br.cave_addr = disc.fs_base + CAVE_MAIN;
    br.cave_len = 256; /* 0x000..0x0FF for bridge stubs; 0x300..0x31F for temp copy helper */

    /* Make cave page R-X */
    Result rc = vesper_fs_protect(kh, disc.fs_base + CAVE_PAGE, 0x1000, 5);
    if (R_FAILED(rc)) {
        ifail(logf, err_buf, err_len, "vesper_fs_protect(cave, RX) failed: 0x%x", rc);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }
    br.page_readable = true;

    /* Verify cave is unoccupied (prevents double-install in the same boot) */
    uint8_t cur_cave[GATEWAY_MAX] = {0}, zeros[GATEWAY_MAX] = {0};
    rc = vesper_fs_read(kh, br.cave_addr, cur_cave, GATEWAY_MAX);
    if (R_FAILED(rc) || memcmp(cur_cave, zeros, GATEWAY_MAX) != 0) {
        ifail(logf, err_buf, err_len, "FS runtime already installed in this boot! Reboot Switch 2 to switch game.");
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }

    rc = vesper_fs_read(kh, disc.fs_base + NOTIFIER_OFF, br.orig_notifier, 16);
    if (R_FAILED(rc)) {
        ifail(logf, err_buf, err_len, "Read notifier prologue failed: 0x%x", rc);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }
    put_u32(br.hook_notifier, 0, encode_b(disc.fs_base + NOTIFIER_OFF, br.cave_addr));
    for (int i = 4; i < 16; i += 4) put_u32(br.hook_notifier, i, 0xd503201fu);

    /* Allocate CTX_TOTAL_SIZE (64 KiB CodeMutable RW) + 0x1000 staging block via FS+0x694c0 */
    uint64_t ctx_size = CTX_TOTAL_SIZE;
    uint64_t ctx_addr = 0, stage_cm = 0;
    uint64_t a1[1] = {ctx_size};
    rc = bridge_invoke(&br, a1, 1, disc.fs_base + 0x694c0ULL, false, true, &ctx_addr);
    if (R_FAILED(rc) || ctx_addr < 0x100000000ULL) {
        ifail(logf, err_buf, err_len, "Context alloc failed: rc=0x%x addr=0x%llx", rc, (unsigned long long)ctx_addr);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }
    ilog(logf, "Allocated context at 0x%llx (size 0x%llx)", (unsigned long long)ctx_addr, (unsigned long long)ctx_size);

    uint64_t a_stage[1] = {0x1000ULL};
    rc = bridge_invoke(&br, a_stage, 1, disc.fs_base + 0x694c0ULL, false, true, &stage_cm);
    if (R_FAILED(rc) || stage_cm < 0x100000000ULL) {
        ifail(logf, err_buf, err_len, "Staging alloc failed: rc=0x%x", rc);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }

    /* Allocate Normal/RW pages for ARM64 runtime via FS+0x215620 */
    size_t rt_bytes = (size_t)align_up(VESPER_RT_CODE_SIZE, 4096);
    uint64_t a_rt[3] = {disc.fs_base + 0x11ee960ULL, ctx_addr + 152ULL, (uint64_t)rt_bytes};
    uint64_t alloc_rc = 1;
    rc = bridge_invoke(&br, a_rt, 3, disc.fs_base + 0x215620ULL, false, false, &alloc_rc);
    if (R_FAILED(rc) || alloc_rc != 0) {
        ifail(logf, err_buf, err_len, "Runtime Normal alloc failed: rc=0x%x fs_rc=0x%llx", rc, (unsigned long long)alloc_rc);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }

    uint64_t rt_source = 0;
    vesper_fs_read(kh, ctx_addr + 152ULL, &rt_source, 8);
    VesperMemInfo rt_info = {0};
    rc = vesper_fs_query(kh, rt_source, &rt_info);
    uint64_t rt_dest = rt_info.free_address;
    if (R_FAILED(rc) || rt_info.type != 5 || !rt_dest) {
        ifail(logf, err_buf, err_len, "Invalid runtime mapping: src=0x%llx type=%u dst=0x%llx",
              (unsigned long long)rt_source, rt_info.type, (unsigned long long)rt_dest);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }
    ilog(logf, "Runtime: source=0x%llx dest=0x%llx bytes=0x%zx",
         (unsigned long long)rt_source, (unsigned long long)rt_dest, rt_bytes);

    /* Build gateways & hooks targeting rt_dest */
    BuiltGateways gw;
    if (!build_gateways(kh, disc.fs_base, rt_dest, &gw, logf)) {
        ifail(logf, err_buf, err_len, "Failed to build ARM64 gateways.");
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }

    /* Patch runtime code literals */
    uint8_t* rt_buf = (uint8_t*)calloc(1, rt_bytes);
    if (!rt_buf) {
        ifail(logf, err_buf, err_len, "Out of memory allocating runtime buffer.");
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }
    memcpy(rt_buf, VESPER_RT_CODE, VESPER_RT_CODE_SIZE);
    put_u64(rt_buf, VESPER_RT_OFF_RESIDENT_CONTEXT, ctx_addr);
    put_u64(rt_buf, VESPER_RT_OFF_NAMESPACE_CONTEXT, ctx_addr + 368ULL);
    put_u64(rt_buf, VESPER_RT_OFF_LAZY_CONTEXT, ctx_addr + 480ULL);
    put_u64(rt_buf, VESPER_RT_OFF_LFS_CODEFS_ORIGINAL_ADDRESS, disc.fs_base + gw.orig_mount);

    /* Copy runtime into Normal memory & map each page RX */
    rc = copy_to_normal_fs(&br, stage_cm, rt_source, rt_buf, rt_bytes);
    free(rt_buf);
    if (R_FAILED(rc)) {
        ifail(logf, err_buf, err_len, "Copy runtime to Normal FS failed: 0x%x", rc);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }

    for (size_t off = 0; off < rt_bytes; off += 0x1000) {
        rc = vesper_fs_map_code(kh, rt_source + off, rt_dest + off);
        if (R_FAILED(rc)) {
            ifail(logf, err_buf, err_len, "map_code page +0x%zx failed: 0x%x", off, rc);
            vesper_fs_discovery_close(&disc);
            if (logf) fclose(logf);
            return false;
        }
    }

    /* Flush I-cache on mapped runtime */
    uint64_t pub_args[2] = {rt_dest, (uint64_t)rt_bytes};
    rc = bridge_invoke(&br, pub_args, 2, 0, true, false, NULL);
    if (R_FAILED(rc)) {
        ifail(logf, err_buf, err_len, "Publish runtime (icache flush) failed: 0x%x", rc);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }
    ilog(logf, "Runtime mapped and published at 0x%llx", (unsigned long long)rt_dest);

    /* Build and write NS2LFS02 manifest into ctx_addr */
    size_t built_ctx_sz = 0;
    uint8_t* ctx_buf = build_ns2lfs02_context(host_tid, prof, &disc, &gw, ctx_addr, &built_ctx_sz);
    if (!ctx_buf || built_ctx_sz > ctx_size) {
        free(ctx_buf);
        ifail(logf, err_buf, err_len, "Context build failed (sz=0x%zx > 0x%llx).", built_ctx_sz, (unsigned long long)ctx_size);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }

    uint8_t* cur_ctx = (uint8_t*)calloc(1, built_ctx_sz);
    if (!cur_ctx) {
        free(ctx_buf);
        ifail(logf, err_buf, err_len, "Out of memory allocating context read buffer.");
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }
    vesper_fs_read(kh, ctx_addr, cur_ctx, built_ctx_sz);
    rc = vesper_fs_write(kh, ctx_addr, cur_ctx, ctx_buf, built_ctx_sz);
    free(cur_ctx);
    free(ctx_buf);
    if (R_FAILED(rc)) {
        ifail(logf, err_buf, err_len, "Write NS2LFS02 context failed: 0x%x", rc);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }
    ilog(logf, "Wrote NS2LFS02 manifest (0x%zx bytes)", built_ctx_sz);

    /* Retain SD filesystem reference via lfs_initialize */
    uint64_t init_res = 1;
    rc = bridge_invoke(&br, NULL, 0, rt_dest + VESPER_RT_OFF_LFS_INITIALIZE, false, false, &init_res);
    if (R_FAILED(rc) || init_res != 0) {
        ifail(logf, err_buf, err_len, "lfs_initialize failed: rc=0x%x res=0x%llx", rc, (unsigned long long)init_res);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }
    /* Close our client SD handle now that FS holds its own reference */
    svcCloseHandle(disc.sd_fs_handle);
    disc.sd_fs_handle = INVALID_HANDLE;

    /* Switch bridge to control cave 0x24d800 so we can write gateways to 0x24d100 */
    rc = bridge_switch_to_ctrl_cave(&br);
    if (R_FAILED(rc)) {
        ifail(logf, err_buf, err_len, "Switch to control cave failed: 0x%x", rc);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }

    /* Stage gateways into 0x24d100 */
    uint8_t gw_padded[GATEWAY_MAX] = {0};
    memcpy(gw_padded, gw.gateways, gw.gateway_len);
    for (size_t i = 0; i < GATEWAY_MAX; i += 16) {
        if (memcmp(gw_padded + i, zeros, 16) != 0) {
            rc = vesper_fs_patch(kh, disc.fs_base + CAVE_MAIN + i, zeros, gw_padded + i, 16);
            if (R_FAILED(rc)) {
                ifail(logf, err_buf, err_len, "Patch gateway +0x%zx failed: 0x%x", i, rc);
                vesper_fs_discovery_close(&disc);
                if (logf) fclose(logf);
                return false;
            }
        }
    }

    /* Attach all hooks */
    for (size_t i = 0; i < gw.hook_count; i++) {
        rc = vesper_fs_patch(kh, disc.fs_base + gw.hooks[i].offset, gw.hooks[i].orig, gw.hooks[i].repl, 16);
        if (R_FAILED(rc)) {
            ifail(logf, err_buf, err_len, "Patch hook #%zu (0x%llx) failed: 0x%x",
                  i, (unsigned long long)gw.hooks[i].offset, rc);
            vesper_fs_discovery_close(&disc);
            if (logf) fclose(logf);
            return false;
        }
    }

    /* Activate resident runtime */
    uint64_t act_res = 1;
    rc = bridge_invoke(&br, NULL, 0, rt_dest + VESPER_RT_OFF_LFS_ACTIVATE, false, false, &act_res);
    if (R_FAILED(rc) || act_res != 0) {
        ifail(logf, err_buf, err_len, "lfs_activate failed: rc=0x%x res=0x%llx", rc, (unsigned long long)act_res);
        vesper_fs_discovery_close(&disc);
        if (logf) fclose(logf);
        return false;
    }

    /* Free staging block & clear control cave */
    uint64_t free_args[2] = {stage_cm, 0x1000ULL};
    bridge_invoke(&br, free_args, 2, disc.fs_base + 0x69580ULL, false, false, NULL);
    bridge_stage(&br, NULL, 0);
    vesper_fs_discovery_close(&disc);

    ilog(logf, "=== RESIDENT FS RUNTIME ACTIVE! ===");
    if (logf) fclose(logf);
    return true;
}
