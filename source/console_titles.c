/* SPDX-License-Identifier: GPL-2.0-only */
#include "console_titles.h"
#include "raw_inflate.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HOSTS_CFG_DIR   "sdmc:/vesper"
#define HOSTS_CFG_PATH  "sdmc:/vesper/hosts.cfg"
#define SD_MOD_ROOT     "sdmc:/ns2jb-modloader"

static bool dir_exists_simple(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Creates every component after "sdmc:/" and verifies the target directory exists. */
static bool mkdir_p_verified(const char* path, char* err_buf, size_t err_len) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    char* start = strstr(tmp, ":/");
    char* p = start ? (start + 2) : (tmp + 1);
    while (*p == '/') p++;

    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!dir_exists_simple(tmp)) {
                if (mkdir(tmp, 0777) != 0 && errno != EEXIST && !dir_exists_simple(tmp)) {
                    if (err_buf && err_len) snprintf(err_buf, err_len, "mkdir(%s) failed: %s", tmp, strerror(errno));
                    return false;
                }
            }
            *p = '/';
        }
    }
    if (!dir_exists_simple(tmp)) {
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST && !dir_exists_simple(tmp)) {
            if (err_buf && err_len) snprintf(err_buf, err_len, "mkdir(%s) failed: %s", tmp, strerror(errno));
            return false;
        }
    }
    if (!dir_exists_simple(tmp)) {
        if (err_buf && err_len) snprintf(err_buf, err_len, "Directory not created: %s", tmp);
        return false;
    }
    return true;
}

/* Converts NACP UTF-8 title into clean printable ASCII for our AA bitmap renderer
 * (strips trademark/copyright symbols cleanly and folds common Latin accents). */
static void sanitize_title_name(const char* src, char* dst, size_t dst_len) {
    size_t out = 0;
    const unsigned char* p = (const unsigned char*)src;
    while (*p && out + 1 < dst_len) {
        if (*p >= 32 && *p <= 126) {
            dst[out++] = (char)*p++;
        } else if (*p == '\n' || *p == '\r' || *p == '\t') {
            if (out > 0 && dst[out - 1] != ' ') dst[out++] = ' ';
            p++;
        } else if (p[0] == 0xC2 && (p[1] == 0xAE || p[1] == 0xA9)) {
            p += 2;
        } else if (p[0] == 0xE2 && p[1] == 0x84 && p[2] == 0xA2) {
            p += 3;
        } else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x99 || p[2] == 0x98)) {
            dst[out++] = '\'';
            p += 3;
        } else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x93 || p[2] == 0x94)) {
            dst[out++] = '-';
            p += 3;
        } else if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
            static const char LATIN1[65] =
                "AAAAAAECEEEEIIIIDNOOOOOxOUUUUYPs"
                "aaaaaaeceeeeiiiionooooo/ouuuuypy";
            dst[out++] = LATIN1[p[1] - 0x80];
            p += 2;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            p += 4;
        } else {
            p++;
        }
    }
    while (out > 0 && dst[out - 1] == ' ') out--;
    dst[out] = '\0';
}

