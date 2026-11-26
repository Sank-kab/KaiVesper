/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VESPER_MAX_HOSTS       16
#define VESPER_MAX_PROFILES    32
#define VESPER_MAX_PATCHES     16
#define VESPER_MAX_EXEFS_FILES 16
#define VESPER_MAX_PATH        256
#define VESPER_MAX_NAME        96

typedef struct {
    char name[VESPER_MAX_NAME];       /* e.g. exefs_patches/<BUILD_ID>.ips32 */
    char sd_path[VESPER_MAX_PATH];    /* e.g. /ns2jb-modloader/contents/<TID>/games/<P>/exefs_patches/<ID>.ips32 */
    uint64_t size;
} VesperPatchFile;

typedef struct {
    char name[64];                    /* e.g. rtld, main, subsdk0, sdk */
    char sd_path[VESPER_MAX_PATH];    /* e.g. /ns2jb-modloader/contents/<TID>/games/<P>/exefs/main */
    uint64_t size;
} VesperExefsFile;

typedef struct {
    char name[VESPER_MAX_NAME];       /* profile folder name or "(direct)" */
    char dir_path[VESPER_MAX_PATH];   /* /ns2jb-modloader/contents/<TID>/games/<P> */
    char dir_mount[VESPER_MAX_PATH];  /* sdmc:/ns2jb-modloader/contents/<TID>/games/<P> */

    bool has_exefs_nsp;
    char exefs_nsp_path[VESPER_MAX_PATH];
    uint64_t exefs_nsp_size;

    bool has_exefs_dir;
    VesperExefsFile exefs_files[VESPER_MAX_EXEFS_FILES];
    size_t exefs_file_count;
    uint64_t exefs_dir_size;

    bool has_romfs_bin;
    char romfs_bin_path[VESPER_MAX_PATH];
    uint64_t romfs_bin_size;

    bool has_romfs_dir;
    char romfs_dir_mount[VESPER_MAX_PATH]; /* sdmc:/.../romfs */
    char romfs_dir_rel[VESPER_MAX_PATH];   /* /ns2jb-modloader/.../romfs */

    VesperPatchFile patches[VESPER_MAX_PATCHES];
    size_t patch_count;
    bool ok;
} VesperProfile;

typedef struct {
    char title_id_str[17];            /* 16-hex lowercase (01... = Switch 1, 04... = Switch 2) */
    uint64_t title_id;
    bool is_switch1;
    VesperProfile profiles[VESPER_MAX_PROFILES];
    size_t profile_count;
} VesperHost;

typedef struct {
    VesperHost hosts[VESPER_MAX_HOSTS];
    size_t host_count;
} VesperCatalog;

/* Scans sdmc:/ns2jb-modloader/contents/<HOST_TITLE_ID>/games/<PROFILE>/
 * plus universal sdmc:/ns2jb-modloader/sw1/<PROFILE>/ and sw2/<PROFILE>/ bound to selected hosts. */
int vesper_scan_sd(VesperCatalog* out, const char* sw1_host_tid, const char* sw2_host_tid);

/* Formats a byte count into a human-readable string like "15.6 GB" or "4.2 MB". */
void vesper_fmt_size(uint64_t bytes, char* buf, size_t buf_len);
