// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Ports are compile-time constants on hardware; the host test harness overrides them.
#ifndef DEV_PORT
#define DEV_PORT 17491
#endif
#ifndef DEV_LOG_PORT
#define DEV_LOG_PORT 17493
#endif
#define DEV_HEADER_SIZE 40
#define DEV_ACK_SIZE 16
#define DEV_MAX_NDS (32u * 1024 * 1024)
#define DEV_MAX_ASSET (8u * 1024 * 1024)
#define DEV_MAX_FILE (16u * 1024 * 1024)
#define DEV_MAX_NAME 64
#define DEV_MAGIC "DSIUPL1"
#define DEV_ACK "DSIACK1"
#define DEV_QUERY "DSIDEV1?"
#define DEV_RESET "DSIRESET"
enum { DEV_RUN=1, DEV_STORE=2, DEV_LAUNCH=3, DEV_ASSET=4, DEV_FILE=5, DEV_LOADER=6 };
enum { DEV_READY=0, DEV_COMMITTED=1, DEV_UNCHANGED=2,
       DEV_BAD_REQUEST=100, DEV_UNAUTHORIZED=101, DEV_IO_ERROR=102,
       DEV_BAD_CRC=103, DEV_BAD_NDS=104, DEV_USE_LOADER=105, DEV_BAD_NAME=106 };

static inline uint32_t dev_u32(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static inline void dev_put32(unsigned char *p, uint32_t v) {
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}
uint32_t dev_crc32(uint32_t crc, const void *data, size_t size);
// Decodes the 32 lowercase hex digits the loader passes to a launched build.
bool dev_parse_token(const char *hex, unsigned char token[16]);
bool dev_nds_valid(const unsigned char *header, uint32_t size);
// Accepts a plain file name for the deployment directory: no separators, no traversal.
bool dev_name_valid(const char *name);