/* Picks the best readable title from an array of NacpLanguageEntry (16 for format 0, 32 for format 1). */
static bool pick_best_lang_name(const NacpLanguageEntry* entries, size_t count, int pref_idx,
                                char* out_name, size_t out_len) {
    /* Preferred readable Latin-script language indices first:
     * 0=AmericanEnglish, 1=BritishEnglish, 5=Italian, 2=French, 3=German, 4=Spanish,
     * 7=Dutch, 9=Portuguese, 13=LatinAmericanSpanish, 12=CanadianFrench, 17=BrazilianPortuguese */
    static const int PREF_ORDER[] = {0, 1, 5, 2, 3, 4, 7, 9, 13, 12, 17};

    if (pref_idx >= 0 && (size_t)pref_idx < count && entries[pref_idx].name[0]) {
        sanitize_title_name(entries[pref_idx].name, out_name, out_len);
        if (out_name[0]) return true;
    }
    for (size_t i = 0; i < sizeof(PREF_ORDER) / sizeof(PREF_ORDER[0]); i++) {
        size_t idx = (size_t)PREF_ORDER[i];
        if (idx < count && entries[idx].name[0]) {
            sanitize_title_name(entries[idx].name, out_name, out_len);
            if (out_name[0]) return true;
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (entries[i].name[0]) {
            sanitize_title_name(entries[i].name, out_name, out_len);
            if (out_name[0]) return true;
        }
    }
    return false;
}

static bool extract_nacp_title(NacpStruct* nacp, bool set_ok, NacpLanguageEntry* inflate_buf32,
                               char* out_name, size_t out_len) {
    out_name[0] = '\0';

    /* Switch 2 FW 21.0.0+: TitlesDataFormat == 1 stores raw DEFLATE-compressed NacpLanguageEntry[32] */
    if (nacp->titles_data_format == 1) {
        size_t csz = nacp->lang_data.compressed_data.buffer_size;
        if (csz > sizeof(nacp->lang_data.compressed_data.buffer)) {
            csz = sizeof(nacp->lang_data.compressed_data.buffer);
        }
        if (csz > 0 && inflate_buf32) {
            memset(inflate_buf32, 0, 32 * sizeof(NacpLanguageEntry));
            size_t out_bytes = 0;
            if (vesper_raw_inflate(nacp->lang_data.compressed_data.buffer, csz,
                                   (uint8_t*)inflate_buf32, 32 * sizeof(NacpLanguageEntry),
                                   &out_bytes)) {
                size_t nentries = out_bytes / sizeof(NacpLanguageEntry);
                if (nentries == 0 && out_bytes >= 0x200) nentries = 1;
                if (nentries > 32) nentries = 32;
                int pref = -1;
                if (set_ok) {
                    u64 lang_code = 0;
                    SetLanguage sys_lang = SetLanguage_ENUS;
                    if (R_SUCCEEDED(setGetSystemLanguage(&lang_code)) &&
                        R_SUCCEEDED(setMakeLanguage(lang_code, &sys_lang))) {
                        pref = (int)sys_lang;
                    }
                }
                if (pick_best_lang_name(inflate_buf32, nentries, pref, out_name, out_len)) {
                    return true;
                }
            }
        }
        return false;
    }

    /* Standard TitlesDataFormat == 0: uncompressed NacpLanguageEntry[16] */
    if (set_ok) {
        NacpLanguageEntry* lang = NULL;
        if (R_SUCCEEDED(nacpGetLanguageEntry(nacp, &lang)) && lang && lang->name[0]) {
            sanitize_title_name(lang->name, out_name, out_len);
            if (out_name[0]) return true;
        }
    }
    return pick_best_lang_name(nacp->lang_data.lang, 16, -1, out_name, out_len);
}

#define CTRL_BUF_SIZE (512u * 1024u) /* 512 KiB: fits Switch 2 >128KB 256x256 icons + 16KB NACP */

static bool fetch_control_data(u64 tid, NsApplicationControlData* ctrl, size_t ctrl_sz, FILE* logf) {
    u64 actual = 0;
    u32 unk = 0;
    Result r1 = nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, ctrl, ctrl_sz, &actual);
    if (R_SUCCEEDED(r1) && actual >= sizeof(NacpStruct)) return true;

    Result r2 = nsGetApplicationControlData2(NsApplicationControlSource_Storage, tid, ctrl, ctrl_sz, 0, 0, &actual, &unk);
    if (R_SUCCEEDED(r2) && actual >= sizeof(NacpStruct)) return true;

    Result r2b = nsGetApplicationControlData2(NsApplicationControlSource_Storage, tid, ctrl, ctrl_sz, 1, 0, &actual, &unk);
    if (R_SUCCEEDED(r2b) && actual >= sizeof(NacpStruct)) return true;

    Result r3 = nsGetApplicationControlData2(NsApplicationControlSource_Storage, tid, ctrl, ctrl_sz, 0, 1, &actual, &unk);
    if (R_SUCCEEDED(r3) && actual >= sizeof(NacpStruct)) return true;

    Result r4 = nsGetApplicationControlData2(NsApplicationControlSource_Storage, tid, ctrl, ctrl_sz, 0xFF, 0, &actual, &unk);
    if (R_SUCCEEDED(r4) && actual >= sizeof(NacpStruct)) return true;

    Result r5 = nsGetApplicationControlData(NsApplicationControlSource_CacheOnly, tid, ctrl, ctrl_sz, &actual);
    if (R_SUCCEEDED(r5) && actual >= sizeof(NacpStruct)) return true;

    Result r6 = nsGetApplicationControlData2(NsApplicationControlSource_CacheOnly, tid, ctrl, ctrl_sz, 1, 0, &actual, &unk);
    if (R_SUCCEEDED(r6) && actual >= sizeof(NacpStruct)) return true;

    if (logf) {
        fprintf(logf, "  [MISS] %016llx: Data(S)=0x%x D2(S,0,0)=0x%x D2(S,1,0)=0x%x D2(S,0,1)=0x%x D2(S,FF,0)=0x%x Data(C)=0x%x D2(C,1,0)=0x%x actual=0x%llx\n",
                (unsigned long long)tid, r1, r2, r2b, r3, r4, r5, r6, (unsigned long long)actual);
    }
    return false;
}

