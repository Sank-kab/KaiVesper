/* SPDX-License-Identifier: GPL-2.0-only */
#include "gui.h"
#include "font_aa.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

/* RGBA8888 (little-endian: 0xAABBGGRR) */
#define RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define RGB(r, g, b)     RGBA(r, g, b, 255)

/* Sleek Dark Slate & Switch 2 Accent Palette */
#define COL_BG           RGB(11, 15, 25)
#define COL_SIDEBAR      RGB(16, 22, 35)
#define COL_CARD         RGB(22, 30, 47)
#define COL_CARD_HOVER   RGB(28, 42, 68)
#define COL_CARD_DEEP    RGB(14, 19, 31)
#define COL_SHADOW       RGB(6, 9, 16)
#define COL_BORDER       RGB(42, 55, 80)
#define COL_BORDER_HI    RGB(66, 86, 122)
#define COL_CYAN         RGB(0, 216, 255)
#define COL_CYAN_BG      RGB(11, 52, 72)
#define COL_CORAL        RGB(255, 78, 114)
#define COL_GREEN        RGB(52, 211, 153)
#define COL_GREEN_BG     RGB(14, 58, 43)
#define COL_AMBER        RGB(251, 191, 36)
#define COL_AMBER_BG     RGB(66, 46, 14)
#define COL_RED          RGB(248, 113, 113)
#define COL_RED_BG       RGB(76, 22, 30)
#define COL_PURPLE       RGB(192, 132, 252)
#define COL_PURPLE_BG    RGB(52, 26, 82)
#define COL_TEXT         RGB(244, 247, 254)
#define COL_MUTED        RGB(150, 166, 192)
#define COL_DIM          RGB(102, 118, 144)

static const uint32_t AVATAR_COLORS[6] = {
    RGB(14, 165, 233),
    RGB(244, 63, 94),
    RGB(16, 185, 129),
    RGB(139, 92, 246),
    RGB(245, 158, 11),
    RGB(6, 182, 212),
};

static Framebuffer g_fb;
static bool g_fb_ready = false;

static inline void put_px(uint32_t* fb, uint32_t sp, int x, int y, uint32_t col) {
    if ((unsigned)x < GUI_WIDTH && (unsigned)y < GUI_HEIGHT) fb[y * sp + x] = col;
}

static inline void blend_px(uint32_t* fb, uint32_t sp, int x, int y, uint32_t col, uint8_t a) {
    if ((unsigned)x >= GUI_WIDTH || (unsigned)y >= GUI_HEIGHT || a == 0) return;
    if (a == 255) { fb[y * sp + x] = col; return; }
    uint32_t dst = fb[y * sp + x];
    uint32_t sr = col & 0xFFu, sg = (col >> 8) & 0xFFu, sb = (col >> 16) & 0xFFu;
    uint32_t dr = dst & 0xFFu, dg = (dst >> 8) & 0xFFu, db = (dst >> 16) & 0xFFu;
    uint32_t ia = 255u - a;
    uint32_t rr = (sr * a + dr * ia) / 255u;
    uint32_t rg = (sg * a + dg * ia) / 255u;
    uint32_t rb = (sb * a + db * ia) / 255u;
    fb[y * sp + x] = RGB(rr, rg, rb);
}

static void fill_rect(uint32_t* fb, uint32_t sp, int x, int y, int w, int h, uint32_t col) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GUI_WIDTH)  w = GUI_WIDTH - x;
    if (y + h > GUI_HEIGHT) h = GUI_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j++) {
        uint32_t* row = fb + j * sp + x;
        for (int i = 0; i < w; i++) row[i] = col;
    }
}

static void fill_rounded_rect(uint32_t* fb, uint32_t sp, int x, int y, int w, int h, int r, uint32_t col) {
    if (r <= 0) { fill_rect(fb, sp, x, y, w, h, col); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fill_rect(fb, sp, x + r, y, w - 2 * r, h, col);
    fill_rect(fb, sp, x, y + r, r, h - 2 * r, col);
    fill_rect(fb, sp, x + w - r, y + r, r, h - 2 * r, col);
    int r2 = r * r;
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int d2 = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
            if (d2 <= r2) {
                put_px(fb, sp, x + dx,         y + dy,         col);
                put_px(fb, sp, x + w - 1 - dx, y + dy,         col);
                put_px(fb, sp, x + dx,         y + h - 1 - dy, col);
                put_px(fb, sp, x + w - 1 - dx, y + h - 1 - dy, col);
            }
        }
    }
}

static void draw_circle(uint32_t* fb, uint32_t sp, int cx, int cy, int r, uint32_t col) {
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r2) put_px(fb, sp, cx + dx, cy + dy, col);
        }
    }
}

static void draw_card(uint32_t* fb, uint32_t sp, int x, int y, int w, int h, int r,
                      int border_px, uint32_t border_col, uint32_t fill_col, bool shadow) {
    if (shadow) fill_rounded_rect(fb, sp, x + 2, y + 4, w, h, r, COL_SHADOW);
    if (border_px > 0) {
        fill_rounded_rect(fb, sp, x, y, w, h, r, border_col);
        fill_rounded_rect(fb, sp, x + border_px, y + border_px,
                          w - 2 * border_px, h - 2 * border_px,
                          r > border_px ? r - border_px : 0, fill_col);
    } else {
        fill_rounded_rect(fb, sp, x, y, w, h, r, fill_col);
    }
}

