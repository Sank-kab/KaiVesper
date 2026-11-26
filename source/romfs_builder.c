/* SPDX-License-Identifier: GPL-2.0-only */
#include "romfs_builder.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROMFS_EMPTY      0xFFFFFFFFu
#define ROMFS_DATA_START 0x200ULL

static inline uint64_t align_u64(uint64_t n, uint64_t a) {
    return (n + a - 1ULL) & ~(a - 1ULL);
}

static inline uint32_t align_u32(uint32_t n, uint32_t a) {
    return (n + a - 1u) & ~(a - 1u);
}

static inline void put_u32_le(uint8_t* p, uint32_t v) {
    memcpy(p, &v, 4);
}

static inline void put_u64_le(uint8_t* p, uint64_t v) {
    memcpy(p, &v, 8);
}

static uint32_t romfs_bucket_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1u;
    while ((n % 2u == 0) || (n % 3u == 0) || (n % 5u == 0) ||
           (n % 7u == 0) || (n % 11u == 0) || (n % 13u == 0) || (n % 17u == 0)) {
        n++;
    }
    return n;
}

static uint32_t romfs_name_hash(uint32_t parent_off, const char* name, uint32_t len) {
    uint32_t h = parent_off ^ 123456789u;
    for (uint32_t i = 0; i < len; i++) {
        h = ((h >> 5) | (h << 27)) ^ (uint8_t)name[i];
    }
    return h;
}

static bool count_walk(const char* dir, size_t max_files, size_t* files, uint64_t* bytes) {
    DIR* d = opendir(dir);
    if (!d) return false;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[1024];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!count_walk(full, max_files, files, bytes)) {
                closedir(d);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            (*files)++;
            (*bytes) += (uint64_t)st.st_size;
            if (max_files > 0 && *files > max_files) {
                closedir(d);
                return true;
            }
        }
    }
    closedir(d);
    return true;
}

bool vesper_romfs_count_dir(const char* dir_path, size_t max_files,
                            size_t* out_files, uint64_t* out_bytes) {
    size_t f = 0;
    uint64_t b = 0;
    bool ok = count_walk(dir_path, max_files, &f, &b);
    if (out_files) *out_files = f;
    if (out_bytes) *out_bytes = b;
    return ok;
}

/* --- Dynamic RomFS Builder --- */

typedef struct {
    char* rel_path;         /* "" for root, "dir/sub" otherwise */
    const char* base_name;  /* points inside rel_path */
    uint32_t name_len;
    int32_t parent_idx;     /* 0 for root's children, -1 for root */
    uint32_t meta_off;
    uint32_t first_child_dir;   /* index or ROMFS_EMPTY */
    uint32_t next_sibling_dir;  /* index or ROMFS_EMPTY */
    uint32_t first_file;        /* index or ROMFS_EMPTY */
} RomfsDirEntry;

typedef struct {
    char* rel_path;
    char* full_path;
    const char* base_name;
    uint32_t name_len;
    int32_t parent_idx;
    uint64_t size;
    uint64_t data_off;      /* relative to ROMFS_DATA_START */
    uint32_t meta_off;
    uint32_t next_sibling_file; /* index or ROMFS_EMPTY */
} RomfsFileEntry;

typedef struct {
    RomfsDirEntry* dirs;
    size_t dir_count;
    size_t dir_cap;
    RomfsFileEntry* files;
    size_t file_count;
    size_t file_cap;
    uint64_t total_file_bytes;
} RomfsTree;

static void tree_free(RomfsTree* t) {
    for (size_t i = 0; i < t->dir_count; i++) free(t->dirs[i].rel_path);
    for (size_t i = 0; i < t->file_count; i++) {
        free(t->files[i].rel_path);
        free(t->files[i].full_path);
    }
    free(t->dirs);
    free(t->files);
    memset(t, 0, sizeof(*t));
}

static char* dup_str(const char* s) {
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    if (out) memcpy(out, s, n + 1);
    return out;
}