/* Official Switch 2 (20.0.0+/21.0.0+) batch title lookup via nsListApplicationTitle2
 * (auto-decompresses TitlesDataFormat==1 and resolves Switch 2 Editions & cached titles). */
static void batch_resolve_titles_v2(VesperConsoleCatalog* out, bool* resolved,
                                    NsApplicationControlSource source, FILE* logf) {
    u64 tids[VESPER_MAX_CONSOLE_TITLES];
    size_t idx_map[VESPER_MAX_CONSOLE_TITLES];
    size_t n = 0;
    for (size_t i = 0; i < out->count; i++) {
        if (!resolved[i]) {
            tids[n] = out->titles[i].title_id;
            idx_map[n] = i;
            n++;
        }
    }
    if (n == 0) return;

    size_t raw_sz = n * sizeof(NacpLanguageEntry) + n * sizeof(u64) + CTRL_BUF_SIZE + 0x10000;
    size_t buf_sz = (raw_sz + 0xFFFULL) & ~0xFFFULL;
    if (buf_sz < 0x100000) buf_sz = 0x100000; /* 1 MiB TransferMemory */
    void* buf = aligned_alloc(0x1000, buf_sz);
    if (!buf) return;
    memset(buf, 0, buf_sz);

    AsyncValue av;
    memset(&av, 0, sizeof(av));
    Result rc = nsListApplicationTitle2(&av, source, tids, (s32)n, buf, buf_sz);
    if (R_SUCCEEDED(rc)) {
        Result wrc = asyncValueWait(&av, 5000000000ULL);
        s32 off = -1;
        u64 val_sz = 0;
        Result grc = R_SUCCEEDED(wrc) ? asyncValueGet(&av, &off, sizeof(off)) : wrc;
        if (R_SUCCEEDED(grc)) asyncValueGetSize(&av, &val_sz);
        asyncValueClose(&av);

        size_t hits = 0;
        if (R_SUCCEEDED(grc) && off >= 0 && (size_t)off + n * sizeof(NacpLanguageEntry) <= buf_sz) {
            const NacpLanguageEntry* entries = (const NacpLanguageEntry*)((const uint8_t*)buf + off);
            for (size_t k = 0; k < n; k++) {
                if (entries[k].name[0]) {
                    VesperConsoleTitle* t = &out->titles[idx_map[k]];
                    sanitize_title_name(entries[k].name, t->name, sizeof(t->name));
                    if (t->name[0]) {
                        resolved[idx_map[k]] = true;
                        hits++;
                    }
                }
            }
        }
        if (logf) {
            fprintf(logf, "nsListApplicationTitle2(src=%d, n=%zu): wait=0x%x get=0x%x off=%d val_sz=%llu hits=%zu\n",
                    (int)source, n, wrc, grc, off, (unsigned long long)val_sz, hits);
        }
    } else if (logf) {
        fprintf(logf, "nsListApplicationTitle2(src=%d, n=%zu) failed: 0x%x\n", (int)source, n, rc);
    }

    free(buf);
}