/* --- Anti-Aliased Proportional Segoe UI Text Renderer --- */

static int text_width(const VesperFont* f, const char* s) {
    int w = 0;
    for (const char* p = s; p && *p; p++) {
        unsigned char uc = (unsigned char)*p;
        if (uc < 32 || uc > 126) uc = '?';
        w += f->glyphs[uc - 32].adv;
    }
    return w;
}

static void draw_text_aa(uint32_t* fb, uint32_t sp, const VesperFont* f,
                         int x, int y, const char* s, uint32_t col) {
    int cx = x;
    for (const char* p = s; p && *p; p++) {
        unsigned char uc = (unsigned char)*p;
        if (uc < 32 || uc > 126) uc = '?';
        const VesperGlyph* g = &f->glyphs[uc - 32];
        if (g->w && g->h) {
            const uint8_t* a = f->alpha + g->off;
            int gx = cx + g->xoff;
            int gy = y + g->yoff;
            for (int r = 0; r < g->h; r++) {
                for (int c = 0; c < g->w; c++) {
                    uint8_t av = a[r * g->w + c];
                    if (av) blend_px(fb, sp, gx + c, gy + r, col, av);
                }
            }
        }
        cx += g->adv;
    }
}

static void draw_text_trunc(uint32_t* fb, uint32_t sp, const VesperFont* f,
                            int x, int y, const char* s, uint32_t col, int max_px) {
    if (!s) return;
    if (text_width(f, s) <= max_px) {
        draw_text_aa(fb, sp, f, x, y, s, col);
        return;
    }
    int ell_w = text_width(f, "...");
    int limit = max_px - ell_w;
    if (limit <= 0) return;
    char buf[192];
    int w = 0, n = 0;
    for (const char* p = s; *p && n < (int)sizeof(buf) - 5; p++) {
        unsigned char uc = (unsigned char)*p;
        if (uc < 32 || uc > 126) uc = '?';
        int adv = f->glyphs[uc - 32].adv;
        if (w + adv > limit) break;
        buf[n++] = *p;
        w += adv;
    }
    memcpy(buf + n, "...", 4);
    draw_text_aa(fb, sp, f, x, y, buf, col);
}

static int draw_pill(uint32_t* fb, uint32_t sp, const VesperFont* f, int x, int y,
                     const char* label, uint32_t bg, uint32_t border, uint32_t fg) {
    int pad_x = 14;
    int h = f->line_h + 8;
    int tw = text_width(f, label);
    int w = tw + pad_x * 2;
    draw_card(fb, sp, x, y, w, h, h / 2, border ? 1 : 0, border, bg, false);
    draw_text_aa(fb, sp, f, x + pad_x, y + 4, label, fg);
    return w;
}

static int draw_btn_hint(uint32_t* fb, uint32_t sp, int x, int y, const char* key,
                         uint32_t key_bg, uint32_t key_fg, const char* label, uint32_t lbl_col) {
    int kw = text_width(&FONT_SM, key) + 18;
    if (kw < 32) kw = 32;
    int kh = 28;
    fill_rounded_rect(fb, sp, x, y, kw, kh, 14, key_bg);
    draw_text_aa(fb, sp, &FONT_SM, x + (kw - text_width(&FONT_SM, key)) / 2, y + 2, key, key_fg);
    draw_text_aa(fb, sp, &FONT_SM, x + kw + 10, y + 2, label, lbl_col);
    return kw + 10 + text_width(&FONT_SM, label) + 28;
}

static uint32_t hash_name_color(const char* name) {
    uint32_t h = 2166136261u;
    for (const char* p = name; p && *p; p++) {
        h ^= (uint8_t)tolower((unsigned char)*p);
        h *= 16777619u;
    }
    return AVATAR_COLORS[h % 6];
}

static void make_monogram(const char* name, char out[3]) {
    out[0] = '?'; out[1] = '\0'; out[2] = '\0';
    if (!name || !*name) return;
    int idx = 0;
    for (const char* p = name; *p && idx < 2; p++) {
        if (isalnum((unsigned char)*p)) {
            if (idx == 0) out[idx++] = (char)toupper((unsigned char)*p);
            else if (p > name && (*(p - 1) == '-' || *(p - 1) == '_' || *(p - 1) == ' '))
                out[idx++] = (char)toupper((unsigned char)*p);
        }
    }
    if (idx == 1) {
        for (const char* p = name + 1; *p && idx < 2; p++) {
            if (isalnum((unsigned char)*p)) out[idx++] = (char)toupper((unsigned char)*p);
        }
    }
    out[idx] = '\0';
}

bool vesper_gui_init(void) {
    NWindow* win = nwindowGetDefault();
    if (!win) return false;
    Result rc = framebufferCreate(&g_fb, win, GUI_WIDTH, GUI_HEIGHT, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) return false;
    rc = framebufferMakeLinear(&g_fb);
    if (R_FAILED(rc)) { framebufferClose(&g_fb); return false; }
    g_fb_ready = true;
    return true;
}

void vesper_gui_exit(void) {
    if (g_fb_ready) { framebufferClose(&g_fb); g_fb_ready = false; }
}

