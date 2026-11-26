/* SPDX-License-Identifier: GPL-2.0-only */
#include "fs_discover.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define FS_TEXT_SIZE_EXPECTED 0x24e000ULL
#define DOMAIN_CAPACITY       0x4000u
#define DOMAIN_ENTRY_SIZE     0x30ULL
#define CLONE_COUNT           7
#define MAX_CANDIDATES        4096

static bool is_canonical(uint64_t v) {
    return v >= 0x100000000ULL && v < 0x8000000000ULL;
}

static bool within_regions(const VesperRegion* regs, size_t count, uint64_t addr, uint64_t len) {
    if (addr + len < addr) return false;
    for (size_t i = 0; i < count; i++) {
        if (addr >= regs[i].address && addr + len <= regs[i].address + regs[i].size) return true;
    }
    return false;
}

static void dlog(FILE* f, const char* fmt, ...) {
    va_list a;
    if (f) {
        va_start(a, fmt);
        vfprintf(f, fmt, a);
        fprintf(f, "\n");
        fflush(f);
        va_end(a);
    }
}

static void fail(VesperFsDiscovery* out, FILE* logf, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(out->error, sizeof(out->error), fmt, a);
    va_end(a);
    dlog(logf, "[FAIL] %s", out->error);
}

static Result fs_read_u32(VesperKaihenCtx* kh, uint64_t addr, uint32_t* val) {
    return vesper_fs_read(kh, addr, val, sizeof(*val));
}

static Result fs_read_u64(VesperKaihenCtx* kh, uint64_t addr, uint64_t* val) {
    return vesper_fs_read(kh, addr, val, sizeof(*val));
}

