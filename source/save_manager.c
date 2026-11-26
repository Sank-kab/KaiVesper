/* SPDX-License-Identifier: GPL-2.0-only */
#include "save_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAVE_DEV_NAME  "save"
#define SAVE_MOUNT     "save:/"
#define SD_SAVES_ROOT  "sdmc:/vesper/saves"

static void join_path(char* out, size_t out_len, const char* dir, const char* name) {
    size_t dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] == '/') {
        snprintf(out, out_len, "%s%s", dir, name);
    } else {
        snprintf(out, out_len, "%s/%s", dir, name);
    }
}

static void slog(FILE* f, const char* fmt, ...) {
    if (!f) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    fprintf(f, "\n");
    fflush(f);
    va_end(a);
}

static void format_uid_hex(AccountUid uid, char out[33]) {
    snprintf(out, 33, "%016llx%016llx",
             (unsigned long long)uid.uid[0], (unsigned long long)uid.uid[1]);
}

static void mkdir_p(const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static bool dir_exists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int count_tree(const char* dir, size_t* files, uint64_t* bytes) {
    DIR* d = opendir(dir);
    if (!d) return -1;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[512];
        join_path(child, sizeof(child), dir, ent->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            count_tree(child, files, bytes);
        } else if (S_ISREG(st.st_mode)) {
            if (files) (*files)++;
            if (bytes) (*bytes) += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    return 0;
}

static int copy_file_verified(const char* src, const char* dst, FILE* logf) {
    FILE* in = fopen(src, "rb");
    if (!in) {
        slog(logf, "  [ERR] open src '%s': %s", src, strerror(errno));
        return -1;
    }
    FILE* out = fopen(dst, "wb");
    if (!out) {
        slog(logf, "  [ERR] open dst '%s': %s", dst, strerror(errno));
        fclose(in);
        return -1;
    }

    static uint8_t buf[0x20000]; /* 128 KiB copy buffer */
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            slog(logf, "  [ERR] write dst '%s': %s", dst, strerror(errno));
            rc = -1;
            break;
        }
    }
    if (ferror(in)) {
        slog(logf, "  [ERR] read src '%s'", src);
        rc = -1;
    }
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

static int copy_tree(const char* src_dir, const char* dst_dir, size_t* copied_files, FILE* logf) {
    if (strcmp(dst_dir, SAVE_MOUNT) != 0) mkdir_p(dst_dir);
    DIR* d = opendir(src_dir);
    if (!d) return -1;
    struct dirent* ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char s[512], t[512];
        join_path(s, sizeof(s), src_dir, ent->d_name);
        join_path(t, sizeof(t), dst_dir, ent->d_name);
        struct stat st;
        if (stat(s, &st) != 0) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (copy_tree(s, t, copied_files, logf) != 0) { rc = -1; break; }
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file_verified(s, t, logf) != 0) { rc = -1; break; }
            if (copied_files) (*copied_files)++;
        }
    }
    closedir(d);
    return rc;
}

static int clear_dir_contents(const char* dir, bool remove_self) {
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[512];
        join_path(child, sizeof(child), dir, ent->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (clear_dir_contents(child, true) != 0) rc = -1;
        } else {
            if (unlink(child) != 0) rc = -1;
        }
    }
    closedir(d);
    if (remove_self) {
        if (rmdir(dir) != 0) rc = -1;
    }
    return rc;
}

bool vesper_save_init(VesperSaveCtx* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->auto_swap_enabled = true;
    snprintf(ctx->active_owner, sizeof(ctx->active_owner), "_stock");

    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) return false;

    ctx->initialized = true;

    AccountUid uids[VESPER_MAX_USERS];
    s32 total = 0;
    rc = accountListAllUsers(uids, VESPER_MAX_USERS, &total);
    if (R_SUCCEEDED(rc) && total > 0) {
        AccountUid presel = {0};
        bool has_presel = R_SUCCEEDED(accountGetPreselectedUser(&presel)) && accountUidIsValid(&presel);

        for (s32 i = 0; i < total && ctx->user_count < VESPER_MAX_USERS; i++) {
            if (!accountUidIsValid(&uids[i])) continue;
            VesperUser* u = &ctx->users[ctx->user_count];
            u->uid = uids[i];
            format_uid_hex(u->uid, u->uid_hex);
            snprintf(u->nickname, sizeof(u->nickname), "User %zu", ctx->user_count + 1);

            AccountProfile prof;
            if (R_SUCCEEDED(accountGetProfile(&prof, u->uid))) {
                AccountProfileBase base = {0};
                if (R_SUCCEEDED(accountProfileGet(&prof, NULL, &base)) && base.nickname[0]) {
                    snprintf(u->nickname, sizeof(u->nickname), "%.32s", base.nickname);
                }
                accountProfileClose(&prof);
            }

            if (has_presel && memcmp(&u->uid, &presel, sizeof(AccountUid)) == 0) {
                ctx->selected_user = ctx->user_count;
            }
            ctx->user_count++;
        }
    }

    if (ctx->user_count == 0) {
        AccountUid presel = {0};
        if (R_SUCCEEDED(accountGetPreselectedUser(&presel)) && accountUidIsValid(&presel)) {
            VesperUser* u = &ctx->users[0];
            u->uid = presel;
            format_uid_hex(u->uid, u->uid_hex);
            snprintf(u->nickname, sizeof(u->nickname), "Current User");
            ctx->user_count = 1;
            ctx->selected_user = 0;
        }
    }

    return ctx->user_count > 0;
}