static bool profile_matches_live(const VesperResidentState* rs, const VesperProfile* p) {
    if (!rs || !rs->installed || rs->active_count == 0 || !rs->active_sd_path[0]) return false;
    if (p->has_romfs_bin && strcmp(rs->active_sd_path, p->romfs_bin_path) == 0) return true;
    if (p->has_exefs_nsp && strcmp(rs->active_sd_path, p->exefs_nsp_path) == 0) return true;
    for (size_t i = 0; i < p->exefs_file_count; i++) {
        if (strcmp(rs->active_sd_path, p->exefs_files[i].sd_path) == 0) return true;
    }
    if (p->has_romfs_dir) {
        size_t len = strlen(p->romfs_dir_rel);
        if (len > 0 && strncmp(rs->active_sd_path, p->romfs_dir_rel, len) == 0 &&
            (rs->active_sd_path[len] == '/' || rs->active_sd_path[len] == '\0')) return true;
    }
    for (size_t i = 0; i < p->patch_count; i++) {
        if (strcmp(rs->active_sd_path, p->patches[i].sd_path) == 0) return true;
    }
    return false;
}

/* Sidebar Navigation geometry */
#define SB_W        264
#define NAV_X       20
#define NAV_Y       126
#define NAV_W       224
#define NAV_H       54
#define NAV_GAP     12

/* Spacious Single-Column Games List geometry */
#define LIST_X      304
#define LIST_Y      98
#define ROW_W       940
#define ROW_H       84
#define ROW_GAP     12
#define LIST_PAGE   5

/* Hosts Tab list geometry */
#define HOSTS_Y     176
#define HOSTS_ROW_H 88
#define HOSTS_GAP   12
#define HOSTS_PAGE  4

int vesper_gui_hit_test_tab(int tx, int ty) {
    if (tx < NAV_X || tx >= NAV_X + NAV_W) return -1;
    for (int i = 0; i < TAB_COUNT; i++) {
        int y = NAV_Y + i * (NAV_H + NAV_GAP);
        if (ty >= y && ty < y + NAV_H) return i;
    }
    return -1;
}

int vesper_gui_hit_test_card(const VesperCatalog* cat, size_t host_idx, size_t prof_idx, int tx, int ty) {
    if (!cat || cat->host_count == 0 || host_idx >= cat->host_count) return -1;
    if (tx < LIST_X || tx >= LIST_X + ROW_W) return -1;
    const VesperHost* h = &cat->hosts[host_idx];
    size_t page_start = (prof_idx / LIST_PAGE) * LIST_PAGE;
    for (size_t slot = 0; slot < LIST_PAGE; slot++) {
        size_t idx = page_start + slot;
        if (idx >= h->profile_count) break;
        int y = LIST_Y + (int)slot * (ROW_H + ROW_GAP);
        if (ty >= y && ty < y + ROW_H) return (int)idx;
    }
    return -1;
}

int vesper_gui_hit_test_host_row(size_t filtered_count, size_t ct_idx, int tx, int ty) {
    if (filtered_count == 0) return -1;
    if (tx < LIST_X || tx >= LIST_X + ROW_W) return -1;
    size_t page_start = (ct_idx / HOSTS_PAGE) * HOSTS_PAGE;
    for (size_t slot = 0; slot < HOSTS_PAGE; slot++) {
        size_t idx = page_start + slot;
        if (idx >= filtered_count) break;
        int y = HOSTS_Y + (int)slot * (HOSTS_ROW_H + HOSTS_GAP);
        if (ty >= y && ty < y + HOSTS_ROW_H) return (int)idx;
    }
    return -1;
}