/* Raw CMIF Control request (type=5): cmd 0 = ConvertCurrentObjectToDomain, cmd 2 = CloneCurrentObject */
static Result cmif_control_clone(Handle session, Handle* out_clone) {
    void* tls = armGetTls();
    CmifRequest req = cmifMakeRequest(tls, (CmifRequestFormat){
        .request_id = 2,
        .data_size = 0,
        .context = 0,
    });
    (void)req;
    /* Override HIPC header type to Control (5) */
    uint32_t* hdr = (uint32_t*)tls;
    hdr[0] = (hdr[0] & ~0xFFFFu) | CmifCommandType_Control;

    Result rc = svcSendSyncRequest(session);
    if (R_FAILED(rc)) return rc;

    CmifResponse resp = {0};
    rc = cmifParseResponse(&resp, tls, false, 0);
    if (R_FAILED(rc)) return rc;
    if (resp.move_handles && resp.move_handles[0] != INVALID_HANDLE) {
        *out_clone = resp.move_handles[0];
        return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

static Result cmif_control_convert_domain(Handle session, uint32_t* out_obj_id) {
    void* tls = armGetTls();
    CmifRequest req = cmifMakeRequest(tls, (CmifRequestFormat){
        .request_id = 0,
        .data_size = 0,
        .context = 0,
    });
    (void)req;
    uint32_t* hdr = (uint32_t*)tls;
    hdr[0] = (hdr[0] & ~0xFFFFu) | CmifCommandType_Control;

    Result rc = svcSendSyncRequest(session);
    if (R_FAILED(rc)) return rc;

    CmifResponse resp = {0};
    rc = cmifParseResponse(&resp, tls, false, sizeof(uint32_t));
    if (R_FAILED(rc)) return rc;
    if (out_obj_id && resp.data) {
        memcpy(out_obj_id, resp.data, sizeof(uint32_t));
    }
    return 0;
}

static Result fsp_open_cmd_move(Handle fs_root, uint32_t cmd_id, Handle* out_h) {
    Service s = {0};
    serviceCreate(&s, fs_root);
    return serviceDispatch(&s, cmd_id,
        .out_handle_attrs = {SfOutHandleAttr_HipcMove},
        .out_handles = out_h);
}

static bool verify_domain_chain(VesperKaihenCtx* kh, const VesperRegion* regs, size_t reg_count,
                                uint64_t entry, uint64_t E, uint32_t k,
                                uint64_t* out_table, uint32_t* out_count) {
    if (!within_regions(regs, reg_count, entry, DOMAIN_ENTRY_SIZE)) return false;
    uint8_t ebuf[0x30];
    if (R_FAILED(vesper_fs_read(kh, entry, ebuf, sizeof(ebuf)))) return false;

    uint64_t entry_obj = 0, domain_ptr = 0;
    memcpy(&entry_obj, ebuf + 0x20, 8);
    memcpy(&domain_ptr, ebuf + 0x18, 8);
    if (entry_obj != E || !is_canonical(domain_ptr)) return false;

    if (!within_regions(regs, reg_count, domain_ptr + 0x18, 8)) return false;
    uint64_t manager = 0;
    if (R_FAILED(fs_read_u64(kh, domain_ptr + 0x18, &manager)) || !is_canonical(manager)) return false;

    if (!within_regions(regs, reg_count, manager + 0x10, 0x10)) return false;
    uint8_t mbuf[0x10];
    if (R_FAILED(vesper_fs_read(kh, manager + 0x10, mbuf, sizeof(mbuf)))) return false;

    uint32_t count = 0;
    uint64_t table = 0;
    memcpy(&count, mbuf, 4);
    memcpy(&table, mbuf + 8, 8);

    uint64_t expected_table = entry - (uint64_t)(k - 1) * DOMAIN_ENTRY_SIZE;
    if (count == DOMAIN_CAPACITY && table == expected_table) {
        *out_table = table;
        *out_count = count;
        return true;
    }
    return false;
}

typedef struct {
    uint64_t address;
    size_t size;
    uint8_t* bytes;
} SnapBlock;

static void free_snap(SnapBlock* snap, size_t count) {
    if (!snap) return;
    for (size_t i = 0; i < count; i++) free(snap[i].bytes);
    free(snap);
}

static SnapBlock* take_snapshot(VesperKaihenCtx* kh, const VesperRegion* regs, size_t count, FILE* logf, const char* label) {
    SnapBlock* snap = (SnapBlock*)calloc(count, sizeof(SnapBlock));
    if (!snap) return NULL;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        snap[i].address = regs[i].address;
        snap[i].size = (size_t)regs[i].size;
        snap[i].bytes = (uint8_t*)malloc(snap[i].size);
        if (!snap[i].bytes) {
            free_snap(snap, count);
            return NULL;
        }
        Result rc = vesper_fs_read(kh, snap[i].address, snap[i].bytes, snap[i].size);
        if (R_FAILED(rc)) {
            dlog(logf, "[SNAP %s] fsRead(0x%llx, 0x%zx) failed: 0x%x",
                 label, (unsigned long long)snap[i].address, snap[i].size, rc);
            free_snap(snap, count);
            return NULL;
        }
        total += snap[i].size;
    }
    dlog(logf, "[SNAP %s] read %.2f MiB across %zu CodeMutable RW block(s)",
         label, (double)total / (1024.0 * 1024.0), count);
    return snap;
}

void vesper_fs_discovery_close(VesperFsDiscovery* disc) {
    if (!disc) return;
    if (disc->sd_fs_handle != INVALID_HANDLE && disc->sd_fs_handle != 0) {
        svcCloseHandle(disc->sd_fs_handle);
        disc->sd_fs_handle = INVALID_HANDLE;
    }
    if (disc->notifier_handle != INVALID_HANDLE && disc->notifier_handle != 0) {
        svcCloseHandle(disc->notifier_handle);
        disc->notifier_handle = INVALID_HANDLE;
    }
}

bool vesper_fs_discover(VesperKaihenCtx* kh, VesperFsDiscovery* out, bool keep_handles) {
    memset(out, 0, sizeof(*out));
    out->notifier_handle = INVALID_HANDLE;
    out->sd_fs_handle = INVALID_HANDLE;

    FILE* logf = fopen("sdmc:/vesper-discover.log", "w");
    dlog(logf, "=== Vesper Read-Only FS Discovery ===");

    if (!kh || !kh->open || !kh->has_fs_helpers) {
        fail(out, logf, "Patched kaiHEN FS helpers are not active.");
        if (logf) fclose(logf);
        return false;
    }

    /* 1. Walk FS memory map to find FS .text (contiguous CodeStatic perm 4/5 summing to 0x24e000,
     *    handling both unsplit 0x24e000 and split 0x24d000 + 0x1000 after cave RX toggle)
     *    and collect all CodeMutable RW blocks. */
    uint64_t at = 0;
    uint64_t cs_start = 0, cs_size = 0;
    for (int iter = 0; iter < 0x10000; iter++) {
        VesperMemInfo q = {0};
        Result rc = vesper_fs_query(kh, at, &q);
        if (R_FAILED(rc)) {
            fail(out, logf, "fsQuery(0x%llx) failed: 0x%x", (unsigned long long)at, rc);
            if (logf) fclose(logf);
            return false;
        }
        if (q.size == 0) break;

        if (!out->fs_base && q.type == 3 && (q.perm == 4 || q.perm == 5)) {
            if (cs_start && q.address == cs_start + cs_size) {
                cs_size += q.size;
            } else {
                cs_start = q.address;
                cs_size = q.size;
            }
            if (cs_size == FS_TEXT_SIZE_EXPECTED) {
                out->fs_base = cs_start;
                out->fs_text_size = cs_size;
                dlog(logf, "Found FS .text: base=0x%llx size=0x%llx (last block perm=%u size=0x%llx)",
                     (unsigned long long)cs_start, (unsigned long long)cs_size, q.perm, (unsigned long long)q.size);
            }
        } else if (q.type != 3) {
            cs_start = 0;
            cs_size = 0;
        }
        if (q.type == 4 && q.perm == 3 && out->cm_region_count < VESPER_MAX_CM_REGIONS) {
            out->cm_regions[out->cm_region_count].address = q.address;
            out->cm_regions[out->cm_region_count].size = q.size;
            out->cm_region_count++;
            dlog(logf, "  CodeMutable RW[%zu]: 0x%llx .. +0x%llx",
                 out->cm_region_count - 1, (unsigned long long)q.address, (unsigned long long)q.size);
        }

        uint64_t next = q.address + q.size;
        if (next <= at) break; /* wrapped 2^64 */
        at = next;
    }

    if (!out->fs_base) {
        fail(out, logf, "FS .text (CodeStatic, size 0x24e000) not found.");
        if (logf) fclose(logf);
        return false;
    }
    if (out->cm_region_count == 0) {
        fail(out, logf, "No CodeMutable RW blocks found in FS.");
        if (logf) fclose(logf);
        return false;
    }

    /* 2. Verify FS ABI prologues (read-only) & read program registry */
    uint8_t p_read[16] = {0}, p_size[16] = {0};
    static const uint8_t EXP_READ[16] = {0x62,0x07,0xf8,0xb7,0xe4,0x08,0xf8,0xb7,0x69,0x04,0x40,0xf9,0x9f,0x00,0x09,0xeb};
    static const uint8_t EXP_SIZE[16] = {0x7f,0x23,0x03,0xd5,0xff,0xc3,0x00,0xd1,0xfd,0x7b,0x01,0xa9,0xf4,0x4f,0x02,0xa9};
    if (R_SUCCEEDED(vesper_fs_read(kh, out->fs_base + 0x104ef0ULL, p_read, 16)) &&
        R_SUCCEEDED(vesper_fs_read(kh, out->fs_base + 0x105480ULL, p_size, 16)) &&
        memcmp(p_read, EXP_READ, 16) == 0 && memcmp(p_size, EXP_SIZE, 16) == 0) {
        out->abi_verified = true;
        dlog(logf, "FS ABI prologues VERIFIED (23.0.1 match!)");
    } else {
        dlog(logf, "WARNING: FS ABI prologue read/match failed (may be X-only .text or different FW)");
    }

    uint64_t reg_ptr = 0;
    if (R_SUCCEEDED(fs_read_u64(kh, out->fs_base + 0x11e12d0ULL, &reg_ptr)) && is_canonical(reg_ptr)) {
        out->program_registry = reg_ptr + 16ULL;
        dlog(logf, "Program registry: 0x%llx", (unsigned long long)out->program_registry);
    } else {
        dlog(logf, "WARNING: Program registry unreadable at base+0x11e12d0 (raw=0x%llx)", (unsigned long long)reg_ptr);
    }

    /* 3. Open owned ISystemDataUpdateEventNotifier (fsp-srv cmd 510) */
    Result rc = fsp_open_cmd_move(kh->fs_root, 510, &out->notifier_handle);
    if (R_FAILED(rc)) {
        fail(out, logf, "OpenSystemDataUpdateEventNotifier (cmd 510) failed: 0x%x", rc);
        if (logf) fclose(logf);
        return false;
    }
    dlog(logf, "Opened notifier handle: 0x%x", out->notifier_handle);

    /* 4. Refcount bump 1 -> 8 -> 1 to find notifier object E */
    SnapBlock* s1 = take_snapshot(kh, out->cm_regions, out->cm_region_count, logf, "1-session");
    if (!s1) {
        fail(out, logf, "Snapshot 1 failed (out of memory or fsRead error).");
        vesper_fs_discovery_close(out);
        if (logf) fclose(logf);
        return false;
    }

    Handle clones[CLONE_COUNT];
    int cloned = 0;
    for (int i = 0; i < CLONE_COUNT; i++) {
        clones[i] = INVALID_HANDLE;
        rc = cmif_control_clone(out->notifier_handle, &clones[i]);
        if (R_FAILED(rc)) {
            for (int j = 0; j < cloned; j++) svcCloseHandle(clones[j]);
            free_snap(s1, out->cm_region_count);
            fail(out, logf, "CloneCurrentObject #%d failed: 0x%x", i, rc);
            vesper_fs_discovery_close(out);
            if (logf) fclose(logf);
            return false;
        }
        cloned++;
    }

    SnapBlock* s2 = take_snapshot(kh, out->cm_regions, out->cm_region_count, logf, "8-sessions");
    for (int j = 0; j < cloned; j++) svcCloseHandle(clones[j]);

    if (!s2) {
        free_snap(s1, out->cm_region_count);
        fail(out, logf, "Snapshot 2 failed.");
        vesper_fs_discovery_close(out);
        if (logf) fclose(logf);
        return false;
    }

    uint64_t* cands = (uint64_t*)malloc(MAX_CANDIDATES * sizeof(uint64_t));
    size_t cand_count = 0;
    for (size_t r = 0; r < out->cm_region_count; r++) {
        for (size_t o = 0; o + 4 <= s1[r].size; o += 8) {
            uint32_t v1 = 0, v2 = 0;
            memcpy(&v1, s1[r].bytes + o, 4);
            memcpy(&v2, s2[r].bytes + o, 4);
            if (v1 == 1 && v2 == (CLONE_COUNT + 1)) {
                if (cand_count < MAX_CANDIDATES) cands[cand_count++] = s1[r].address + o;
            }
        }
    }
    free_snap(s1, out->cm_region_count);
    dlog(logf, "Refcount 1->8 candidates: %zu", cand_count);

    /* Filter survivors back to 1 */
    size_t surv = 0;
    for (size_t i = 0; i < cand_count; i++) {
        uint32_t v = 0;
        if (R_SUCCEEDED(fs_read_u32(kh, cands[i], &v)) && v == 1) {
            cands[surv++] = cands[i];
        }
    }
    dlog(logf, "Refcount survivors after close: %zu", surv);

    /* Narrow ambiguity if >1 survivor */
    for (int round = 0; surv > 1 && round < 3; round++) {
        int n = 3 + round;
        Handle rc_clones[8];
        int ok_c = 0;
        for (int i = 0; i < n; i++) {
            if (R_SUCCEEDED(cmif_control_clone(out->notifier_handle, &rc_clones[ok_c]))) ok_c++;
        }
        size_t next = 0;
        for (size_t i = 0; i < surv; i++) {
            uint32_t v = 0;
            if (R_SUCCEEDED(fs_read_u32(kh, cands[i], &v)) && v == (uint32_t)(ok_c + 1)) cands[next++] = cands[i];
        }
        surv = next;
        for (int i = 0; i < ok_c; i++) svcCloseHandle(rc_clones[i]);
        next = 0;
        for (size_t i = 0; i < surv; i++) {
            uint32_t v = 0;
            if (R_SUCCEEDED(fs_read_u32(kh, cands[i], &v)) && v == 1) cands[next++] = cands[i];
        }
        surv = next;
    }

    if (surv != 1) {
        free(cands);
        free_snap(s2, out->cm_region_count);
        fail(out, logf, "Notifier refcount not unique (%zu candidates).", surv);
        vesper_fs_discovery_close(out);
        if (logf) fclose(logf);
        return false;
    }

    out->notifier_E = cands[0] - 0x10ULL;
    free(cands);

    uint8_t head[0x18] = {0};
    if (R_FAILED(vesper_fs_read(kh, out->notifier_E, head, sizeof(head)))) {
        free_snap(s2, out->cm_region_count);
        fail(out, logf, "Failed to read notifier object E at 0x%llx.", (unsigned long long)out->notifier_E);
        vesper_fs_discovery_close(out);
        if (logf) fclose(logf);
        return false;
    }
    memcpy(&out->notifier_vptr, head, 8);
    memcpy(&out->notifier_B, head + 8, 8);
    dlog(logf, "Notifier E=0x%llx B=0x%llx vptr=0x%llx",
         (unsigned long long)out->notifier_E, (unsigned long long)out->notifier_B, (unsigned long long)out->notifier_vptr);

    /* 5. Find CMIF domain table via converted clone */
    Handle dom_clone = INVALID_HANDLE;
    rc = cmif_control_clone(out->notifier_handle, &dom_clone);
    if (R_FAILED(rc)) {
        free_snap(s2, out->cm_region_count);
        fail(out, logf, "Clone for domain table failed: 0x%x", rc);
        vesper_fs_discovery_close(out);
        if (logf) fclose(logf);
        return false;
    }

    uint32_t obj_id = 0;
    rc = cmif_control_convert_domain(dom_clone, &obj_id);
    if (R_FAILED(rc) || obj_id == 0 || obj_id > DOMAIN_CAPACITY) {
        svcCloseHandle(dom_clone);
        free_snap(s2, out->cm_region_count);
        fail(out, logf, "ConvertCurrentObjectToDomain failed: rc=0x%x id=%u", rc, obj_id);
        vesper_fs_discovery_close(out);
        if (logf) fclose(logf);
        return false;
    }

    bool found_table = false;
    for (size_t r = 0; r < out->cm_region_count && !found_table; r++) {
        for (size_t o = 0; o + 0x20 <= s2[r].size; o += 8) {
            uint32_t cap = 0;
            uint64_t tbl = 0;
            memcpy(&cap, s2[r].bytes + o + 0x10, 4);
            memcpy(&tbl, s2[r].bytes + o + 0x18, 8);
            if (cap == DOMAIN_CAPACITY && is_canonical(tbl) &&
                within_regions(out->cm_regions, out->cm_region_count, tbl, (uint64_t)DOMAIN_CAPACITY * DOMAIN_ENTRY_SIZE)) {
                uint64_t entry = tbl + (uint64_t)(obj_id - 1) * DOMAIN_ENTRY_SIZE;
                if (verify_domain_chain(kh, out->cm_regions, out->cm_region_count, entry, out->notifier_E, obj_id,
                                        &out->domain_table, &out->domain_count)) {
                    found_table = true;
                    break;
                }
            }
        }
    }
    svcCloseHandle(dom_clone);
    free_snap(s2, out->cm_region_count);

    if (!found_table) {
        fail(out, logf, "CMIF domain table not found via manager scan (obj_id=%u).", obj_id);
        vesper_fs_discovery_close(out);
        if (logf) fclose(logf);
        return false;
    }
    dlog(logf, "CMIF domain table=0x%llx count=0x%x",
         (unsigned long long)out->domain_table, out->domain_count);

    /* 6. Locate SD Card IFileSystem object via fsp-srv cmd 18 + domain clone */
    rc = fsp_open_cmd_move(kh->fs_root, 18, &out->sd_fs_handle);
    if (R_FAILED(rc)) {
        fail(out, logf, "OpenSdCardFileSystem (cmd 18) failed: 0x%x", rc);
        vesper_fs_discovery_close(out);
        if (logf) fclose(logf);
        return false;
    }
    Handle sd_clone = INVALID_HANDLE;
    rc = cmif_control_clone(out->sd_fs_handle, &sd_clone);
    if (R_SUCCEEDED(rc)) {
        uint32_t sd_id = 0;
        rc = cmif_control_convert_domain(sd_clone, &sd_id);
        if (R_SUCCEEDED(rc) && sd_id > 0 && sd_id <= out->domain_count) {
            uint64_t sd_entry = out->domain_table + (uint64_t)(sd_id - 1) * DOMAIN_ENTRY_SIZE;
            if (R_SUCCEEDED(fs_read_u64(kh, sd_entry + 0x20, &out->sd_fs_object)) && is_canonical(out->sd_fs_object)) {
                uint32_t ref_during = 0;
                fs_read_u32(kh, out->sd_fs_object + 0x28, &ref_during);
                svcCloseHandle(sd_clone);
                sd_clone = INVALID_HANDLE;
                fs_read_u32(kh, out->sd_fs_object + 0x28, &out->sd_fs_baseline);
                dlog(logf, "SD IFileSystem object=0x%llx ref(during=%u -> baseline=%u)",
                     (unsigned long long)out->sd_fs_object, ref_during, out->sd_fs_baseline);
            }
        }
        if (sd_clone != INVALID_HANDLE) svcCloseHandle(sd_clone);
    }

    if (!is_canonical(out->sd_fs_object)) {
        fail(out, logf, "Could not locate SD IFileSystem object in domain table.");
        vesper_fs_discovery_close(out);
        if (logf) fclose(logf);
        return false;
    }

    if (!keep_handles) {
        vesper_fs_discovery_close(out);
    }

    out->ok = true;
    dlog(logf, "=== DISCOVERY COMPLETE (ALL VERIFIED, 0 BYTES WRITTEN) ===");
    if (logf) fclose(logf);
    return true;
}