static int32_t tree_add_dir(RomfsTree* t, const char* rel_path, int32_t parent_idx) {
    if (t->dir_count == t->dir_cap) {
        size_t ncap = t->dir_cap ? t->dir_cap * 2 : 256;
        RomfsDirEntry* nd = (RomfsDirEntry*)realloc(t->dirs, ncap * sizeof(*nd));
        if (!nd) return -1;
        t->dirs = nd;
        t->dir_cap = ncap;
    }
    char* copy = dup_str(rel_path);
    if (!copy) return -1;
    const char* slash = strrchr(copy, '/');
    const char* base = slash ? (slash + 1) : copy;

    size_t idx = t->dir_count++;
    RomfsDirEntry* e = &t->dirs[idx];
    e->rel_path = copy;
    e->base_name = base;
    e->name_len = (uint32_t)strlen(base);
    e->parent_idx = parent_idx;
    e->meta_off = 0;
    e->first_child_dir = ROMFS_EMPTY;
    e->next_sibling_dir = ROMFS_EMPTY;
    e->first_file = ROMFS_EMPTY;
    return (int32_t)idx;
}

static bool tree_add_file(RomfsTree* t, const char* rel_path, const char* full_path,
                          int32_t parent_idx, uint64_t size) {
    if (t->file_count == t->file_cap) {
        size_t ncap = t->file_cap ? t->file_cap * 2 : 512;
        RomfsFileEntry* nf = (RomfsFileEntry*)realloc(t->files, ncap * sizeof(*nf));
        if (!nf) return false;
        t->files = nf;
        t->file_cap = ncap;
    }
    char* rcopy = dup_str(rel_path);
    char* fcopy = dup_str(full_path);
    if (!rcopy || !fcopy) {
        free(rcopy);
        free(fcopy);
        return false;
    }
    const char* slash = strrchr(rcopy, '/');
    const char* base = slash ? (slash + 1) : rcopy;

    RomfsFileEntry* e = &t->files[t->file_count++];
    e->rel_path = rcopy;
    e->full_path = fcopy;
    e->base_name = base;
    e->name_len = (uint32_t)strlen(base);
    e->parent_idx = parent_idx;
    e->size = size;
    e->data_off = 0;
    e->meta_off = 0;
    e->next_sibling_file = ROMFS_EMPTY;
    t->total_file_bytes += size;
    return true;
}

static int cmp_str_ptr(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static bool scan_recursive(RomfsTree* t, const char* cur_full, const char* cur_rel,
                           int32_t parent_idx, char* err, size_t err_len) {
    DIR* d = opendir(cur_full);
    if (!d) {
        snprintf(err, err_len, "Cannot open romfs directory: %.120s", cur_full);
        return false;
    }

    char** names = NULL;
    size_t count = 0, cap = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            char** nn = (char**)realloc(names, ncap * sizeof(*nn));
            if (!nn) {
                closedir(d);
                for (size_t i = 0; i < count; i++) free(names[i]);
                free(names);
                snprintf(err, err_len, "Out of memory scanning romfs/");
                return false;
            }
            names = nn;
            cap = ncap;
        }
        names[count] = dup_str(ent->d_name);
        if (!names[count]) {
            closedir(d);
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            snprintf(err, err_len, "Out of memory scanning romfs/");
            return false;
        }
        count++;
    }
    closedir(d);

    if (count > 1) qsort(names, count, sizeof(char*), cmp_str_ptr);

    bool ok = true;
    for (size_t i = 0; i < count && ok; i++) {
        char child_full[1024], child_rel[1024];
        int fn = snprintf(child_full, sizeof(child_full), "%s/%s", cur_full, names[i]);
        int rn = cur_rel[0]
            ? snprintf(child_rel, sizeof(child_rel), "%s/%s", cur_rel, names[i])
            : snprintf(child_rel, sizeof(child_rel), "%s", names[i]);
        if (fn <= 0 || (size_t)fn >= sizeof(child_full) || rn <= 0 || (size_t)rn >= sizeof(child_rel)) {
            snprintf(err, err_len, "RomFS path too long: %.120s", names[i]);
            ok = false;
            break;
        }
        struct stat st;
        if (stat(child_full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            int32_t idx = tree_add_dir(t, child_rel, parent_idx);
            if (idx < 0) {
                snprintf(err, err_len, "Out of memory adding RomFS dir.");
                ok = false;
                break;
            }
            if (!scan_recursive(t, child_full, child_rel, idx, err, err_len)) {
                ok = false;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!tree_add_file(t, child_rel, child_full, parent_idx, (uint64_t)st.st_size)) {
                snprintf(err, err_len, "Out of memory adding RomFS file.");
                ok = false;
                break;
            }
        }
    }

    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return ok;
}

