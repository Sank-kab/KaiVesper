/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "kaihen_ro.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VESPER_MAX_CM_REGIONS 64

typedef struct {
    uint64_t address;
    uint64_t size;
} VesperRegion;

typedef struct {
    bool ok;
    uint64_t fs_base;
    uint64_t fs_text_size;
    uint64_t program_registry;
    uint64_t notifier_E;
    uint64_t notifier_B;
    uint64_t notifier_vptr;
    uint64_t domain_table;
    uint32_t domain_count;
    uint64_t sd_fs_object;
    uint32_t sd_fs_baseline;
    bool abi_verified;
    size_t cm_region_count;
    VesperRegion cm_regions[VESPER_MAX_CM_REGIONS];
    /* Live handles kept open only if keep_handles=true; otherwise closed cleanly. */
    Handle notifier_handle;
    Handle sd_fs_handle;
    char error[256];
} VesperFsDiscovery;

/* 100% READ-ONLY discovery of FS .text, ABI prologues, program registry,
 * notifier E/B, CMIF domain table, and SD IFileSystem object.
 * Never writes a single byte to FS! Logs details to sdmc:/vesper-discover.log. */
bool vesper_fs_discover(VesperKaihenCtx* kh, VesperFsDiscovery* out, bool keep_handles);
void vesper_fs_discovery_close(VesperFsDiscovery* disc);