static void render_games_view(uint32_t* fb, uint32_t sp,
                              const VesperResidentState* rs,
                              const VesperCatalog* cat,
                              const VesperConsoleCatalog* cc,
                              size_t host_idx, size_t prof_idx) {
    draw_text_aa(fb, sp, &FONT_LG, LIST_X, 28, "Games", COL_TEXT);

    if (cat && cat->host_count > 0 && host_idx < cat->host_count) {
        const VesperHost* h = &cat->hosts[host_idx];
        const char* gen_lbl = h->is_switch1 ? "SW1" : "SW2";
        draw_pill(fb, sp, &FONT_SM, LIST_X + 124, 34, gen_lbl,
                  h->is_switch1 ? COL_PURPLE_BG : COL_CYAN_BG,
                  h->is_switch1 ? COL_PURPLE : COL_CYAN,
                  h->is_switch1 ? COL_PURPLE : COL_CYAN);

        const char* hname = vesper_console_lookup_name(cc, h->title_id_str);
        char meta[128];
        size_t pages = (h->profile_count + LIST_PAGE - 1) / LIST_PAGE;
        if (hname) {
            snprintf(meta, sizeof(meta), "Host: %.28s  |  %zu game(s)  |  Pg %u/%u",
                     hname, h->profile_count,
                     (unsigned)(prof_idx / LIST_PAGE + 1), (unsigned)(pages ? pages : 1));
        } else {
            snprintf(meta, sizeof(meta), "Host %s  |  %zu game(s)  |  Pg %u/%u",
                     h->title_id_str, h->profile_count,
                     (unsigned)(prof_idx / LIST_PAGE + 1), (unsigned)(pages ? pages : 1));
        }
        int mw = text_width(&FONT_SM, meta);
        int mx = LIST_X + ROW_W - mw;
        if (mx < LIST_X + 200) mx = LIST_X + 200;
        draw_text_trunc(fb, sp, &FONT_SM, mx, 40, meta, COL_MUTED, LIST_X + ROW_W - mx);
    }

    if (!cat || cat->host_count == 0 || host_idx >= cat->host_count || cat->hosts[host_idx].profile_count == 0) {
        draw_card(fb, sp, LIST_X, LIST_Y, ROW_W, 210, 16, 1, COL_BORDER, COL_CARD, true);
        draw_text_aa(fb, sp, &FONT_MD, LIST_X + 36, LIST_Y + 34, "No games ready yet", COL_AMBER);
        draw_text_aa(fb, sp, &FONT_SM, LIST_X + 36, LIST_Y + 78,
                     "1. Press [R] to open Hosts and pick your SW1 / SW2 Host title.", COL_TEXT);
        draw_text_aa(fb, sp, &FONT_SM, LIST_X + 36, LIST_Y + 110,
                     "2. Put games in /ns2jb-modloader/sw1/ or /sw2/ (or use KaiVesper Prep on Windows).", COL_MUTED);
        draw_text_aa(fb, sp, &FONT_SM, LIST_X + 36, LIST_Y + 144,
                     "Press [Y] after copying files to refresh the list.", COL_DIM);
        return;
    }

    const VesperHost* h = &cat->hosts[host_idx];
    size_t page_start = (prof_idx / LIST_PAGE) * LIST_PAGE;

    for (size_t slot = 0; slot < LIST_PAGE; slot++) {
        size_t idx = page_start + slot;
        if (idx >= h->profile_count) break;
        const VesperProfile* p = &h->profiles[idx];
        bool sel = (idx == prof_idx);
        bool active = profile_matches_live(rs, p);

        int y = LIST_Y + (int)slot * (ROW_H + ROW_GAP);
        uint32_t border = sel ? COL_CYAN : (active ? COL_GREEN : COL_BORDER);
        uint32_t bg     = sel ? COL_CARD_HOVER : COL_CARD;
        draw_card(fb, sp, LIST_X, y, ROW_W, ROW_H, 14, sel ? 3 : 1, border, bg, true);

        /* Left color monogram tile */
        fill_rounded_rect(fb, sp, LIST_X + 18, y + 16, 52, 52, 12, hash_name_color(p->name));
        char mono[3];
        make_monogram(p->name, mono);
        draw_text_aa(fb, sp, &FONT_MD, LIST_X + 18 + (52 - text_width(&FONT_MD, mono)) / 2, y + 28, mono, COL_TEXT);

        /* Game Name (large 22px bold) + Simple 1-line specs (18px) */
        int tx = LIST_X + 86;
        draw_text_trunc(fb, sp, &FONT_MD, tx, y + 15, p->name, COL_TEXT, 580);

        char ex_sz[20] = "", rm_sz[20] = "", sub[160] = "";
        char rom_part[64] = "", exe_part[64] = "", pat_part[32] = "";
        if (p->has_romfs_bin) {
            vesper_fmt_size(p->romfs_bin_size, rm_sz, sizeof(rm_sz));
            snprintf(rom_part, sizeof(rom_part), "%s RomFS", rm_sz);
        } else if (p->has_romfs_dir) {
            snprintf(rom_part, sizeof(rom_part),
                     (p->has_exefs_nsp || p->has_exefs_dir) ? "romfs/ (auto-pack)" : "romfs/ mod");
        }
        if (p->has_exefs_nsp) {
            vesper_fmt_size(p->exefs_nsp_size, ex_sz, sizeof(ex_sz));
            snprintf(exe_part, sizeof(exe_part), "%s ExeFS", ex_sz);
        } else if (p->has_exefs_dir) {
            vesper_fmt_size(p->exefs_dir_size, ex_sz, sizeof(ex_sz));
            snprintf(exe_part, sizeof(exe_part), "%s exefs/ (%zu NSO)", ex_sz, p->exefs_file_count);
        }
        if (p->patch_count > 0) {
            snprintf(pat_part, sizeof(pat_part), "%zu IPS", p->patch_count);
        }
        size_t slen = 0;
        if (rom_part[0]) slen += (size_t)snprintf(sub + slen, sizeof(sub) - slen, "%s", rom_part);
        if (exe_part[0]) slen += (size_t)snprintf(sub + slen, sizeof(sub) - slen, "%s%s", slen ? "  |  " : "", exe_part);
        if (pat_part[0]) snprintf(sub + slen, sizeof(sub) - slen, "%s%s", slen ? "  |  " : "", pat_part);
        draw_text_trunc(fb, sp, &FONT_SM, tx, y + 48, sub, COL_MUTED, 580);

        /* Right clear action / status pill */
        if (active) {
            int pw = text_width(&FONT_SM, "ACTIVE") + 28;
            draw_pill(fb, sp, &FONT_SM, LIST_X + ROW_W - pw - 22, y + 26, "ACTIVE", COL_GREEN_BG, COL_GREEN, COL_GREEN);
        } else if (sel) {
            const char* lbl = (rs && rs->installed) ? "A  Hot-Swap" : "A  Load Game";
            int pw = text_width(&FONT_SM, lbl) + 28;
            draw_pill(fb, sp, &FONT_SM, LIST_X + ROW_W - pw - 22, y + 26, lbl, COL_CYAN, 0, COL_BG);
        }
    }
}