void vesper_save_exit(VesperSaveCtx* ctx) {
    if (ctx && ctx->initialized) {
        accountExit();
        ctx->initialized = false;
    }
}

static void build_user_save_root(const VesperSaveCtx* ctx, const char* host_tid_str, char* out, size_t out_len) {
    const char* uid_hex = (ctx->user_count > 0) ? ctx->users[ctx->selected_user].uid_hex : "default";
    snprintf(out, out_len, "%s/%s/%s", SD_SAVES_ROOT, host_tid_str, uid_hex);
}

void vesper_save_refresh_owner(VesperSaveCtx* ctx, const char* host_tid_str) {
    if (!ctx || !host_tid_str) return;
    snprintf(ctx->active_owner, sizeof(ctx->active_owner), "_stock");
    if (ctx->user_count == 0) return;

    char root[320], marker[384];
    build_user_save_root(ctx, host_tid_str, root, sizeof(root));
    snprintf(marker, sizeof(marker), "%s/.active_save", root);

    FILE* f = fopen(marker, "r");
    if (!f) return;
    char line[128] = {0};
    if (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
        if (line[0]) snprintf(ctx->active_owner, sizeof(ctx->active_owner), "%s", line);
    }
    fclose(f);
}

bool vesper_save_probe_readonly(VesperSaveCtx* ctx, uint64_t host_tid, const char* host_tid_str,
                                size_t* out_files, uint64_t* out_bytes, char* err_buf, size_t err_len) {
    if (out_files) *out_files = 0;
    if (out_bytes) *out_bytes = 0;

    FILE* logf = fopen("sdmc:/vesper-save.log", "w");
    slog(logf, "=== Vesper Read-Only Save Probe ===");

    if (!ctx || ctx->user_count == 0) {
        snprintf(err_buf, err_len, "No Switch 2 user account available.");
        slog(logf, "[ERR] %s", err_buf);
        if (logf) fclose(logf);
        return false;
    }

    const VesperUser* u = &ctx->users[ctx->selected_user];
    slog(logf, "Host=%s  User='%s' (%s)", host_tid_str, u->nickname, u->uid_hex);

    Result rc = fsdevMountSaveData(SAVE_DEV_NAME, host_tid, u->uid);
    if (R_FAILED(rc)) {
        snprintf(err_buf, err_len, "Mount save:/ failed (0x%x). Launch host once on '%s' first.", rc, u->nickname);
        slog(logf, "[ERR] fsdevMountSaveData failed: 0x%x", rc);
        if (logf) fclose(logf);
        return false;
    }

    size_t files = 0;
    uint64_t bytes = 0;
    count_tree(SAVE_MOUNT, &files, &bytes);
    fsdevUnmountDevice(SAVE_DEV_NAME);

    if (out_files) *out_files = files;
    if (out_bytes) *out_bytes = bytes;
    slog(logf, "[OK] save:/ mounted cleanly: %zu file(s), %llu bytes (0 bytes written)",
         files, (unsigned long long)bytes);
    if (logf) fclose(logf);
    return true;
}

