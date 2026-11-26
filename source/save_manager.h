/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <switch.h>

#define VESPER_MAX_USERS 8

typedef struct {
    AccountUid uid;
    char uid_hex[33];     /* 32-hex lowercase */
    char nickname[33];    /* Account display name */
} VesperUser;

typedef struct {
    bool initialized;
    VesperUser users[VESPER_MAX_USERS];
    size_t user_count;
    size_t selected_user;
    bool auto_swap_enabled;
    char active_owner[128]; /* "_stock" or profile name currently inside save:/ */
} VesperSaveCtx;

/* Initializes Switch 2 account service and enumerates user profiles on the console. */
bool vesper_save_init(VesperSaveCtx* ctx);
void vesper_save_exit(VesperSaveCtx* ctx);

/* Refreshes ctx->active_owner from sdmc:/vesper/saves/<HOST_ID>/<UID>/.active_save. */
void vesper_save_refresh_owner(VesperSaveCtx* ctx, const char* host_tid_str);

/* 100% READ-ONLY test: mounts save:/ for (host_tid, selected user), counts files & bytes,
 * unmounts cleanly without writing a single byte. Logs to sdmc:/vesper-save.log. */
bool vesper_save_probe_readonly(VesperSaveCtx* ctx, uint64_t host_tid, const char* host_tid_str,
                                size_t* out_files, uint64_t* out_bytes, char* err_buf, size_t err_len);

/* Safely swaps save:/ from its current owner (e.g. "_stock" or previous profile)
 * to `target_profile` (use "_stock" when disabling mods).
 * ALWAYS completes and verifies backup before clearing save:/, and keeps a permanent
 * _stock_initial_backup copy the very first time _stock is backed up. */
bool vesper_save_swap_profile(VesperSaveCtx* ctx, uint64_t host_tid, const char* host_tid_str,
                              const char* target_profile, char* err_buf, size_t err_len);
