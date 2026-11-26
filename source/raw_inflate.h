/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RFC 1951 raw DEFLATE decompressor (equivalent to zlib inflate with wbits = -15).
 * Used to decode Switch 2 (FW 21.0.0+) NacpStruct TitlesDataFormat == 1 compressed titles. */
bool vesper_raw_inflate(const uint8_t* src, size_t src_len,
                        uint8_t* dst, size_t dst_cap,
                        size_t* out_len);
