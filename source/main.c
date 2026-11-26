/* SPDX-License-Identifier: GPL-2.0-only */
#include "console_titles.h"
#include "fs_discover.h"
#include "fs_installer.h"
#include "gui.h"
#include "kaihen_ro.h"
#include "profile_manager.h"
#include "romfs_builder.h"
#include "save_manager.h"
#include "sd_scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

typedef struct {
    KaiVesperTab tab;
    const VesperKaihenCtx* kh;
    const VesperResidentState* rs;
    const VesperSaveCtx* sv;
    const VesperCatalog* cat;
    const VesperConsoleCatalog* cc;
    size_t host_idx;
    size_t prof_idx;
    HostFilterMode host_filter;
    size_t ct_idx;
    const char* prof_name;
} RomfsUiProgressCtx;

static void on_romfs_pack_progress(uint64_t done, uint64_t total,
                                   size_t fdone, size_t ftotal, void* ud) {
    RomfsUiProgressCtx* c = (RomfsUiProgressCtx*)ud;
    char sdone[24], stot[24], msg[256];
    vesper_fmt_size(done, sdone, sizeof(sdone));
    vesper_fmt_size(total, stot, sizeof(stot));
    unsigned pct = (total > 0) ? (unsigned)((done * 100ULL) / total) : 100u;
    snprintf(msg, sizeof(msg), "Packing romfs.bin for '%s'... %u%% (%s / %s, %zu/%zu files)",
             c->prof_name, pct, sdone, stot, fdone, ftotal);
    vesper_gui_render(c->tab, c->kh, c->rs, c->sv, c->cat, c->cc,
                      c->host_idx, c->prof_idx, c->host_filter, c->ct_idx,
                      msg, TOAST_INFO, true);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (!vesper_gui_init()) {
        return 1;
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    VesperKaihenCtx kh;
    vesper_kaihen_open(&kh);
    VesperResidentState rs;
    vesper_fs_query_resident(&kh, &rs);
    VesperSaveCtx sv;
    vesper_save_init(&sv);

    VesperCatalog* cat = (VesperCatalog*)calloc(1, sizeof(VesperCatalog));
    VesperConsoleCatalog* cc = (VesperConsoleCatalog*)calloc(1, sizeof(VesperConsoleCatalog));
    if (!cat || !cc) {
        free(cat);
        free(cc);
        vesper_save_exit(&sv);
        vesper_kaihen_close(&kh);
        vesper_gui_exit();
        return 1;
    }

    vesper_console_scan(cc);
    vesper_scan_sd(cat, cc->selected_sw1_str, cc->selected_sw2_str);

    KaiVesperTab tab = TAB_LIBRARY;
    size_t host_idx = 0;
    size_t prof_idx = 0;
    HostFilterMode host_filter = HOST_FILTER_ALL;
    size_t ct_idx = 0;

    if (cat->host_count > 0) {
        vesper_save_refresh_owner(&sv, cat->hosts[0].title_id_str);
    }

    char status[256];
    VesperToastKind kind = TOAST_INFO;
    if (rs.installed) {
        snprintf(status, sizeof(status), "KaiVesper Ready! Pick any game & press [A] to hot-swap (no reboot).");
        kind = TOAST_OK;
    } else if (kh.open && kh.has_fs_helpers) {
        snprintf(status, sizeof(status), "Welcome to KaiVesper! %zu console title(s) detected in Hosts tab.", cc->count);
        kind = TOAST_INFO;
    } else {
        snprintf(status, sizeof(status), "Stock kaiHEN detected — patched kaiHEN needed for on-console FS install.");
        kind = TOAST_WARN;
    }

    vesper_gui_render(tab, &kh, &rs, &sv, cat, cc, host_idx, prof_idx, host_filter, ct_idx, status, kind, false);

    s32 prev_touch_count = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) break;

        bool dirty = false;

        /* Precompute filtered console titles for Hosts tab */
        size_t filtered_ct[VESPER_MAX_CONSOLE_TITLES];
        size_t filtered_ct_count = vesper_console_filter(cc, host_filter, filtered_ct, VESPER_MAX_CONSOLE_TITLES);
        if (filtered_ct_count > 0 && ct_idx >= filtered_ct_count) ct_idx = filtered_ct_count - 1;

        /* Touchscreen tap: sidebar tabs, library rows, or console hosts rows */
        HidTouchScreenState tstate = {0};
        if (hidGetTouchScreenStates(&tstate, 1)) {
            if (tstate.count > 0 && prev_touch_count == 0) {
                int tx = (int)tstate.touches[0].x;
                int ty = (int)tstate.touches[0].y;
                int hit_tab = vesper_gui_hit_test_tab(tx, ty);
                if (hit_tab >= 0 && hit_tab < TAB_COUNT) {
                    tab = (KaiVesperTab)hit_tab;
                    dirty = true;
                } else if (tab == TAB_LIBRARY) {
                    int hit = vesper_gui_hit_test_card(cat, host_idx, prof_idx, tx, ty);
                    if (hit >= 0 && (size_t)hit != prof_idx) {
                        prof_idx = (size_t)hit;
                        dirty = true;
                    }
                } else if (tab == TAB_HOSTS) {
                    int hit = vesper_gui_hit_test_host_row(filtered_ct_count, ct_idx, tx, ty);
                    if (hit >= 0 && (size_t)hit != ct_idx) {
                        ct_idx = (size_t)hit;
                        dirty = true;
                    }
                }
            }
            prev_touch_count = tstate.count;
        }

        /* L / R switch sidebar tabs */
        if (kDown & HidNpadButton_R) {
            tab = (KaiVesperTab)((tab + 1) % TAB_COUNT);
            dirty = true;
        } else if (kDown & HidNpadButton_L) {
            tab = (KaiVesperTab)((tab == 0) ? (TAB_COUNT - 1) : (tab - 1));
            dirty = true;
        }

        /* Vertical navigation */
        if (tab == TAB_LIBRARY && cat->host_count > 0 && cat->hosts[host_idx].profile_count > 0) {
            size_t count = cat->hosts[host_idx].profile_count;
            if (kDown & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
                prof_idx = (prof_idx + 1) % count;
                dirty = true;
            } else if (kDown & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
                prof_idx = (prof_idx == 0) ? (count - 1) : (prof_idx - 1);
                dirty = true;
            }
        } else if (tab == TAB_HOSTS && filtered_ct_count > 0) {
            if (kDown & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
                ct_idx = (ct_idx + 1) % filtered_ct_count;
                dirty = true;
            } else if (kDown & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
                ct_idx = (ct_idx == 0) ? (filtered_ct_count - 1) : (ct_idx - 1);
                dirty = true;
            }
        }

        /* Hosts Tab actions: [A] Set as Host, [X] Cycle Filter, [Y] Rescan */
        if (tab == TAB_HOSTS) {
            if ((kDown & HidNpadButton_A) && filtered_ct_count > 0) {
                const VesperConsoleTitle* t = &cc->titles[filtered_ct[ct_idx]];
                char err[160] = {0};
                if (vesper_console_select_host(cc, t, err, sizeof(err))) {
                    vesper_scan_sd(cat, cc->selected_sw1_str, cc->selected_sw2_str);
                    if (host_idx >= cat->host_count) host_idx = 0;
                    if (cat->host_count > 0 && prof_idx >= cat->hosts[host_idx].profile_count) prof_idx = 0;
                    snprintf(status, sizeof(status),
                             "%s Host set: '%s'! Created /ns2jb-modloader/contents/%s/games/",
                             t->is_switch1 ? "SW1" : "SW2", t->name, t->title_id_str);
                    kind = TOAST_OK;
                } else {
                    snprintf(status, sizeof(status), "Host select failed: %.180s", err);
                    kind = TOAST_ERR;
                }
                dirty = true;
            } else if (kDown & HidNpadButton_X) {
                host_filter = (HostFilterMode)((host_filter + 1) % 3);
                ct_idx = 0;
                snprintf(status, sizeof(status), "Filter: %s",
                         host_filter == HOST_FILTER_SW1 ? "Switch 1 (01...) only"
                         : host_filter == HOST_FILTER_SW2 ? "Switch 2 (04...) only"
                         : "All console titles (SW1 + SW2)");
                kind = TOAST_INFO;
                dirty = true;
            } else if (kDown & HidNpadButton_Y) {
                snprintf(status, sizeof(status), "Scanning installed Switch 1 & Switch 2 titles...");
                vesper_gui_render(tab, &kh, &rs, &sv, cat, cc, host_idx, prof_idx, host_filter, ct_idx, status, TOAST_INFO, true);
                vesper_console_scan(cc);
                vesper_scan_sd(cat, cc->selected_sw1_str, cc->selected_sw2_str);
                ct_idx = 0;
                snprintf(status, sizeof(status), "Found %zu installed title(s) on console.", cc->count);
                kind = TOAST_OK;
                dirty = true;
            }
            goto next_frame;
        }

        /* Account switch (ZL / ZR) and Host cycle (ZR in Library if >1 host) */
        if ((kDown & HidNpadButton_ZL) && sv.user_count > 1) {
            sv.selected_user = (sv.selected_user + 1) % sv.user_count;
            if (cat->host_count > 0) vesper_save_refresh_owner(&sv, cat->hosts[host_idx].title_id_str);
            snprintf(status, sizeof(status), "Account: %s  (current save slot: %s)",
                     sv.users[sv.selected_user].nickname, sv.active_owner);
            kind = TOAST_INFO;
            dirty = true;
        } else if (kDown & HidNpadButton_ZR) {
            if (tab == TAB_LIBRARY && cat->host_count > 1) {
                host_idx = (host_idx + 1) % cat->host_count;
                prof_idx = 0;
                vesper_save_refresh_owner(&sv, cat->hosts[host_idx].title_id_str);
                const char* hn = vesper_console_lookup_name(cc, cat->hosts[host_idx].title_id_str);
                snprintf(status, sizeof(status), "Switched Host: %s (%s)",
                         hn ? hn : cat->hosts[host_idx].title_id_str,
                         cat->hosts[host_idx].is_switch1 ? "SW1" : "SW2");
                kind = TOAST_INFO;
                dirty = true;
            } else if (sv.user_count > 1) {
                sv.selected_user = (sv.selected_user == 0) ? (sv.user_count - 1) : (sv.selected_user - 1);
                if (cat->host_count > 0) vesper_save_refresh_owner(&sv, cat->hosts[host_idx].title_id_str);
                snprintf(status, sizeof(status), "Account: %s  (current save slot: %s)",
                         sv.users[sv.selected_user].nickname, sv.active_owner);
                kind = TOAST_INFO;
                dirty = true;
            }
        } else if ((kDown & HidNpadButton_Minus) || (tab == TAB_SYSTEM && (kDown & HidNpadButton_A))) {
            sv.auto_swap_enabled = !sv.auto_swap_enabled;
            snprintf(status, sizeof(status), "Auto Save-Swap: %s",
                     sv.auto_swap_enabled ? "ON (same-account save isolation active)" : "OFF");
            kind = sv.auto_swap_enabled ? TOAST_OK : TOAST_WARN;
            dirty = true;
        } else if (kDown & HidNpadButton_B) {
            snprintf(status, sizeof(status), "Running read-only FS & Save check...");
            vesper_gui_render(tab, &kh, &rs, &sv, cat, cc, host_idx, prof_idx, host_filter, ct_idx, status, TOAST_INFO, true);
            VesperFsDiscovery disc;
            bool fs_ok = vesper_fs_discover(&kh, &disc, false);
            size_t sv_files = 0;
            uint64_t sv_bytes = 0;
            char sv_err[128] = {0};
            bool sv_ok = (cat->host_count > 0) &&
                vesper_save_probe_readonly(&sv, cat->hosts[host_idx].title_id,
                                           cat->hosts[host_idx].title_id_str,
                                           &sv_files, &sv_bytes, sv_err, sizeof(sv_err));
            if (fs_ok && sv_ok) {
                char sz[24];
                vesper_fmt_size(sv_bytes, sz, sizeof(sz));
                snprintf(status, sizeof(status), "ALL OK! FS base=%llx | save:/ %zu file(s) (%s)",
                         (unsigned long long)disc.fs_base, sv_files, sz);
                kind = TOAST_OK;
            } else if (fs_ok) {
                snprintf(status, sizeof(status), "FS OK | Save check: %.150s", sv_err);
                kind = TOAST_WARN;
            } else {
                snprintf(status, sizeof(status), "FS Check failed: %.180s", disc.error);
                kind = TOAST_ERR;
            }
            dirty = true;
        } else if (kDown & HidNpadButton_Y) {
            vesper_console_scan(cc);
            vesper_scan_sd(cat, cc->selected_sw1_str, cc->selected_sw2_str);
            vesper_fs_query_resident(&kh, &rs);
            if (host_idx >= cat->host_count) host_idx = 0;
            if (cat->host_count > 0) {
                if (prof_idx >= cat->hosts[host_idx].profile_count) prof_idx = 0;
                vesper_save_refresh_owner(&sv, cat->hosts[host_idx].title_id_str);
            }
            snprintf(status, sizeof(status), "Refreshed SD library, Console Hosts (%zu) & FS status.", cc->count);
            kind = TOAST_INFO;
            dirty = true;
        } else if (tab == TAB_LIBRARY && (kDown & HidNpadButton_A) && cat->host_count > 0) {
            const VesperHost* h = &cat->hosts[host_idx];
            if (h->profile_count > 0) {
                const VesperProfile* p = &h->profiles[prof_idx];
                char err[200] = {0};

                /* Step 0: Auto-pack loose romfs/ -> romfs.bin on-console when needed */
                if (!p->has_romfs_bin && p->has_romfs_dir) {
                    size_t loose_files = 0;
                    uint64_t loose_bytes = 0;
                    vesper_romfs_count_dir(p->romfs_dir_mount, 81, &loose_files, &loose_bytes);
                    bool need_pack = p->has_exefs_nsp || p->has_exefs_dir || (loose_files > 80);
                    if (need_pack) {
                        char out_img[VESPER_MAX_PATH];
                        char saved_tid[17], saved_name[VESPER_MAX_NAME];
                        snprintf(out_img, sizeof(out_img), "%s/romfs.bin", p->dir_mount);
                        snprintf(saved_tid, sizeof(saved_tid), "%s", h->title_id_str);
                        snprintf(saved_name, sizeof(saved_name), "%s", p->name);

                        snprintf(status, sizeof(status), "Scanning romfs/ for '%s' (first-time one-time pack)...", p->name);
                        vesper_gui_render(tab, &kh, &rs, &sv, cat, cc, host_idx, prof_idx, host_filter, ct_idx, status, TOAST_INFO, true);

                        RomfsUiProgressCtx pctx = {tab, &kh, &rs, &sv, cat, cc, host_idx, prof_idx, host_filter, ct_idx, saved_name};
                        if (!vesper_romfs_build_image(p->romfs_dir_mount, out_img, on_romfs_pack_progress, &pctx, err, sizeof(err))) {
                            snprintf(status, sizeof(status), "RomFS pack failed: %.190s", err);
                            kind = TOAST_ERR;
                            dirty = true;
                            goto next_frame;
                        }

                        vesper_scan_sd(cat, cc->selected_sw1_str, cc->selected_sw2_str);
                        host_idx = 0;
                        prof_idx = 0;
                        for (size_t hi = 0; hi < cat->host_count; hi++) {
                            if (strcmp(cat->hosts[hi].title_id_str, saved_tid) == 0) {
                                host_idx = hi;
                                for (size_t pi = 0; pi < cat->hosts[hi].profile_count; pi++) {
                                    if (strcmp(cat->hosts[hi].profiles[pi].name, saved_name) == 0) {
                                        prof_idx = pi;
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                        if (cat->host_count == 0 || cat->hosts[host_idx].profile_count == 0) {
                            snprintf(status, sizeof(status), "RomFS built, but profile vanished on rescan.");
                            kind = TOAST_ERR;
                            dirty = true;
                            goto next_frame;
                        }
                        h = &cat->hosts[host_idx];
                        p = &h->profiles[prof_idx];
                    }
                }

                /* Determine effective Host: for loose exefs/ (no baked main.npdm), honor active Console Host choice */
                uint64_t eff_tid = h->title_id;
                const char* eff_tid_str = h->title_id_str;
                if (!p->has_exefs_nsp) {
                    if (h->is_switch1 && cc->selected_sw1_tid != 0) {
                        eff_tid = cc->selected_sw1_tid;
                        eff_tid_str = cc->selected_sw1_str;
                    } else if (!h->is_switch1 && cc->selected_sw2_tid != 0) {
                        eff_tid = cc->selected_sw2_tid;
                        eff_tid_str = cc->selected_sw2_str;
                    }
                }

                /* Step 1: Per-profile save isolation swap (if enabled) */
                if (sv.auto_swap_enabled && sv.user_count > 0) {
                    snprintf(status, sizeof(status), "Backing up & swapping saves for '%s'...", p->name);
                    vesper_gui_render(tab, &kh, &rs, &sv, cat, cc, host_idx, prof_idx, host_filter, ct_idx, status, TOAST_INFO, true);
                    char save_err[140] = {0};
                    if (!vesper_save_swap_profile(&sv, eff_tid, eff_tid_str, p->name, save_err, sizeof(save_err))) {
                        snprintf(status, sizeof(status), "Save swap aborted: %.160s ([-] to bypass)", save_err);
                        kind = TOAST_ERR;
                        dirty = true;
                        goto next_frame;
                    }
                }

                /* Step 2: Install or hot-swap in FS */
                if (kh.open && kh.has_fs_helpers) {
                    bool was_swap = false;
                    snprintf(status, sizeof(status), "%s '%s' in FS...",
                             rs.installed ? "Hot-swapping" : "Installing", p->name);
                    vesper_gui_render(tab, &kh, &rs, &sv, cat, cc, host_idx, prof_idx, host_filter, ct_idx, status, TOAST_INFO, true);
                    if (vesper_fs_install_or_swap(&kh, eff_tid, eff_tid_str, p, &was_swap, err, sizeof(err))) {
                        vesper_fs_query_resident(&kh, &rs);
                        const char* hn = vesper_console_lookup_name(cc, eff_tid_str);
                        snprintf(status, sizeof(status),
                                 "%s '%s' ready! Press [+] & launch %s!",
                                 was_swap ? "HOT-SWAPPED!" : "READY!",
                                 p->name,
                                 hn ? hn : eff_tid_str);
                        kind = TOAST_OK;
                    } else {
                        snprintf(status, sizeof(status), "FS failed: %.200s", err);
                        kind = TOAST_ERR;
                    }
                } else {
                    int rc = vesper_activate_profile(h, p, err, sizeof(err));
                    if (rc == 0) {
                        snprintf(status, sizeof(status), "Staged '%s' on SD (patched kaiHEN required for FS).", p->name);
                        kind = TOAST_WARN;
                        vesper_scan_sd(cat, cc->selected_sw1_str, cc->selected_sw2_str);
                    } else {
                        snprintf(status, sizeof(status), "Staging failed: %.200s", err);
                        kind = TOAST_ERR;
                    }
                }
                dirty = true;
            }
        } else if ((tab == TAB_LIBRARY || tab == TAB_SYSTEM) && (kDown & HidNpadButton_X) && cat->host_count > 0) {
            const VesperHost* h = &cat->hosts[host_idx];
            char err[200] = {0};

            if (sv.auto_swap_enabled && sv.user_count > 0) {
                snprintf(status, sizeof(status), "Restoring _stock save for %s...", h->title_id_str);
                vesper_gui_render(tab, &kh, &rs, &sv, cat, cc, host_idx, prof_idx, host_filter, ct_idx, status, TOAST_INFO, true);
                if (!vesper_save_swap_profile(&sv, h->title_id, h->title_id_str, "_stock", err, sizeof(err))) {
                    snprintf(status, sizeof(status), "Restore _stock save failed: %.180s", err);
                    kind = TOAST_ERR;
                    dirty = true;
                    goto next_frame;
                }
            }

            if (kh.open && kh.has_fs_helpers && rs.installed) {
                if (vesper_fs_hot_disable(&kh, err, sizeof(err))) {
                    vesper_fs_query_resident(&kh, &rs);
                    snprintf(status, sizeof(status),
                             (sv.auto_swap_enabled && sv.user_count > 0)
                                 ? "Stock mode active (_stock save restored, no reboot)!"
                                 : "Stock mode active (no reboot)!");
                    kind = TOAST_WARN;
                } else {
                    snprintf(status, sizeof(status), "Disable failed: %.200s", err);
                    kind = TOAST_ERR;
                }
            } else {
                int rc = vesper_deactivate_profile(h, err, sizeof(err));
                if (rc == 0) {
                    snprintf(status, sizeof(status), "Unstaged SD profile for %s.", h->title_id_str);
                    kind = TOAST_INFO;
                    vesper_scan_sd(cat, cc->selected_sw1_str, cc->selected_sw2_str);
                } else {
                    snprintf(status, sizeof(status), "Unstage failed: %.200s", err);
                    kind = TOAST_ERR;
                }
            }
            dirty = true;
        }

    next_frame:
        if (dirty) {
            vesper_gui_render(tab, &kh, &rs, &sv, cat, cc, host_idx, prof_idx, host_filter, ct_idx, status, kind, false);
        }
    }

    free(cc);
    free(cat);
    vesper_save_exit(&sv);
    vesper_kaihen_close(&kh);
    vesper_gui_exit();
    return 0;
}