bool vesper_save_swap_profile(VesperSaveCtx* ctx, uint64_t host_tid, const char* host_tid_str,
                              const char* target_profile, char* err_buf, size_t err_len) {
    FILE* logf = fopen("sdmc:/vesper-save.log", "w");
    slog(logf, "=== Vesper Per-Profile Save Swap ===");

    if (!ctx || ctx->user_count == 0) {
        snprintf(err_buf, err_len, "No Switch 2 account selected.");
        slog(logf, "[ERR] %s", err_buf);
        if (logf) fclose(logf);
        return false;
    }

    vesper_save_refresh_owner(ctx, host_tid_str);
    const VesperUser* u = &ctx->users[ctx->selected_user];
    slog(logf, "Host=%s  User='%s' (%s)  '%s' -> '%s'",
         host_tid_str, u->nickname, u->uid_hex, ctx->active_owner, target_profile);

    if (strcmp(ctx->active_owner, target_profile) == 0) {
        slog(logf, "[OK] Target '%s' already owns save:/ — keeping save intact.", target_profile);
        if (logf) fclose(logf);
        return true;
    }

    Result rc = fsdevMountSaveData(SAVE_DEV_NAME, host_tid, u->uid);
    if (R_FAILED(rc)) {
        snprintf(err_buf, err_len, "Cannot mount save:/ (0x%x) for '%s'.", rc, u->nickname);
        slog(logf, "[ERR] fsdevMountSaveData failed: 0x%x", rc);
        if (logf) fclose(logf);
        return false;
    }

    char user_root[320];
    build_user_save_root(ctx, host_tid_str, user_root, sizeof(user_root));
    mkdir_p(user_root);

    /* 1. Permanent first-time _stock safety backup (never overwritten!) */
    if (strcmp(ctx->active_owner, "_stock") == 0) {
        char init_bak[448];
        snprintf(init_bak, sizeof(init_bak), "%s/_stock_initial_backup", user_root);
        if (!dir_exists(init_bak)) {
            size_t n = 0;
            slog(logf, "Creating one-time permanent _stock_initial_backup...");
            if (copy_tree(SAVE_MOUNT, init_bak, &n, logf) != 0) {
                clear_dir_contents(init_bak, true);
                fsdevUnmountDevice(SAVE_DEV_NAME);
                snprintf(err_buf, err_len, "Initial _stock backup failed — save:/ untouched!");
                if (logf) fclose(logf);
                return false;
            }
            slog(logf, "Saved %zu file(s) to _stock_initial_backup.", n);
        }
    }

    /* 2. Backup current save:/ into <active_owner>.tmp, then atomic rename -> <active_owner> */
    char cur_dir[448], tmp_dir[460], bak_dir[460];
    snprintf(cur_dir, sizeof(cur_dir), "%s/%s", user_root, ctx->active_owner);
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/%s.tmp", user_root, ctx->active_owner);
    snprintf(bak_dir, sizeof(bak_dir), "%s/%s.bak", user_root, ctx->active_owner);

    clear_dir_contents(tmp_dir, true);
    size_t backed_up = 0;
    if (copy_tree(SAVE_MOUNT, tmp_dir, &backed_up, logf) != 0) {
        clear_dir_contents(tmp_dir, true);
        fsdevUnmountDevice(SAVE_DEV_NAME);
        snprintf(err_buf, err_len, "Backup of '%s' failed — save:/ was NOT modified!", ctx->active_owner);
        slog(logf, "[ERR] %s", err_buf);
        if (logf) fclose(logf);
        return false;
    }

    clear_dir_contents(bak_dir, true);
    if (dir_exists(cur_dir)) rename(cur_dir, bak_dir);
    if (rename(tmp_dir, cur_dir) != 0) {
        if (dir_exists(bak_dir)) rename(bak_dir, cur_dir);
        fsdevUnmountDevice(SAVE_DEV_NAME);
        snprintf(err_buf, err_len, "Backup rename failed — save:/ was NOT modified!");
        slog(logf, "[ERR] %s", err_buf);
        if (logf) fclose(logf);
        return false;
    }
    clear_dir_contents(bak_dir, true);
    slog(logf, "[OK] Backed up %zu file(s) from save:/ -> %s", backed_up, cur_dir);

    /* 3. Clear save:/ now that backup is 100% verified on SD */
    if (clear_dir_contents(SAVE_MOUNT, false) != 0) {
        fsdevUnmountDevice(SAVE_DEV_NAME);
        snprintf(err_buf, err_len, "Failed clearing save:/ (backup is safe on SD!).");
        slog(logf, "[ERR] %s", err_buf);
        if (logf) fclose(logf);
        return false;
    }

    /* 4. Restore target profile's save from SD (if any exists yet) */
    char tgt_dir[448];
    snprintf(tgt_dir, sizeof(tgt_dir), "%s/%s", user_root, target_profile);
    size_t restored = 0;
    if (dir_exists(tgt_dir)) {
        if (copy_tree(tgt_dir, SAVE_MOUNT, &restored, logf) != 0) {
            fsdevUnmountDevice(SAVE_DEV_NAME);
            snprintf(err_buf, err_len, "Failed restoring save for '%s' (backups safe on SD).", target_profile);
            slog(logf, "[ERR] %s", err_buf);
            if (logf) fclose(logf);
            return false;
        }
        slog(logf, "[OK] Restored %zu file(s) from %s -> save:/", restored, tgt_dir);
    } else {
        slog(logf, "[OK] Fresh clean save:/ prepared for '%s' (no prior save on SD yet).", target_profile);
    }

    /* 5. Commit save:/ and unmount */
    rc = fsdevCommitDevice(SAVE_DEV_NAME);
    fsdevUnmountDevice(SAVE_DEV_NAME);
    if (R_FAILED(rc)) {
        snprintf(err_buf, err_len, "fsdevCommitDevice(save) failed: 0x%x", rc);
        slog(logf, "[ERR] %s", err_buf);
        if (logf) fclose(logf);
        return false;
    }

    /* 6. Update .active_save marker */
    char marker[384];
    snprintf(marker, sizeof(marker), "%s/.active_save", user_root);
    FILE* mf = fopen(marker, "w");
    if (mf) {
        fprintf(mf, "%s\n", target_profile);
        fclose(mf);
    }
    snprintf(ctx->active_owner, sizeof(ctx->active_owner), "%s", target_profile);
    slog(logf, "=== SAVE SWAP COMPLETE: '%s' now owns save:/ ===", target_profile);
    if (logf) fclose(logf);
    return true;
}