static int cmp_titles_ci(const void* a, const void* b) {
    const VesperConsoleTitle* ta = (const VesperConsoleTitle*)a;
    const VesperConsoleTitle* tb = (const VesperConsoleTitle*)b;
    const unsigned char* pa = (const unsigned char*)ta->name;
    const unsigned char* pb = (const unsigned char*)tb->name;
    while (*pa && *pb) {
        int ca = tolower(*pa++);
        int cb = tolower(*pb++);
        if (ca != cb) return ca - cb;
    }
    return tolower(*pa) - tolower(*pb);
}

static void load_host_prefs(VesperConsoleCatalog* cat) {
    FILE* f = fopen(HOSTS_CFG_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (strncmp(line, "sw1=", 4) == 0 && strlen(line + 4) >= 16) {
            memcpy(cat->selected_sw1_str, line + 4, 16);
            cat->selected_sw1_str[16] = '\0';
            cat->selected_sw1_tid = strtoull(cat->selected_sw1_str, NULL, 16);
            const char* sep = strchr(line + 4, '|');
            if (sep && sep[1]) snprintf(cat->selected_sw1_name, sizeof(cat->selected_sw1_name), "%s", sep + 1);
        } else if (strncmp(line, "sw2=", 4) == 0 && strlen(line + 4) >= 16) {
            memcpy(cat->selected_sw2_str, line + 4, 16);
            cat->selected_sw2_str[16] = '\0';
            cat->selected_sw2_tid = strtoull(cat->selected_sw2_str, NULL, 16);
            const char* sep = strchr(line + 4, '|');
            if (sep && sep[1]) snprintf(cat->selected_sw2_name, sizeof(cat->selected_sw2_name), "%s", sep + 1);
        }
    }
    fclose(f);
}

static bool save_host_prefs(const VesperConsoleCatalog* cat, char* err_buf, size_t err_len) {
    if (!mkdir_p_verified(HOSTS_CFG_DIR, err_buf, err_len)) return false;
    FILE* f = fopen(HOSTS_CFG_PATH, "w");
    if (!f) {
        if (err_buf && err_len) snprintf(err_buf, err_len, "Cannot write %s: %s", HOSTS_CFG_PATH, strerror(errno));
        return false;
    }
    if (cat->selected_sw1_str[0]) {
        fprintf(f, "sw1=%s|%s\n", cat->selected_sw1_str, cat->selected_sw1_name);
    }
    if (cat->selected_sw2_str[0]) {
        fprintf(f, "sw2=%s|%s\n", cat->selected_sw2_str, cat->selected_sw2_name);
    }
    fclose(f);
    return true;
}

