/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*VesperRomfsProgressFn)(uint64_t bytes_done, uint64_t bytes_total,
                                      size_t files_done, size_t files_total,
                                      void* user_data);

/* Quickly counts regular files & total bytes in a directory (stops early if max_files > 0 is exceeded).
 * Returns true on success. */
bool vesper_romfs_count_dir(const char* dir_path, size_t max_files,
                            size_t* out_files, uint64_t* out_bytes);

/* Builds a standard unencrypted Nintendo Switch RomFS image (romfs.bin) directly from
 * an extracted romfs/ directory tree. Writes atomically via <out_image>.tmp -> rename. */
bool vesper_romfs_build_image(const char* romfs_dir,
                              const char* out_image_path,
                              VesperRomfsProgressFn progress_cb,
                              void* user_data,
                              char* err_buf,
                              size_t err_len);
