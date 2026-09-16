// SPDX-License-Identifier: MIT
#pragma once
#include "storage.h"
// dswifi declares the byte-order helpers in arpa/inet.h, not netinet/in.h.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
typedef struct {
    DevStore store;
    int tcp,udp,client,state;
    bool loader,launch,return_requested,asset_changed;
    unsigned char token[16],header[DEV_HEADER_SIZE],ack[DEV_ACK_SIZE];
    char name[DEV_MAX_NAME+1];
    size_t used,sent;
    uint32_t op,extra,build_crc;
    time_t activity;
    struct in_addr host;
    unsigned log_sequence;
} DevService;
bool dev_service_init(DevService *s, const char *root, const unsigned char token[16], bool loader);
void dev_service_poll(DevService *s, size_t byte_budget);
void dev_service_close(DevService *s);
void dev_service_log(DevService *s, const char *text);