static void render_hosts_view(uint32_t* fb, uint32_t sp,
                              const VesperConsoleCatalog* cc,
                              HostFilterMode filter_mode,
                              size_t ct_idx) {
    draw_text_aa(fb, sp, &FONT_LG, LIST_X, 28, "Console Hosts", COL_TEXT);

    const char* flbl = (filter_mode == HOST_FILTER_SW1) ? "Filter: SW1 Only"
                     : (filter_mode == HOST_FILTER_SW2) ? "Filter: SW2 Only"
                     : "Filter: All (SW1 + SW2)";
    uint32_t fbg = (filter_mode == HOST_FILTER_SW1) ? COL_PURPLE_BG
                 : (filter_mode == HOST_FILTER_SW2) ? COL_CYAN_BG : COL_CARD;
    uint32_t ffg = (filter_mode == HOST_FILTER_SW1) ? COL_PURPLE
                 : (filter_mode == HOST_FILTER_SW2) ? COL_CYAN : COL_TEXT;
    draw_pill(fb, sp, &FONT_SM, LIST_X + 244, 34, flbl, fbg, ffg, ffg);

    /* Active Host Summary Bar */
    draw_card(fb, sp, LIST_X, 92, ROW_W, 66, 12, 1, COL_BORDER, COL_CARD_DEEP, false);
    draw_circle(fb, sp, LIST_X + 24, 125, 6, COL_PURPLE);
    char sw1_line[128], sw2_line[128];
    if (cc && cc->selected_sw1_str[0]) {
        snprintf(sw1_line, sizeof(sw1_line), "SW1 Host: %s",
                 cc->selected_sw1_name[0] ? cc->selected_sw1_name : cc->selected_sw1_str);
    } else {
        snprintf(sw1_line, sizeof(sw1_line), "SW1 Host: Not selected");
    }
    draw_text_trunc(fb, sp, &FONT_SM, LIST_X + 40, 113, sw1_line,
                    (cc && cc->selected_sw1_str[0]) ? COL_TEXT : COL_MUTED, 420);

    draw_circle(fb, sp, LIST_X + 490, 125, 6, COL_CYAN);
    if (cc && cc->selected_sw2_str[0]) {
        snprintf(sw2_line, sizeof(sw2_line), "SW2 Host: %s",
                 cc->selected_sw2_name[0] ? cc->selected_sw2_name : cc->selected_sw2_str);
    } else {
        snprintf(sw2_line, sizeof(sw2_line), "SW2 Host: Not selected");
    }
    draw_text_trunc(fb, sp, &FONT_SM, LIST_X + 506, 113, sw2_line,
                    (cc && cc->selected_sw2_str[0]) ? COL_TEXT : COL_MUTED, 420);

    size_t indices[VESPER_MAX_CONSOLE_TITLES];
    size_t fcount = vesper_console_filter(cc, filter_mode, indices, VESPER_MAX_CONSOLE_TITLES);

    char meta[64];
    size_t pages = (fcount + HOSTS_PAGE - 1) / HOSTS_PAGE;
    snprintf(meta, sizeof(meta), "%zu title(s)  |  Pg %u/%u",
             fcount, (unsigned)(fcount ? (ct_idx / HOSTS_PAGE + 1) : 1), (unsigned)(pages ? pages : 1));
    int mw = text_width(&FONT_SM, meta);
    draw_text_aa(fb, sp, &FONT_SM, LIST_X + ROW_W - mw, 40, meta, COL_MUTED);

    if (!cc || !cc->ns_ok) {
        draw_card(fb, sp, LIST_X, HOSTS_Y, ROW_W, 180, 16, 1, COL_AMBER, COL_CARD, true);
        draw_text_aa(fb, sp, &FONT_MD, LIST_X + 32, HOSTS_Y + 34, "Console title scanner unavailable", COL_AMBER);
        char emsg[128];
        snprintf(emsg, sizeof(emsg), "nsInitialize returned 0x%x. You can still place games in /ns2jb-modloader/contents/<TID>/games/",
                 cc ? cc->ns_rc : 0u);
        draw_text_trunc(fb, sp, &FONT_SM, LIST_X + 32, HOSTS_Y + 82, emsg, COL_MUTED, ROW_W - 64);
        return;
    }
    if (fcount == 0) {
        draw_card(fb, sp, LIST_X, HOSTS_Y, ROW_W, 160, 16, 1, COL_BORDER, COL_CARD, true);
        draw_text_aa(fb, sp, &FONT_MD, LIST_X + 32, HOSTS_Y + 36, "No installed titles match this filter", COL_AMBER);
        draw_text_aa(fb, sp, &FONT_SM, LIST_X + 32, HOSTS_Y + 82,
                     "Press [X] to switch filter (All / SW1 / SW2) or [Y] to rescan.", COL_MUTED);
        return;
    }

    if (ct_idx >= fcount) ct_idx = 0;
    size_t page_start = (ct_idx / HOSTS_PAGE) * HOSTS_PAGE;

    for (size_t slot = 0; slot < HOSTS_PAGE; slot++) {
        size_t pos = page_start + slot;
        if (pos >= fcount) break;
        const VesperConsoleTitle* t = &cc->titles[indices[pos]];
        bool sel = (pos == ct_idx);
        bool is_active_host = t->is_switch1
            ? (strcmp(cc->selected_sw1_str, t->title_id_str) == 0)
            : (strcmp(cc->selected_sw2_str, t->title_id_str) == 0);

        int y = HOSTS_Y + (int)slot * (HOSTS_ROW_H + HOSTS_GAP);
        uint32_t border = sel ? COL_CYAN : (is_active_host ? COL_GREEN : COL_BORDER);
        uint32_t bg     = sel ? COL_CARD_HOVER : COL_CARD;
        draw_card(fb, sp, LIST_X, y, ROW_W, HOSTS_ROW_H, 14, sel ? 3 : 1, border, bg, true);

        /* Left SW1 / SW2 generation tile */
        uint32_t tbg = t->is_switch1 ? COL_PURPLE_BG : COL_CYAN_BG;
        uint32_t tfg = t->is_switch1 ? COL_PURPLE : COL_CYAN;
        draw_card(fb, sp, LIST_X + 18, y + 18, 62, 52, 12, 1, tfg, tbg, false);
        const char* tag = t->is_switch1 ? "SW1" : "SW2";
        draw_text_aa(fb, sp, &FONT_SM, LIST_X + 18 + (62 - text_width(&FONT_SM, tag)) / 2, y + 32, tag, tfg);

        int tx = LIST_X + 96;
        draw_text_trunc(fb, sp, &FONT_MD, tx, y + 16, t->name, COL_TEXT, 580);

        const char* logo_tag = (t->logo_handling == 0) ? "  |  Auto Logo"
                             : (t->logo_handling == 1) ? "  |  Manual Logo (Prep auto-fix)"
                             : "";
        char sub[160];
        snprintf(sub, sizeof(sub), "ID: %s%s%s",
                 t->title_id_str, logo_tag, t->has_sd_folder ? "  |  SD ready" : "");
        draw_text_trunc(fb, sp, &FONT_SM, tx, y + 50, sub,
                        (t->logo_handling == 0) ? COL_GREEN : (t->logo_handling == 1 ? COL_AMBER : COL_MUTED), 580);

        if (is_active_host) {
            int pw = text_width(&FONT_SM, "ACTIVE HOST") + 28;
            draw_pill(fb, sp, &FONT_SM, LIST_X + ROW_W - pw - 22, y + 28,
                      "ACTIVE HOST", COL_GREEN_BG, COL_GREEN, COL_GREEN);
        } else if (sel) {
            int pw = text_width(&FONT_SM, "A  Set as Host") + 28;
            draw_pill(fb, sp, &FONT_SM, LIST_X + ROW_W - pw - 22, y + 28,
                      "A  Set as Host", COL_CYAN, 0, COL_BG);
        }
    }
}

