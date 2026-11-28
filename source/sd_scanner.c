/* SPDX-License-Identifier: GPL-2.0-only */
#include "sd_scanner.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SD_ROOT_MOUNT "sdmc:/ns2jb-modloader"
#define SD_ROOT_REL   "/ns2jb-modloader"

static bool is_title_id(const char* s) {
    if (!s || strlen(s) != 16) return false;
    if (s[0] != '0' || (s[1] != '1' && s[1] != '4')) return false;
    for (int i = 0; i < 16; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static bool ends_with_ci(const char* str, const char* suffix) {
    size_t sl = strlen(str), fl = strlen(suffix);
    if (fl > sl) return false;
    for (size_t i = 0; i < fl; i++) {
        if (tolower((unsigned char)str[sl - fl + i]) != tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

static bool str_eq_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false;
    }
    return *a == '\0' && *b == '\0';
}

static bool is_ignored(const char* name) {
    return !name || name[0] == '.' ||
           strcmp(name, ".gitkeep") == 0 ||
           strcmp(name, ".DS_Store") == 0 ||
           strcmp(name, "Thumbs.db") == 0;
}

static bool file_has_magic4(const char* path, const char* expected) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t magic[4] = {0};
    bool ok = (fread(magic, 1, 4, f) == 4 && memcmp(magic, expected, 4) == 0);
    fclose(f);
    return ok;
}

static bool file_has_nso_magic(const char* path) {
    return file_has_magic4(path, "NSO0");
}

static bool dir_has_any_entry(const char* path) {
    DIR* d = opendir(path);
    if (!d) return false;
    struct dirent* ent;
    bool found = false;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static int cmp_profiles(const void* a, const void* b) {
    const VesperProfile* pa = (const VesperProfile*)a;
    const VesperProfile* pb = (const VesperProfile*)b;
    return strcmp(pa->name, pb->name);
}

static int cmp_hosts(const void* a, const void* b) {
    const VesperHost* ha = (const VesperHost*)a;
    const VesperHost* hb = (const VesperHost*)b;
    return strcmp(ha->title_id_str, hb->title_id_str);
}

static int cmp_exefs(const void* a, const void* b) {
    const VesperExefsFile* ea = (const VesperExefsFile*)a;
    const VesperExefsFile* eb = (const VesperExefsFile*)b;
    return strcmp(ea->name, eb->name);
}

static void scan_exefs_dir(VesperProfile* prof, const char* mount_dir, const char* rel_dir) {
    DIR* d = opendir(mount_dir);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL && prof->exefs_file_count < VESPER_MAX_EXEFS_FILES) {
        if (is_ignored(ent->d_name)) continue;
        /* NEVER replace Host's signed main.npdm — keep Host's Program ID & ACID policy intact! */
        if (str_eq_ci(ent->d_name, "main.npdm")) continue;

        char full[VESPER_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", mount_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        /* Optional 128-byte unsigned META header (meta.bin) for automatic SystemResourceSize / Stack merge */
        if (str_eq_ci(ent->d_name, "meta.bin")) {
            if (st.st_size >= 0x80 && file_has_magic4(full, "META")) {
                VesperExefsFile* ef = &prof->exefs_files[prof->exefs_file_count++];
                snprintf(ef->name, sizeof(ef->name), "__meta.bin");
                snprintf(ef->sd_path, sizeof(ef->sd_path), "%s/%s", rel_dir, ent->d_name);
                ef->size = (uint64_t)st.st_size;
            }
            continue;
        }

        if (st.st_size < 0x100) continue;
        if (!file_has_nso_magic(full)) continue;

        VesperExefsFile* ef = &prof->exefs_files[prof->exefs_file_count++];
        snprintf(ef->name, sizeof(ef->name), "%s", ent->d_name);
        snprintf(ef->sd_path, sizeof(ef->sd_path), "%s/%s", rel_dir, ent->d_name);
        ef->size = (uint64_t)st.st_size;
        prof->exefs_dir_size += ef->size;
    }
    closedir(d);
    if (prof->exefs_file_count > 1) {
        qsort(prof->exefs_files, prof->exefs_file_count, sizeof(VesperExefsFile), cmp_exefs);
    }
    prof->has_exefs_dir = (prof->exefs_file_count > 0);
}

static void scan_patches_dir(VesperProfile* prof, const char* mount_dir, const char* rel_dir, const char* prefix) {
    DIR* d = opendir(mount_dir);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL && prof->patch_count < VESPER_MAX_PATCHES) {
        if (is_ignored(ent->d_name)) continue;
        char full[VESPER_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", mount_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            char sub_rel[VESPER_MAX_PATH];
            char sub_pre[VESPER_MAX_NAME];
            snprintf(sub_rel, sizeof(sub_rel), "%s/%s", rel_dir, ent->d_name);
            snprintf(sub_pre, sizeof(sub_pre), "%s/%s", prefix, ent->d_name);
            scan_patches_dir(prof, full, sub_rel, sub_pre);
        } else if (S_ISREG(st.st_mode) && (ends_with_ci(ent->d_name, ".ips") || ends_with_ci(ent->d_name, ".ips32"))) {
            VesperPatchFile* pf = &prof->patches[prof->patch_count++];
            snprintf(pf->name, sizeof(pf->name), "%s/%s", prefix, ent->d_name);
            snprintf(pf->sd_path, sizeof(pf->sd_path), "%s/%s", rel_dir, ent->d_name);
            pf->size = (uint64_t)st.st_size;
        }
    }
    closedir(d);
}

static bool scan_profile_dir(VesperProfile* prof, const char* name, const char* mount_dir, const char* rel_dir) {
    memset(prof, 0, sizeof(*prof));
    snprintf(prof->name, sizeof(prof->name), "%s", name);
    snprintf(prof->dir_path, sizeof(prof->dir_path), "%s", rel_dir);
    snprintf(prof->dir_mount, sizeof(prof->dir_mount), "%s", mount_dir);

    char p[VESPER_MAX_PATH];
    struct stat st;

    snprintf(p, sizeof(p), "%s/exefs.nsp", mount_dir);
    if (stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        prof->has_exefs_nsp = true;
        prof->exefs_nsp_size = (uint64_t)st.st_size;
        snprintf(prof->exefs_nsp_path, sizeof(prof->exefs_nsp_path), "%s/exefs.nsp", rel_dir);
    }

    char exefs_mount[VESPER_MAX_PATH], exefs_rel[VESPER_MAX_PATH];
    snprintf(exefs_mount, sizeof(exefs_mount), "%s/exefs", mount_dir);
    snprintf(exefs_rel, sizeof(exefs_rel), "%s/exefs", rel_dir);
    if (stat(exefs_mount, &st) == 0 && S_ISDIR(st.st_mode)) {
        scan_exefs_dir(prof, exefs_mount, exefs_rel);
    }

    snprintf(p, sizeof(p), "%s/romfs.bin", mount_dir);
    if (stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        prof->has_romfs_bin = true;
        prof->romfs_bin_size = (uint64_t)st.st_size;
        snprintf(prof->romfs_bin_path, sizeof(prof->romfs_bin_path), "%s/romfs.bin", rel_dir);
    }

    char romfs_mount[VESPER_MAX_PATH], romfs_rel[VESPER_MAX_PATH];
    snprintf(romfs_mount, sizeof(romfs_mount), "%s/romfs", mount_dir);
    snprintf(romfs_rel, sizeof(romfs_rel), "%s/romfs", rel_dir);
    if (stat(romfs_mount, &st) == 0 && S_ISDIR(st.st_mode) && dir_has_any_entry(romfs_mount)) {
        prof->has_romfs_dir = true;
        snprintf(prof->romfs_dir_mount, sizeof(prof->romfs_dir_mount), "%s", romfs_mount);
        snprintf(prof->romfs_dir_rel, sizeof(prof->romfs_dir_rel), "%s", romfs_rel);
    }

    char patch_mount[VESPER_MAX_PATH], patch_rel[VESPER_MAX_PATH];
    snprintf(patch_mount, sizeof(patch_mount), "%s/exefs_patches", mount_dir);
    snprintf(patch_rel, sizeof(patch_rel), "%s/exefs_patches", rel_dir);
    scan_patches_dir(prof, patch_mount, patch_rel, "exefs_patches");

    prof->ok = prof->has_exefs_nsp || prof->has_exefs_dir ||
               prof->has_romfs_bin || prof->has_romfs_dir ||
               prof->patch_count > 0;
    return prof->ok;
}

static VesperHost* find_or_add_host(VesperCatalog* out, const char* tid_str) {
    if (!tid_str || !is_title_id(tid_str)) return NULL;
    char low[17];
    for (int i = 0; i < 16; i++) low[i] = (char)tolower((unsigned char)tid_str[i]);
    low[16] = '\0';
    for (size_t i = 0; i < out->host_count; i++) {
        if (strcmp(out->hosts[i].title_id_str, low) == 0) return &out->hosts[i];
    }
    if (out->host_count >= VESPER_MAX_HOSTS) return NULL;
    VesperHost* h = &out->hosts[out->host_count++];
    memset(h, 0, sizeof(*h));
    memcpy(h->title_id_str, low, 17);
    h->title_id = strtoull(low, NULL, 16);
    h->is_switch1 = (low[0] == '0' && low[1] == '1');
    return h;
}

static void scan_universal_folder(VesperCatalog* out, const char* sub, const char* host_tid) {
    if (!host_tid || !is_title_id(host_tid)) return;
    char mount_dir[VESPER_MAX_PATH], rel_dir[VESPER_MAX_PATH];
    snprintf(mount_dir, sizeof(mount_dir), "%s/%s", SD_ROOT_MOUNT, sub);
    snprintf(rel_dir, sizeof(rel_dir), "%s/%s", SD_ROOT_REL, sub);

    DIR* d = opendir(mount_dir);
    if (!d) return;

    VesperProfile* tmp_prof = (VesperProfile*)calloc(1, sizeof(VesperProfile));
    if (!tmp_prof) { closedir(d); return; }

    VesperHost* host = NULL;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (is_ignored(ent->d_name)) continue;
        char pm[VESPER_MAX_PATH], pr[VESPER_MAX_PATH];
        snprintf(pm, sizeof(pm), "%s/%s", mount_dir, ent->d_name);
        snprintf(pr, sizeof(pr), "%s/%s", rel_dir, ent->d_name);
        struct stat st;
        if (stat(pm, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (!scan_profile_dir(tmp_prof, ent->d_name, pm, pr)) continue;
        if (!host) host = find_or_add_host(out, host_tid);
        if (!host || host->profile_count >= VESPER_MAX_PROFILES) break;
        host->profiles[host->profile_count++] = *tmp_prof;
    }
    free(tmp_prof);
    closedir(d);

    if (host && host->profile_count > 1) {
        qsort(host->profiles, host->profile_count, sizeof(VesperProfile), cmp_profiles);
    }
}

int vesper_scan_sd(VesperCatalog* out, const char* sw1_host_tid, const char* sw2_host_tid) {
    memset(out, 0, sizeof(*out));
    char contents_dir[VESPER_MAX_PATH];
    snprintf(contents_dir, sizeof(contents_dir), "%s/contents", SD_ROOT_MOUNT);

    DIR* cd = opendir(contents_dir);
    if (cd) {
        struct dirent* ent;
        while ((ent = readdir(cd)) != NULL && out->host_count < VESPER_MAX_HOSTS) {
            if (!is_title_id(ent->d_name)) continue;

            char host_mount[VESPER_MAX_PATH], host_rel[VESPER_MAX_PATH];
            snprintf(host_mount, sizeof(host_mount), "%s/%s", contents_dir, ent->d_name);
            snprintf(host_rel, sizeof(host_rel), "%s/contents/%s", SD_ROOT_REL, ent->d_name);

            struct stat st;
            if (stat(host_mount, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            VesperHost* host = &out->hosts[out->host_count];
            memset(host, 0, sizeof(*host));
            for (int i = 0; i < 16; i++) host->title_id_str[i] = (char)tolower((unsigned char)ent->d_name[i]);
            host->title_id_str[16] = '\0';
            host->title_id = strtoull(host->title_id_str, NULL, 16);
            host->is_switch1 = (host->title_id_str[0] == '0' && host->title_id_str[1] == '1');

            /* Scan games/<profile>/ folders */
            char games_mount[VESPER_MAX_PATH], games_rel[VESPER_MAX_PATH];
            snprintf(games_mount, sizeof(games_mount), "%s/games", host_mount);
            snprintf(games_rel, sizeof(games_rel), "%s/games", host_rel);
            DIR* gd = opendir(games_mount);
            if (gd) {
                struct dirent* gent;
                while ((gent = readdir(gd)) != NULL && host->profile_count < VESPER_MAX_PROFILES) {
                    if (is_ignored(gent->d_name)) continue;
                    char pm[VESPER_MAX_PATH], pr[VESPER_MAX_PATH];
                    snprintf(pm, sizeof(pm), "%s/%s", games_mount, gent->d_name);
                    snprintf(pr, sizeof(pr), "%s/%s", games_rel, gent->d_name);
                    struct stat pst;
                    if (stat(pm, &pst) != 0 || !S_ISDIR(pst.st_mode)) continue;
                    if (scan_profile_dir(&host->profiles[host->profile_count], gent->d_name, pm, pr)) {
                        host->profile_count++;
                    }
                }
                closedir(gd);
            }

            /* Also check direct files at contents/<TID>/ (recover real name from .vesper_active if present) */
            if (host->profile_count < VESPER_MAX_PROFILES) {
                char direct_name[VESPER_MAX_NAME] = "Root Profile";
                char marker[VESPER_MAX_PATH];
                snprintf(marker, sizeof(marker), "%s/.vesper_active", host_mount);
                FILE* mf = fopen(marker, "r");
                if (mf) {
                    char line[VESPER_MAX_NAME] = {0};
                    if (fgets(line, sizeof(line), mf)) {
                        size_t n = strlen(line);
                        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
                        if (line[0]) snprintf(direct_name, sizeof(direct_name), "%s", line);
                    }
                    fclose(mf);
                }
                if (scan_profile_dir(&host->profiles[host->profile_count], direct_name, host_mount, host_rel)) {
                    host->profile_count++;
                }
            }

            if (host->profile_count > 0) {
                qsort(host->profiles, host->profile_count, sizeof(VesperProfile), cmp_profiles);
                out->host_count++;
            }
        }
        closedir(cd);
    }

    /* Also scan universal drop-in folders bound to the user's selected Console Hosts */
    scan_universal_folder(out, "sw1", sw1_host_tid);
    scan_universal_folder(out, "sw2", sw2_host_tid);

    if (out->host_count > 1) {
        qsort(out->hosts, out->host_count, sizeof(VesperHost), cmp_hosts);
    }
    return 0;
}

void vesper_fmt_size(uint64_t bytes, char* buf, size_t buf_len) {
    if (bytes >= (1024ULL * 1024ULL * 1024ULL)) {
        snprintf(buf, buf_len, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (1024ULL * 1024ULL)) {
        snprintf(buf, buf_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, buf_len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, buf_len, "%llu B", (unsigned long long)bytes);
    }
}