static bool write_zeros(FILE* f, uint64_t count) {
    static const uint8_t ZEROS[4096] = {0};
    while (count > 0) {
        size_t chunk = (count > sizeof(ZEROS)) ? sizeof(ZEROS) : (size_t)count;
        if (fwrite(ZEROS, 1, chunk, f) != chunk) return false;
        count -= chunk;
    }
    return true;
}

bool vesper_romfs_build_image(const char* romfs_dir,
                              const char* out_image_path,
                              VesperRomfsProgressFn progress_cb,
                              void* user_data,
                              char* err_buf,
                              size_t err_len) {
    RomfsTree tree = {0};
    if (tree_add_dir(&tree, "", -1) < 0) {
        snprintf(err_buf, err_len, "Out of memory initializing RomFS builder.");
        return false;
    }

    if (!scan_recursive(&tree, romfs_dir, "", 0, err_buf, err_len)) {
        tree_free(&tree);
        return false;
    }
    if (tree.file_count == 0 && tree.dir_count <= 1) {
        snprintf(err_buf, err_len, "romfs/ folder is empty.");
        tree_free(&tree);
        return false;
    }

    /* Compute directory & file metadata offsets + sibling/child links */
    uint64_t dir_meta_size = 0;
    for (size_t i = 0; i < tree.dir_count; i++) {
        if (dir_meta_size >= 0xFFFFFFFFULL) {
            snprintf(err_buf, err_len, "RomFS directory table exceeds 4 GiB.");
            tree_free(&tree);
            return false;
        }
        tree.dirs[i].meta_off = (uint32_t)dir_meta_size;
        dir_meta_size += 24ULL + align_u32(tree.dirs[i].name_len, 4);
    }

    uint64_t file_meta_size = 0;
    uint64_t data_cursor = 0;
    for (size_t i = 0; i < tree.file_count; i++) {
        if (file_meta_size >= 0xFFFFFFFFULL) {
            snprintf(err_buf, err_len, "RomFS file table exceeds 4 GiB.");
            tree_free(&tree);
            return false;
        }
        tree.files[i].meta_off = (uint32_t)file_meta_size;
        file_meta_size += 32ULL + align_u32(tree.files[i].name_len, 4);
        data_cursor = align_u64(data_cursor, 16);
        tree.files[i].data_off = data_cursor;
        data_cursor += tree.files[i].size;
    }

    /* Link children/siblings in reverse order so linked lists stay lexicographically sorted */
    for (size_t i = tree.dir_count; i-- > 1; ) {
        int32_t p = tree.dirs[i].parent_idx;
        tree.dirs[i].next_sibling_dir = tree.dirs[p].first_child_dir;
        tree.dirs[p].first_child_dir = (uint32_t)i;
    }
    for (size_t i = tree.file_count; i-- > 0; ) {
        int32_t p = tree.files[i].parent_idx;
        tree.files[i].next_sibling_file = tree.dirs[p].first_file;
        tree.dirs[p].first_file = (uint32_t)i;
    }

    /* Build hash & metadata tables */
    uint32_t dir_buckets = romfs_bucket_count((uint32_t)tree.dir_count);
    uint32_t file_buckets = romfs_bucket_count((uint32_t)tree.file_count);
    size_t dir_hash_bytes = (size_t)dir_buckets * 4;
    size_t file_hash_bytes = (size_t)file_buckets * 4;

    uint32_t* dir_hash = (uint32_t*)malloc(dir_hash_bytes);
    uint32_t* file_hash = (uint32_t*)malloc(file_hash_bytes);
    uint8_t* dir_table = (uint8_t*)calloc(1, (size_t)dir_meta_size);
    uint8_t* file_table = (uint8_t*)calloc(1, (size_t)(file_meta_size ? file_meta_size : 1));
    uint8_t* copy_buf = (uint8_t*)malloc(1024 * 1024); /* 1 MiB streaming buffer */

    if (!dir_hash || !file_hash || !dir_table || !file_table || !copy_buf) {
        free(dir_hash); free(file_hash); free(dir_table); free(file_table); free(copy_buf);
        tree_free(&tree);
        snprintf(err_buf, err_len, "Out of memory building RomFS tables.");
        return false;
    }

    for (uint32_t i = 0; i < dir_buckets; i++) dir_hash[i] = ROMFS_EMPTY;
    for (uint32_t i = 0; i < file_buckets; i++) file_hash[i] = ROMFS_EMPTY;

    for (size_t i = 0; i < tree.dir_count; i++) {
        const RomfsDirEntry* d = &tree.dirs[i];
        uint32_t po = (d->parent_idx >= 0) ? tree.dirs[d->parent_idx].meta_off : 0u;
        uint32_t slot = romfs_name_hash(po, d->base_name, d->name_len) % dir_buckets;
        uint32_t sib = (d->next_sibling_dir != ROMFS_EMPTY) ? tree.dirs[d->next_sibling_dir].meta_off : ROMFS_EMPTY;
        uint32_t chd = (d->first_child_dir != ROMFS_EMPTY) ? tree.dirs[d->first_child_dir].meta_off : ROMFS_EMPTY;
        uint32_t fil = (d->first_file != ROMFS_EMPTY) ? tree.files[d->first_file].meta_off : ROMFS_EMPTY;
        uint8_t* rec = dir_table + d->meta_off;
        put_u32_le(rec + 0,  po);
        put_u32_le(rec + 4,  sib);
        put_u32_le(rec + 8,  chd);
        put_u32_le(rec + 12, fil);
        put_u32_le(rec + 16, dir_hash[slot]);
        put_u32_le(rec + 20, d->name_len);
        if (d->name_len) memcpy(rec + 24, d->base_name, d->name_len);
        dir_hash[slot] = d->meta_off;
    }

    for (size_t i = 0; i < tree.file_count; i++) {
        const RomfsFileEntry* f = &tree.files[i];
        uint32_t po = tree.dirs[f->parent_idx].meta_off;
        uint32_t slot = romfs_name_hash(po, f->base_name, f->name_len) % file_buckets;
        uint32_t sib = (f->next_sibling_file != ROMFS_EMPTY) ? tree.files[f->next_sibling_file].meta_off : ROMFS_EMPTY;
        uint8_t* rec = file_table + f->meta_off;
        put_u32_le(rec + 0,  po);
        put_u32_le(rec + 4,  sib);
        put_u64_le(rec + 8,  f->data_off);
        put_u64_le(rec + 16, f->size);
        put_u32_le(rec + 24, file_hash[slot]);
        put_u32_le(rec + 28, f->name_len);
        if (f->name_len) memcpy(rec + 32, f->base_name, f->name_len);
        file_hash[slot] = f->meta_off;
    }

    uint64_t meta_base = align_u64(ROMFS_DATA_START + data_cursor, 4);
    uint8_t header[80];
    put_u64_le(header + 0,  0x50ULL);
    put_u64_le(header + 8,  meta_base);
    put_u64_le(header + 16, (uint64_t)dir_hash_bytes);
    put_u64_le(header + 24, meta_base + dir_hash_bytes);
    put_u64_le(header + 32, dir_meta_size);
    put_u64_le(header + 40, meta_base + dir_hash_bytes + dir_meta_size);
    put_u64_le(header + 48, (uint64_t)file_hash_bytes);
    put_u64_le(header + 56, meta_base + dir_hash_bytes + dir_meta_size + file_hash_bytes);
    put_u64_le(header + 64, file_meta_size);
    put_u64_le(header + 72, ROMFS_DATA_START);

    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_image_path);
    unlink(tmp_path);

    FILE* out = fopen(tmp_path, "wb");
    if (!out) {
        snprintf(err_buf, err_len, "Cannot create %.120s: %s", tmp_path, strerror(errno));
        free(dir_hash); free(file_hash); free(dir_table); free(file_table); free(copy_buf);
        tree_free(&tree);
        return false;
    }

    bool ok = true;
    uint64_t written_pos = 0;
    uint64_t bytes_done = 0;
    uint64_t last_cb_bytes = 0;

    if (fwrite(header, 1, sizeof(header), out) != sizeof(header) ||
        !write_zeros(out, ROMFS_DATA_START - sizeof(header))) {
        snprintf(err_buf, err_len, "Failed writing RomFS header.");
        ok = false;
    }
    written_pos = ROMFS_DATA_START;

    if (ok && progress_cb) {
        progress_cb(0, tree.total_file_bytes, 0, tree.file_count, user_data);
    }

    for (size_t i = 0; i < tree.file_count && ok; i++) {
        const RomfsFileEntry* f = &tree.files[i];
        uint64_t target = ROMFS_DATA_START + f->data_off;
        if (target > written_pos) {
            if (!write_zeros(out, target - written_pos)) {
                snprintf(err_buf, err_len, "Write failed (SD full or FAT32 >4GB limit).");
                ok = false;
                break;
            }
            written_pos = target;
        }

        FILE* in = fopen(f->full_path, "rb");
        if (!in) {
            snprintf(err_buf, err_len, "Cannot read %.120s", f->rel_path);
            ok = false;
            break;
        }

        uint64_t rem = f->size;
        while (rem > 0) {
            size_t want = (rem > 1024 * 1024) ? (1024 * 1024) : (size_t)rem;
            size_t got = fread(copy_buf, 1, want, in);
            if (got == 0) {
                snprintf(err_buf, err_len, "Unexpected EOF reading %.120s", f->rel_path);
                ok = false;
                break;
            }
            if (fwrite(copy_buf, 1, got, out) != got) {
                int e = errno;
                snprintf(err_buf, err_len,
                         (e == EFBIG || written_pos >= 0xFFFF0000ULL)
                             ? "Write failed: >4GB romfs.bin requires an exFAT microSD!"
                             : "SD write failed: %s",
                         strerror(e));
                ok = false;
                break;
            }
            rem -= got;
            written_pos += got;
            bytes_done += got;
            if (progress_cb && (bytes_done - last_cb_bytes >= 8ULL * 1024ULL * 1024ULL)) {
                last_cb_bytes = bytes_done;
                progress_cb(bytes_done, tree.total_file_bytes, i + 1, tree.file_count, user_data);
            }
        }
        fclose(in);
    }

    if (ok && meta_base > written_pos) {
        if (!write_zeros(out, meta_base - written_pos)) {
            snprintf(err_buf, err_len, "Failed writing RomFS padding.");
            ok = false;
        }
    }

    if (ok) {
        if (fwrite(dir_hash, 1, dir_hash_bytes, out) != dir_hash_bytes ||
            fwrite(dir_table, 1, (size_t)dir_meta_size, out) != (size_t)dir_meta_size ||
            fwrite(file_hash, 1, file_hash_bytes, out) != file_hash_bytes ||
            (file_meta_size > 0 && fwrite(file_table, 1, (size_t)file_meta_size, out) != (size_t)file_meta_size)) {
            snprintf(err_buf, err_len, "Failed writing RomFS metadata tables.");
            ok = false;
        }
    }

    if (fclose(out) != 0 && ok) {
        snprintf(err_buf, err_len, "Failed flushing romfs.bin to SD.");
        ok = false;
    }

    if (ok) {
        unlink(out_image_path);
        if (rename(tmp_path, out_image_path) != 0) {
            snprintf(err_buf, err_len, "Failed finalizing romfs.bin: %s", strerror(errno));
            unlink(tmp_path);
            ok = false;
        } else if (progress_cb) {
            progress_cb(tree.total_file_bytes, tree.total_file_bytes, tree.file_count, tree.file_count, user_data);
        }
    } else {
        unlink(tmp_path);
    }

    free(dir_hash);
    free(file_hash);
    free(dir_table);
    free(file_table);
    free(copy_buf);
    tree_free(&tree);
    return ok;
}