static void render_saves_view(uint32_t* fb, uint32_t sp,
                              const VesperKaihenCtx* kh,
                              const VesperResidentState* rs,
                              const VesperSaveCtx* sv) {
    draw_text_aa(fb, sp, &FONT_LG, LIST_X, 28, "Saves & System", COL_TEXT);

    /* Card 1: Same-Account Save Isolation */
    draw_card(fb, sp, LIST_X, 98, ROW_W, 240, 16, 1,
              (sv && sv->auto_swap_enabled) ? COL_PURPLE : COL_BORDER, COL_CARD, true);
    draw_text_aa(fb, sp, &FONT_MD, LIST_X + 28, 120, "Same-Account Save Isolation", COL_TEXT);
    draw_text_aa(fb, sp, &FONT_SM, LIST_X + 28, 152,
                 "Automatically backs up and swaps save data whenever you switch games on the same account.", COL_MUTED);

    if (sv && sv->user_count > 0) {
        const VesperUser* u = &sv->users[sv->selected_user];
        char acc[96];
        snprintf(acc, sizeof(acc), "Account:  %s   (%zu of %zu)   -   Press [ZL/ZR] to change",
                 u->nickname, sv->selected_user + 1, sv->user_count);
        draw_text_aa(fb, sp, &FONT_SM, LIST_X + 28, 198, acc, COL_TEXT);

        char slot[96];
        snprintf(slot, sizeof(slot), "Active save slot:  %.64s", sv->active_owner);
        draw_text_aa(fb, sp, &FONT_SM, LIST_X + 28, 228, slot, COL_CYAN);

        const char* toggle_lbl = sv->auto_swap_enabled
            ? "Auto Save-Swap: ON   (Press A to turn OFF)"
            : "Auto Save-Swap: OFF  (Press A to turn ON)";
        draw_pill(fb, sp, &FONT_SM, LIST_X + 28, 272, toggle_lbl,
                  sv->auto_swap_enabled ? COL_GREEN_BG : COL_AMBER_BG,
                  sv->auto_swap_enabled ? COL_GREEN : COL_AMBER,
                  sv->auto_swap_enabled ? COL_GREEN : COL_AMBER);
    } else {
        draw_text_aa(fb, sp, &FONT_SM, LIST_X + 28, 206,
                     "Account list unavailable. Launch your Host title with an empty secondary profile.", COL_AMBER);
    }

    /* Card 2: System Health & Diagnostics */
    draw_card(fb, sp, LIST_X, 356, ROW_W, 212, 16, 1, COL_BORDER, COL_CARD, true);
    draw_text_aa(fb, sp, &FONT_MD, LIST_X + 28, 378, "System Status", COL_TEXT);

    const char* kh_str = (kh && kh->open && kh->has_fs_helpers) ? "Patched kaiHEN Active (Ready)"
                       : (kh && kh->open) ? "Stock kaiHEN (Patched kaiHEN needed)"
                       : "kaiHEN Not Detected";
    uint32_t kh_col = (kh && kh->open && kh->has_fs_helpers) ? COL_GREEN : COL_AMBER;
    draw_text_aa(fb, sp, &FONT_SM, LIST_X + 28, 420, "kaiHEN:", COL_MUTED);
    draw_text_aa(fb, sp, &FONT_SM, LIST_X + 160, 420, kh_str, kh_col);

    const char* fs_str = (rs && rs->installed && rs->active_count > 0) ? "Hot-Swap Active (Zero Reboot)"
                       : (rs && rs->installed) ? "Paused (Stock Unmodded Mode)"
                       : "Not Loaded Yet (Pick a game in Games tab)";
    uint32_t fs_col = (rs && rs->installed && rs->active_count > 0) ? COL_CYAN : COL_TEXT;
    draw_text_aa(fb, sp, &FONT_SM, LIST_X + 28, 452, "FS Engine:", COL_MUTED);
    draw_text_aa(fb, sp, &FONT_SM, LIST_X + 160, 452, fs_str, fs_col);

    draw_pill(fb, sp, &FONT_SM, LIST_X + 28, 504, "Press [B]  Test FS & Save Access (Safe Read-Only)",
              COL_CARD_DEEP, COL_BORDER_HI, COL_TEXT);
}