void vesper_console_scan(VesperConsoleCatalog* out) {
    memset(out, 0, sizeof(*out));
    load_host_prefs(out);

    Result rc = nsInitialize();
    out->ns_rc = rc;
    if (R_FAILED(rc)) {
        out->ns_ok = false;
        return;
    }
    out->ns_ok = true;

    FILE* logf = fopen("sdmc:/vesper-hosts.log", "w");
    if (logf) fprintf(logf, "=== KaiVesper Console Title Scan ===\n");

    bool set_ok = R_SUCCEEDED(setInitialize());
    NsApplicationRecord records[32];
    s32 offset = 0;

    /* Pass 1: enumerate all SW1 (01...) & SW2 (04...) application records */
    while (out->count < VESPER_MAX_CONSOLE_TITLES) {
        s32 got = 0;
        rc = nsListApplicationRecord(records, 32, offset, &got);
        if (R_FAILED(rc) || got <= 0) break;

        for (s32 i = 0; i < got && out->count < VESPER_MAX_CONSOLE_TITLES; i++) {
            uint64_t tid = records[i].application_id;
            uint64_t plat = tid >> 56;
            if (plat != 0x01ULL && plat != 0x04ULL) continue;

            VesperConsoleTitle* t = &out->titles[out->count];
            memset(t, 0, sizeof(*t));
            t->title_id = tid;
            t->is_switch1 = (plat == 0x01ULL);
            t->logo_handling = -1;
            snprintf(t->title_id_str, sizeof(t->title_id_str), "%016llx", (unsigned long long)tid);

            char sd_dir[256];
            snprintf(sd_dir, sizeof(sd_dir), "%s/contents/%s/games", SD_MOD_ROOT, t->title_id_str);
            t->has_sd_folder = dir_exists_simple(sd_dir);
            out->count++;
        }

        offset += got;
        if (got < 32) break;
    }

    if (logf) fprintf(logf, "Enumerated %zu SW1/SW2 application record(s)\n", out->count);

    bool resolved[VESPER_MAX_CONSOLE_TITLES];
    memset(resolved, 0, sizeof(resolved));

    /* Pass 2: Official Switch 2 batch API (handles FW 20/21 compressed NACP, Switch 2 Editions & cache) */
    batch_resolve_titles_v2(out, resolved, NsApplicationControlSource_Storage, logf);
    batch_resolve_titles_v2(out, resolved, NsApplicationControlSource_CacheOnly, logf);

    /* Pass 3: Per-title ControlData fallback (512 KiB buffer + raw DEFLATE decoder for TitlesDataFormat==1) */
    NsApplicationControlData* ctrl = (NsApplicationControlData*)calloc(1, CTRL_BUF_SIZE);
    NacpLanguageEntry* inflate_buf = (NacpLanguageEntry*)calloc(32, sizeof(NacpLanguageEntry));

    for (size_t i = 0; i < out->count; i++) {
        VesperConsoleTitle* t = &out->titles[i];
        if (ctrl && fetch_control_data(t->title_id, ctrl, CTRL_BUF_SIZE, resolved[i] ? NULL : logf)) {
            t->logo_handling = (int8_t)ctrl->nacp.logo_handling;
            if (!resolved[i]) {
                if (extract_nacp_title(&ctrl->nacp, set_ok, inflate_buf, t->name, sizeof(t->name))) {
                    resolved[i] = true;
                } else if (logf) {
                    fprintf(logf, "  [NACP_EMPTY] %s fmt=%u csz=%u\n",
                            t->title_id_str, ctrl->nacp.titles_data_format,
                            (unsigned)ctrl->nacp.lang_data.compressed_data.buffer_size);
                }
            }
        }
        if (!resolved[i]) {
            char tid_tmp[17];
            memcpy(tid_tmp, t->title_id_str, sizeof(tid_tmp));
            snprintf(t->name, sizeof(t->name), "%s Title %s",
                     t->is_switch1 ? "Switch 1" : "Switch 2", tid_tmp);
        }

        if (t->is_switch1 && strcmp(out->selected_sw1_str, t->title_id_str) == 0) {
            snprintf(out->selected_sw1_name, sizeof(out->selected_sw1_name), "%s", t->name);
        } else if (!t->is_switch1 && strcmp(out->selected_sw2_str, t->title_id_str) == 0) {
            snprintf(out->selected_sw2_name, sizeof(out->selected_sw2_name), "%s", t->name);
        }
    }

    if (logf) {
        size_t ok_cnt = 0;
        for (size_t i = 0; i < out->count; i++) if (resolved[i]) ok_cnt++;
        fprintf(logf, "Resolved %zu / %zu title names.\n", ok_cnt, out->count);
        fclose(logf);
    }

    free(inflate_buf);
    free(ctrl);
    if (set_ok) setExit();
    nsExit();

    if (out->count > 1) {
        qsort(out->titles, out->count, sizeof(VesperConsoleTitle), cmp_titles_ci);
    }
}

