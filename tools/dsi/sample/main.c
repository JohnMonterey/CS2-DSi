// SPDX-License-Identifier: MIT
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include "dsidev.h"
static void asset(const char *path) {
    char text[96]={0};FILE *f=fopen(path,"rb");
    if(f) {fread(text,1,sizeof(text)-1,f);fclose(f);}
    iprintf("Asset: %s\n",text);
    dsidev_log("asset reloaded: %s",text);
}
int main(int argc,char **argv) {
    consoleDemoInit();fatInitDefault();
    dsidev_set_asset_callback(asset);dsidev_init(argc,argv);
    iprintf("DSi iteration smoke test\nL+R+Start+Select: return\nA: emit a log\n");
    unsigned frame=0;
    while(pmMainLoop()) {
        scanKeys();dsidev_poll();
        if(keysDown()&KEY_A) dsidev_log("A pressed at frame %u",frame);
        if(frame++%300==0) dsidev_log("heartbeat frame=%u DSi=%d",frame,isDSiMode());
        swiWaitForVBlank();
    }
    return 0;
}
