/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "kaihen_ro.h"
#include "sd_scanner.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool installed;
    uint64_t fs_base;
    uint64_t rt_dest;
    uint64_t ctx_addr;
    uint64_t active_count;      /* 0 = disabled (stock), 1 = game active */
    uint64_t active_title_id;
    uint64_t in_flight_io;      /* romfs.active + codefs.active + plan_lock */
    uint64_t live_objects;      /* non-zero while a host game is still running */
    char active_sd_path[VESPER_MAX_PATH];
} VesperResidentState;

/* Inspects FS (100% read-only) to check if Vesper's resident runtime is already
 * installed in the current kaiHEN boot, and which game/profile is active. */
bool vesper_fs_query_resident(VesperKaihenCtx* kh, VesperResidentState* out);

/* Cold-installs (first time per boot) OR hot-swaps (subsequent times in the SAME
 * kaiHEN session, NO REBOOT needed!) the selected Host Title ID + Profile in FS.
 * Sets *out_was_hotswap = true if it performed an instant hot-swap. */
bool vesper_fs_install_or_swap(VesperKaihenCtx* kh,
                               uint64_t host_tid,
                               const char* host_tid_str,
                               const VesperProfile* prof,
                               bool* out_was_hotswap,
                               char* err_buf,
                               size_t err_len);

/* Hot-disables the active profile (sets lazy game count = 0) without rebooting,
 * so the host title boots stock/unmodded until you activate another profile. */
bool vesper_fs_hot_disable(VesperKaihenCtx* kh, char* err_buf, size_t err_len);