bool vesper_console_select_host(VesperConsoleCatalog* cat, const VesperConsoleTitle* title,
                                char* err_buf, size_t err_len) {
    if (!cat || !title) {
        snprintf(err_buf, err_len, "Invalid host selection.");
        return false;
    }

    char games_dir[256];
    snprintf(games_dir, sizeof(games_dir), "%s/contents/%s/games", SD_MOD_ROOT, title->title_id_str);
    if (!mkdir_p_verified(games_dir, err_buf, err_len)) {
        return false;
    }

    /* Write a small marker file inside games/ so the directory is flushed & visible in all FTP/file managers */
    char info_path[300];
    snprintf(info_path, sizeof(info_path), "%s/HOST_INFO.txt", games_dir);
    FILE* hf = fopen(info_path, "w");
    if (hf) {
        fprintf(hf, "KaiVesper %s Host\n", title->is_switch1 ? "Switch 1" : "Switch 2");
        fprintf(hf, "Name: %s\n", title->name);
        fprintf(hf, "Title ID: %s\n\n", title->title_id_str);
        fprintf(hf, "Put each game in its own subfolder here:\n");
        fprintf(hf, "  %s/<game_name>/exefs/\n", games_dir);
        fprintf(hf, "  %s/<game_name>/romfs/  (or romfs.bin)\n", games_dir);
        fflush(hf);
        fclose(hf);
    }

    /* Also create universal sw1/ or sw2/ drop-in folder */
    char univ_dir[256];
    snprintf(univ_dir, sizeof(univ_dir), "%s/%s", SD_MOD_ROOT, title->is_switch1 ? "sw1" : "sw2");
    mkdir_p_verified(univ_dir, NULL, 0);

    if (title->is_switch1) {
        cat->selected_sw1_tid = title->title_id;
        snprintf(cat->selected_sw1_str, sizeof(cat->selected_sw1_str), "%s", title->title_id_str);
        snprintf(cat->selected_sw1_name, sizeof(cat->selected_sw1_name), "%s", title->name);
    } else {
        cat->selected_sw2_tid = title->title_id;
        snprintf(cat->selected_sw2_str, sizeof(cat->selected_sw2_str), "%s", title->title_id_str);
        snprintf(cat->selected_sw2_name, sizeof(cat->selected_sw2_name), "%s", title->name);
    }

    if (!save_host_prefs(cat, err_buf, err_len)) {
        return false;
    }

    for (size_t i = 0; i < cat->count; i++) {
        if (cat->titles[i].title_id == title->title_id) {
            cat->titles[i].has_sd_folder = true;
            break;
        }
    }
    return true;
}

const char* vesper_console_lookup_name(const VesperConsoleCatalog* cat, const char* title_id_str) {
    if (!cat || !title_id_str || !title_id_str[0]) return NULL;
    for (size_t i = 0; i < cat->count; i++) {
        if (strcmp(cat->titles[i].title_id_str, title_id_str) == 0) {
            return cat->titles[i].name;
        }
    }
    if (strcmp(cat->selected_sw1_str, title_id_str) == 0 && cat->selected_sw1_name[0]) return cat->selected_sw1_name;
    if (strcmp(cat->selected_sw2_str, title_id_str) == 0 && cat->selected_sw2_name[0]) return cat->selected_sw2_name;
    return NULL;
}

size_t vesper_console_filter(const VesperConsoleCatalog* cat, HostFilterMode mode,
                             size_t* out_indices, size_t max_out) {
    if (!cat || !out_indices || max_out == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < cat->count && n < max_out; i++) {
        if (mode == HOST_FILTER_SW1 && !cat->titles[i].is_switch1) continue;
        if (mode == HOST_FILTER_SW2 && cat->titles[i].is_switch1) continue;
        out_indices[n++] = i;
    }
    return n;
}
