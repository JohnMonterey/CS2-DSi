// SPDX-License-Identifier: MIT
#pragma once
#include "protocol.h"
#include <stdio.h>
#define DEV_PATH_SIZE 256
// Bounding the root lets the compiler prove every path below fits DEV_PATH_SIZE.
#define DEV_ROOT_SIZE (DEV_PATH_SIZE - 96)
// APP and ASSET alternate between two slots, so a failed write can never damage the copy
// in use. FILE (data beside the build) and LOADER (the card's own dsidev.nds) replace a
// fixed path, verified before the old one is dropped.
enum { DEV_KIND_APP, DEV_KIND_ASSET, DEV_KIND_FILE, DEV_KIND_LOADER };
typedef struct {
    bool valid;
    unsigned slot;
    uint32_t generation, size, crc;
    char path[DEV_PATH_SIZE];
} DevImage;
typedef struct {
    char root[DEV_ROOT_SIZE];
    DevImage app, asset;
    FILE *file;
    int kind;
    unsigned slot;
    uint32_t generation, size, expected_crc, received, crc;
    char name[DEV_MAX_NAME + 1];
    char part[DEV_PATH_SIZE], destination[DEV_PATH_SIZE], record[DEV_PATH_SIZE];
} DevStore;
bool dev_store_init(DevStore *s, const char *root);
uint32_t dev_store_limit(int kind);
// `name` applies to DEV_KIND_FILE only; pass NULL for the other kinds.
bool dev_store_present(DevStore *s, int kind, const char *name, uint32_t size, uint32_t crc);
bool dev_store_begin(DevStore *s, int kind, const char *name, uint32_t size, uint32_t crc);
bool dev_store_write(DevStore *s, const void *data, size_t size);
unsigned dev_store_commit(DevStore *s);
void dev_store_abort(DevStore *s);
bool dev_image_verify(const DevImage *image, bool require_nds);
