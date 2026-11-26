/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <switch.h>

#define VESPER_MAX_CONSOLE_TITLES 128

typedef enum {
    HOST_FILTER_ALL = 0,
    HOST_FILTER_SW1 = 1,
    HOST_FILTER_SW2 = 2
} HostFilterMode;

typedef struct {
    uint64_t title_id;
    char title_id_str[17];
    char name[96];
    bool is_switch1;
    bool has_sd_folder;
    int8_t logo_handling; /* -1=unknown, 0=Auto (best Host), 1=Manual (may keep boot logo) */
} VesperConsoleTitle;

typedef struct {
    bool ns_ok;
    Result ns_rc;
    VesperConsoleTitle titles[VESPER_MAX_CONSOLE_TITLES];
    size_t count;

    uint64_t selected_sw1_tid;
    char selected_sw1_str[17];
    char selected_sw1_name[96];

    uint64_t selected_sw2_tid;
    char selected_sw2_str[17];
    char selected_sw2_name[96];
} VesperConsoleCatalog;

/* Scans installed Switch 1 & Switch 2 applications via ns:am and loads saved Host preferences. */
void vesper_console_scan(VesperConsoleCatalog* out);

/* Sets the chosen installed title as the active SW1 or SW2 Host, saves to sdmc:/vesper/hosts.cfg,
 * and creates sdmc:/ns2jb-modloader/contents/<TID>/games/ on the microSD. */
bool vesper_console_select_host(VesperConsoleCatalog* cat, const VesperConsoleTitle* title,
                                char* err_buf, size_t err_len);

/* Looks up a human-readable title name for a 16-hex Title ID (returns NULL if unknown). */
const char* vesper_console_lookup_name(const VesperConsoleCatalog* cat, const char* title_id_str);

/* Returns filtered indices matching HostFilterMode into out_indices (up to max_out). */
size_t vesper_console_filter(const VesperConsoleCatalog* cat, HostFilterMode mode,
                             size_t* out_indices, size_t max_out);
