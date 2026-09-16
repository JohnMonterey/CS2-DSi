// SPDX-License-Identifier: GPL-2.0-or-later
// Links the devkitPro hbmenu loader (GPL-2.0-or-later).
#include <nds.h>
#include <fat.h>
#include <dswifi9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "service.h"
#include "nds_loader_arm9.h"
#include "token.h"

// Implemented by the small, checked patch to the pinned hbmenu loader.
bool devSetReturnPath(const char *path);
static DevService service;

static void launch(void) {
    const DevImage *app=&service.store.app;
    if(!app->valid || !dev_image_verify(app,true)) {iprintf("No verified build.\n");return;}
    if(!devSetReturnPath("sd:/dsidev.nds")) {iprintf("Missing sd:/dsidev.nds\n");return;}
    char path[DEV_PATH_SIZE],host[64],crc[48],secret[64];
    snprintf(path,sizeof(path),"%s",app->path);
    uint32_t ip=ntohl(service.host.s_addr);
    snprintf(host,sizeof(host),"--dsidev-host=%lu.%lu.%lu.%lu",
        (unsigned long)(ip>>24),(unsigned long)((ip>>16)&255),
        (unsigned long)((ip>>8)&255),(unsigned long)(ip&255));
    snprintf(crc,sizeof(crc),"--dsidev-crc=%08lx",(unsigned long)app->crc);
    // The build inherits the secret here, so the SD card carries one file and no secret.
    memcpy(secret,"--dsidev-token=",15);
    for(unsigned i=0;i<16;i++) snprintf(secret+15+i*2,3,"%02x",DEV_TOKEN[i]);
    const char *args[]={path,host,crc,secret};
    iprintf("Launching %08lx\n",(unsigned long)app->crc);
    dev_service_log(&service,"launching verified image");
    dev_service_close(&service);
    // Same reason as runtime/dsidev.c: the build we start must be able to init dswifi.
    Wifi_DisconnectAP();
    wlmgrStop();
    // Keep SD mounted for runNdsFile's stat/cluster lookup. All upload files are closed.
    fflush(NULL);
    int result=runNdsFile(path,4,args);
    iprintf("Chainload failed: %d\nRestart loader to retry.\n",result);
    while(pmMainLoop()) swiWaitForVBlank();
}

int main(void) {
    consoleDemoInit();
    iprintf("DSi development loader\n\n");
    if(!isDSiMode() || !fatInitDefault() || access("sd:/",F_OK)!=0) {
        iprintf("Needs DSi mode and SD access.\nLaunch via current hbmenu.\n");
        while(pmMainLoop()) swiWaitForVBlank();
        return 1;
    }
    if(!Wifi_InitDefault(INIT_ONLY)) {
        iprintf("Wi-Fi init failed.\n");
        while(pmMainLoop()) swiWaitForVBlank();
        return 1;
    }
    Wifi_AutoConnect();
    iprintf("Connecting to saved Wi-Fi...\n");
    bool running=false;
    while(pmMainLoop()) {
        scanKeys();
        if(!running && Wifi_AssocStatus()==ASSOCSTATUS_ASSOCIATED) {
            if(!dev_service_init(&service,"sd:/dsidev",DEV_TOKEN,true)) {
                iprintf("Service/SD init failed.\n");break;
            }
            running=true;
            uint32_t ip=ntohl(Wifi_GetIP());
            iprintf("%lu.%lu.%lu.%lu:%d\nA: launch latest\nB: reconnect Wi-Fi\nReady for make run-dsi\n",
                (unsigned long)(ip>>24),(unsigned long)((ip>>16)&255),
                (unsigned long)((ip>>8)&255),(unsigned long)(ip&255),DEV_PORT);
        }
        if(running) {
            dev_service_poll(&service,64*1024);
            if(service.launch || ((keysDown()&KEY_A) && service.client<0)) {
                // Clear first: a rejected image must not re-verify the whole file every frame.
                service.launch=false;
                launch();
            }
        }
        if(keysDown()&KEY_B) {
            if(running) dev_service_close(&service);
            running=false;Wifi_DisconnectAP();Wifi_AutoConnect();
            iprintf("Reconnecting...\n");
        }
        swiWaitForVBlank();
    }
    if(running) dev_service_close(&service);
    return 0;
}
