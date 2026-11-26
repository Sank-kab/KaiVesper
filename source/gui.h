/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "console_titles.h"
#include "fs_installer.h"
#include "kaihen_ro.h"
#include "save_manager.h"
#include "sd_scanner.h"
#include <stdbool.h>
#include <stddef.h>

#define GUI_WIDTH  1280
#define GUI_HEIGHT 720

typedef enum {
    TAB_LIBRARY = 0,
    TAB_HOSTS   = 1,
    TAB_SYSTEM  = 2,
    TAB_GUIDE   = 3,
    TAB_COUNT   = 4
} KaiVesperTab;

typedef enum {
    TOAST_INFO = 0,
    TOAST_OK   = 1,
    TOAST_WARN = 2,
    TOAST_ERR  = 3
} VesperToastKind;

bool vesper_gui_init(void);
void vesper_gui_exit(void);

void vesper_gui_render(KaiVesperTab active_tab,
                       const VesperKaihenCtx* kh,
                       const VesperResidentState* rs,
                       const VesperSaveCtx* sv,
                       const VesperCatalog* cat,
                       const VesperConsoleCatalog* cc,
                       size_t host_idx,
                       size_t prof_idx,
                       HostFilterMode host_filter,
                       size_t ct_idx,
                       const char* toast_msg,
                       VesperToastKind toast_kind,
                       bool busy);

/* Touch hit-tests: return tab index (0..3) or -1; card/row index or -1. */
int vesper_gui_hit_test_tab(int tx, int ty);
int vesper_gui_hit_test_card(const VesperCatalog* cat, size_t host_idx, size_t prof_idx, int tx, int ty);
int vesper_gui_hit_test_host_row(size_t filtered_count, size_t ct_idx, int tx, int ty);
