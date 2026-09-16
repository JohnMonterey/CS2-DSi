// SPDX-License-Identifier: MIT
// Host harness for the device service.
//
// It links the real common/ sources (protocol.c, storage.c, service.c) that run on the
// DSi and drives them with the same control flow as loader/main.c and runtime/dsidev.c,
// so the wire protocol and the SD store can be tested on macOS without hardware. Only
// the parts that need real hardware are absent: Wi-Fi association and the chainload.
//
// The token arrives in a file here; on hardware the loader has it compiled in and passes
// it to the launched build on the command line. Either way the service sees 16 bytes.
//
//   host_service <root-dir> <token-file> loader|app [mac-ip] [build-crc-hex]
//
// Events are written to stdout, one per line: READY, LAUNCH, LAUNCH-FAILED, ASSET, RETURN.
#include "service.h"
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DevService service;

// Exercises the hex token handover the loader performs on hardware, where the encoder
// (loader/main.c) and the decoder (runtime/dsidev.c) never meet on this machine.
static int selftest(void) {
    unsigned char expected[16], parsed[16];
    char hex[33];
    for (unsigned round = 0; round < 16; round++) {
        for (unsigned i = 0; i < 16; i++) expected[i] = (unsigned char)(round * 16 + i);
        for (unsigned i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", expected[i]);
        if (!dev_parse_token(hex, parsed) || memcmp(parsed, expected, sizeof(parsed)) != 0) {
            printf("FAIL round %u (%s)\n", round, hex);
            return 1;
        }
    }
    const char *rejected[] = {"", "0", "0123456789abcdef0123456789abcde",
                              "0123456789abcdef0123456789abcdefg", "0123456789abcdef0123456789abcde ",
                              "0123456789ABCDEF0123456789ABCDEF", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", NULL};
    for (unsigned i = 0; rejected[i]; i++) {
        if (dev_parse_token(rejected[i], parsed)) {
            printf("FAIL accepted %s\n", rejected[i]);
            return 1;
        }
    }
    printf("SELFTEST OK\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "selftest") == 0) return selftest();
    if (argc < 4) {
        fprintf(stderr, "usage: host_service <root> <token> loader|app [ip] [crc]\n");
        return 2;
    }
    bool loader = strcmp(argv[3], "loader") == 0;
    if (!loader && strcmp(argv[3], "app") != 0) return 2;
    // The DSi has no signals; a host peer that hangs up mid-send would kill this process.
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    unsigned char token[16];
    FILE *f = fopen(argv[2], "rb");
    bool ok = f && fread(token, 1, sizeof(token), f) == sizeof(token);
    if (f) fclose(f);
    if (!ok) {
        fprintf(stderr, "host_service: cannot read 16 token bytes from %s\n", argv[2]);
        return 2;
    }
    if (!dev_service_init(&service, argv[1], token, loader)) {
        fprintf(stderr, "host_service: service init failed\n");
        return 1;
    }
    if (!loader) {
        // runtime/dsidev.c takes these from the loader's command line arguments.
        if (argc > 4) service.host.s_addr = inet_addr(argv[4]);
        if (argc > 5) service.build_crc = strtoul(argv[5], NULL, 16);
        dev_service_log(&service, "application ready");
        if (service.store.asset.valid) printf("ASSET %s\n", service.store.asset.path);
    }
    printf("READY %s %08lx\n", loader ? "loader" : "app",
           (unsigned long)(loader ? service.store.app.crc : service.build_crc));

    for (;;) {
        dev_service_poll(&service, loader ? 64 * 1024 : 8 * 1024);
        if (loader && service.launch) {
            service.launch = false;
            const DevImage *app = &service.store.app;
            if (app->valid && dev_image_verify(app, true)) {
                printf("LAUNCH %08lx %s\n", (unsigned long)app->crc, app->path);
                dev_service_close(&service);
                return 0;  // the loader chainloads here and never comes back
            }
            printf("LAUNCH-FAILED\n");
        }
        if (!loader && service.asset_changed) {
            service.asset_changed = false;
            printf("ASSET %s\n", service.store.asset.path);
        }
        if (!loader && service.return_requested) {
            dev_service_log(&service, "returning to loader");
            printf("RETURN\n");
            dev_service_close(&service);
            return 0;  // the application exits back to the loader here
        }
        usleep(2000);  // stands in for waiting on the next video frame
    }
}