static void render_guide_view(uint32_t* fb, uint32_t sp) {
    draw_text_aa(fb, sp, &FONT_LG, LIST_X, 28, "Quick Guide", COL_TEXT);

    static const char* STEPS_T[4] = {
        "1.  Choose a Game & Press [A]",
        "2.  Exit & Open Your Host Game",
        "3.  Switch 1 Games (Unencrypted exefs/ + romfs/)",
        "4.  Zero-Reboot Hot-Swap & Save Isolation"
    };
    static const char* STEPS_D[4] = {
        "Highlight any game in Games and press [A]. Loose romfs/ folders auto-pack to romfs.bin once!",
        "Press [+] to leave KaiVesper, go to the Home Menu, and start your Host title normally.",
        "Put Switch 1 exefs/ + romfs/ (or romfs.bin) under a 0100... SW1 Host! Use [ZR] to switch Host.",
        "Close the Host game (X -> Close), reopen KaiVesper & press [A] on the next game. Saves auto-swap!"
    };
    static const uint32_t COLS[4] = {COL_CYAN, COL_GREEN, COL_CORAL, COL_PURPLE};

    for (int i = 0; i < 4; i++) {
        int y = 98 + i * 118;
        draw_card(fb, sp, LIST_X, y, ROW_W, 102, 14, 1, COL_BORDER, COL_CARD, true);
        fill_rounded_rect(fb, sp, LIST_X + 16, y + 18, 6, 66, 3, COLS[i]);
        draw_text_aa(fb, sp, &FONT_MD, LIST_X + 38, y + 18, STEPS_T[i], COLS[i]);
        draw_text_aa(fb, sp, &FONT_SM, LIST_X + 38, y + 54, STEPS_D[i], COL_TEXT);
    }
}

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
                       bool busy) {
    if (!g_fb_ready) return;

    u32 stride_bytes = 0;
    uint32_t* fb = (uint32_t*)framebufferBegin(&g_fb, &stride_bytes);
    if (!fb) return;
    uint32_t sp = stride_bytes / 4;

    /* Clean solid dark canvas + left sidebar */
    fill_rect(fb, sp, 0, 0, GUI_WIDTH, GUI_HEIGHT, COL_BG);
    fill_rect(fb, sp, 0, 0, SB_W, GUI_HEIGHT, COL_SIDEBAR);
    fill_rect(fb, sp, SB_W, 0, 1, GUI_HEIGHT, COL_BORDER);
    fill_rect(fb, sp, 0, 0, SB_W / 2, 4, COL_CYAN);
    fill_rect(fb, sp, SB_W / 2, 0, SB_W / 2, 4, COL_CORAL);

    /* Sidebar Brand */
    fill_rounded_rect(fb, sp, 22, 28, 16, 38, 7, COL_CYAN);
    fill_rounded_rect(fb, sp, 42, 28, 16, 38, 7, COL_CORAL);
    draw_circle(fb, sp, 30, 39, 3, COL_SIDEBAR);
    draw_circle(fb, sp, 50, 53, 3, COL_SIDEBAR);
    draw_text_aa(fb, sp, &FONT_LG, 70, 22, "KaiVesper", COL_TEXT);
    draw_text_aa(fb, sp, &FONT_SM, 72, 58, "by sank-kab", COL_DIM);

    /* Sidebar 4 Big Navigation Buttons */
    static const char* NAV_LABELS[TAB_COUNT] = {"Games", "Hosts", "Saves & System", "Guide"};
    for (int i = 0; i < TAB_COUNT; i++) {
        int y = NAV_Y + i * (NAV_H + NAV_GAP);
        bool cur = ((int)active_tab == i);
        draw_card(fb, sp, NAV_X, y, NAV_W, NAV_H, 12, cur ? 2 : 0,
                  cur ? COL_CYAN : 0, cur ? COL_CYAN_BG : COL_SIDEBAR, false);
        if (cur) fill_rounded_rect(fb, sp, NAV_X + 8, y + 12, 5, NAV_H - 24, 2, COL_CYAN);
        draw_text_aa(fb, sp, &FONT_MD, NAV_X + 26, y + 13, NAV_LABELS[i], cur ? COL_TEXT : COL_MUTED);
    }

    /* Sidebar Bottom Live Status Card */
    draw_card(fb, sp, 20, 520, 224, 122, 14, 1, COL_BORDER, COL_CARD_DEEP, false);
    bool hot = rs && rs->installed && rs->active_count > 0;
    bool ok  = kh && kh->open && kh->has_fs_helpers;
    draw_circle(fb, sp, 38, 546, 6, hot ? COL_GREEN : (ok ? COL_CYAN : COL_AMBER));
    draw_text_aa(fb, sp, &FONT_SM, 54, 535, hot ? "Hot-Swap Active" : (ok ? "FS Ready" : "Stock kaiHEN"),
                 hot ? COL_GREEN : (ok ? COL_CYAN : COL_AMBER));

    if (sv && sv->user_count > 0) {
        char uline[48];
        snprintf(uline, sizeof(uline), "User: %.14s", sv->users[sv->selected_user].nickname);
        draw_text_trunc(fb, sp, &FONT_SM, 32, 570, uline, COL_TEXT, 200);
        char sline[48];
        snprintf(sline, sizeof(sline), "Auto-Save: %s", sv->auto_swap_enabled ? "ON" : "OFF");
        draw_text_aa(fb, sp, &FONT_SM, 32, 600, sline, sv->auto_swap_enabled ? COL_PURPLE : COL_DIM);
    } else {
        draw_text_aa(fb, sp, &FONT_SM, 32, 576, "Empty profile", COL_MUTED);
        draw_text_aa(fb, sp, &FONT_SM, 32, 602, "recommended", COL_MUTED);
    }

    /* Main Right View */
    if (active_tab == TAB_LIBRARY)      render_games_view(fb, sp, rs, cat, cc, host_idx, prof_idx);
    else if (active_tab == TAB_HOSTS)   render_hosts_view(fb, sp, cc, host_filter, ct_idx);
    else if (active_tab == TAB_SYSTEM)  render_saves_view(fb, sp, kh, rs, sv);
    else                                render_guide_view(fb, sp);

    /* Status Toast Bar (right area, y=592..644) */
    uint32_t t_bg = COL_CARD, t_br = COL_BORDER_HI, t_fg = COL_TEXT;
    if (busy)                          { t_bg = COL_CYAN_BG;  t_br = COL_CYAN;  t_fg = COL_CYAN; }
    else if (toast_kind == TOAST_OK)   { t_bg = COL_GREEN_BG; t_br = COL_GREEN; t_fg = COL_GREEN; }
    else if (toast_kind == TOAST_WARN) { t_bg = COL_AMBER_BG; t_br = COL_AMBER; t_fg = COL_AMBER; }
    else if (toast_kind == TOAST_ERR)  { t_bg = COL_RED_BG;   t_br = COL_RED;   t_fg = COL_RED; }

    draw_card(fb, sp, LIST_X, 590, ROW_W, 52, 12, 2, t_br, t_bg, true);
    draw_circle(fb, sp, LIST_X + 24, 616, 6, t_fg);
    draw_text_trunc(fb, sp, &FONT_SM, LIST_X + 42, 604,
                    toast_msg && toast_msg[0] ? toast_msg : "Ready.", t_fg, ROW_W - 64);

    /* Clean Bottom Action Bar */
    fill_rect(fb, sp, SB_W + 1, 660, GUI_WIDTH - SB_W - 1, 60, COL_SIDEBAR);
    fill_rect(fb, sp, 0, 659, GUI_WIDTH, 1, COL_BORDER);

    int bx = 24;
    const int by = 676;
    bx += draw_btn_hint(fb, sp, bx, by, "L/R", COL_CARD, COL_CYAN, "Menu", COL_TEXT);
    if (active_tab == TAB_LIBRARY) {
        bx += draw_btn_hint(fb, sp, bx, by, "A", COL_CYAN, COL_BG,
                            (rs && rs->installed) ? "Hot-Swap Game" : "Load Game", COL_TEXT);
        bx += draw_btn_hint(fb, sp, bx, by, "X", COL_CORAL, COL_BG, "Stock Mode", COL_TEXT);
        if (cat && cat->host_count > 1) {
            bx += draw_btn_hint(fb, sp, bx, by, "ZR", COL_PURPLE_BG, COL_PURPLE, "Host", COL_TEXT);
        }
        bx += draw_btn_hint(fb, sp, bx, by, "Y", COL_CARD, COL_TEXT, "Refresh", COL_MUTED);
    } else if (active_tab == TAB_HOSTS) {
        bx += draw_btn_hint(fb, sp, bx, by, "A", COL_CYAN, COL_BG, "Set as Host", COL_TEXT);
        bx += draw_btn_hint(fb, sp, bx, by, "X", COL_PURPLE_BG, COL_PURPLE, "Filter SW1/SW2", COL_TEXT);
        bx += draw_btn_hint(fb, sp, bx, by, "Y", COL_CARD, COL_TEXT, "Rescan Console", COL_MUTED);
    } else if (active_tab == TAB_SYSTEM) {
        bx += draw_btn_hint(fb, sp, bx, by, "A", COL_CYAN, COL_BG, "Toggle Auto-Save", COL_TEXT);
        bx += draw_btn_hint(fb, sp, bx, by, "ZL/ZR", COL_CARD, COL_TEXT, "Switch Account", COL_TEXT);
        bx += draw_btn_hint(fb, sp, bx, by, "B", COL_CARD, COL_TEXT, "Run Safe Check", COL_TEXT);
    }
    draw_btn_hint(fb, sp, GUI_WIDTH - 134, by, "+", COL_CARD, COL_TEXT, "Exit", COL_TEXT);

    framebufferEnd(&g_fb);
}
