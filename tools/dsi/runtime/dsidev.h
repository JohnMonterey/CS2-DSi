// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#ifdef DSIDEV_ENABLED
void dsidev_init(int argc, char **argv);
void dsidev_poll(void);
void dsidev_log(const char *format, ...) __attribute__((format(printf,1,2)));
void dsidev_set_exit_callback(void (*callback)(void));
void dsidev_set_asset_callback(void (*callback)(const char *path));
bool dsidev_wifi_ready(void);
bool dsidev_wifi_wait(void);
#else
static inline void dsidev_init(int argc,char **argv) {(void)argc;(void)argv;}
static inline void dsidev_poll(void) {}
#define dsidev_log(...) ((void)0)
static inline void dsidev_set_exit_callback(void (*callback)(void)) {(void)callback;}
static inline void dsidev_set_asset_callback(void (*callback)(const char*)) {(void)callback;}
static inline bool dsidev_wifi_ready(void) {return false;}
static inline bool dsidev_wifi_wait(void) {return false;}
#endif
