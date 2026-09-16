// SPDX-License-Identifier: MIT
#ifdef DSIDEV_ENABLED
#include "dsidev.h"
#include "service.h"
#include <nds.h>
#include <dswifi9.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static DevService service;
static bool enabled,initialized;
static unsigned char token[16];
static struct in_addr host;
static uint32_t build_crc;
static void (*exit_callback)(void);
static void (*asset_callback)(const char*);
static Thread *main_thread;
static volatile bool crashed;
static void release_wifi(void);
static Thread watchdog_thread;
alignas(8) static unsigned char watchdog_stack[1024];
static unsigned ticks;

static void report_memory(const char *label) {
    struct mallinfo info=mallinfo();
    dsidev_log("%s frame=%u heap=%u used=%u free=%u headroom=%u",label,ticks,
        (unsigned)info.arena,(unsigned)info.uordblks,(unsigned)info.fordblks,
        (unsigned)(getHeapLimit()-getHeapEnd()));
}
// A hardware exception leaves no trace of its own: the console simply stops. Put the
// register dump on screen for whoever is holding it, then try to get it onto the network.
static void report_crash(ExcptContext *ctx, unsigned flags) {
    guruMeditationDump(ctx,flags);
    dsidev_log("CRASH flags=%u pc=%08lx lr=%08lx sp=%08lx psr=%08lx fault=%08lx",flags,
        (unsigned long)ctx->r[15],(unsigned long)ctx->r[14],(unsigned long)ctx->r[13],
        (unsigned long)ctx->cpsr,(unsigned long)getExceptionAddress(ctx,ctx->r[15]));
    report_memory("CRASH-MEM");
    // The watchdog thread performs the return: this context runs on a small exception
    // stack with the MPU reconfigured, which is no place to shut an application down.
    crashed=true;
    while(1) swiWaitForVBlank();  // interrupts stay live, so the watchdog still runs
}
// The service is driven from the main loop, so a main loop that stops takes the console
// off the network with it. This thread keeps running, and can read where the main thread
// was parked when it was last preempted.
static int watchdog(void *arg) {
    (void)arg;
    unsigned seen=0,stalled=0;
    for(;;) {
        threadSleep(1000000);
        if(crashed) {
            // Give the crash report time to leave the console, then hand the machine back
            // to the loader so the next build can be sent without a power cycle.
            threadSleep(2000000);
            dsidev_log("returning to loader after crash");
            if(initialized) dev_service_close(&service);
            release_wifi();
            fflush(NULL);
            if(pmHasResetJumpTarget()) exit(0);
            for(;;) threadSleep(1000000);  // launched without a return stub: nothing to do
        }
        if(ticks!=seen) {seen=ticks;stalled=0;continue;}
        stalled++;
        // Three seconds of no progress, then once every ten so the log shows if it moves.
        if(stalled==3 || (stalled>3 && stalled%10==0)) {
            dsidev_log("STALL %us frame=%u pc=%08lx lr=%08lx sp=%08lx psr=%08lx",stalled,ticks,
                (unsigned long)main_thread->ctx.r[15],(unsigned long)main_thread->ctx.r[14],
                (unsigned long)main_thread->ctx.r[13],(unsigned long)main_thread->ctx.psr);
            report_memory("STALL-MEM");
        }
    }
    return 0;
}
void dsidev_set_exit_callback(void (*callback)(void)) {exit_callback=callback;}
void dsidev_set_asset_callback(void (*callback)(const char*)) {asset_callback=callback;}
bool dsidev_wifi_ready(void) {return enabled;}
bool dsidev_wifi_wait(void) {
    // The runtime already called Wifi_InitDefault(INIT_ONLY)+Wifi_AutoConnect(), so game
    // code must wait for that association instead of initialising dswifi a second time.
    if(!enabled) return false;
    for(unsigned frames=0;frames<600 && pmMainLoop();frames++) {
        if(Wifi_AssocStatus()==ASSOCSTATUS_ASSOCIATED) return true;
        swiWaitForVBlank();
    }
    return false;
}
void dsidev_init(int argc,char **argv) {
    // Only a launch from our loader carries these arguments, and only it knows the token,
    // so the service stays off for a build started any other way.
    bool addressed=false,authorised=false;
    for(int i=1;i<argc;i++) {
        unsigned a,b,c,d;
        if(sscanf(argv[i],"--dsidev-host=%u.%u.%u.%u",&a,&b,&c,&d)==4 && (a|b|c|d)<=255) {
            host.s_addr=htonl(a<<24|b<<16|c<<8|d);
            addressed=true;
        }
        if(!strncmp(argv[i],"--dsidev-crc=",13)) build_crc=strtoul(argv[i]+13,NULL,16);
        if(!strncmp(argv[i],"--dsidev-token=",15)) authorised=dev_parse_token(argv[i]+15,token);
    }
    if(!addressed || !authorised) return;
    enabled=Wifi_InitDefault(INIT_ONLY);
    if(!enabled) return;
    Wifi_AutoConnect();
    setExceptionHandler(report_crash);
    main_thread=threadGetSelf();
    threadPrepare(&watchdog_thread,watchdog,NULL,&watchdog_stack[sizeof(watchdog_stack)],
        MAIN_THREAD_PRIO+1);
    threadStart(&watchdog_thread);
}
void dsidev_log(const char *format,...) {
    if(!initialized) return;
    static char text[640];va_list args;va_start(args,format);
    vsnprintf(text,sizeof(text),format,args);va_end(args);
    dev_service_log(&service,text);
}
static void release_wifi(void) {
    // Hand the radio over stopped. A chainloaded binary that inherits a live association
    // cannot initialise dswifi again, which costs it the development service entirely.
    if(!enabled) return;
    enabled=false;
    Wifi_DisconnectAP();
    wlmgrStop();
}
void dsidev_poll(void) {
    static unsigned waited;
    ticks++;
    if(initialized && ticks%600==0) report_memory("alive");
    if(enabled && !initialized) {
        if(Wifi_AssocStatus()==ASSOCSTATUS_ASSOCIATED) {
            if(dev_service_init(&service,"sd:/dsidev",token,false)) {
                initialized=true;service.host=host;service.build_crc=build_crc;
                dev_service_log(&service,"application ready");
                if(asset_callback && service.store.asset.valid) asset_callback(service.store.asset.path);
            }
        } else if(++waited%600==0) {
            Wifi_AutoConnect();  // ten seconds without an association: ask again
        }
    }
    if(initialized) {
        dev_service_poll(&service,8192);
        if(service.asset_changed) {
            service.asset_changed=false;
            // Callback runs at this main-loop safe point, never in an interrupt.
            if(asset_callback) asset_callback(service.store.asset.path);
        }
    }
    // Calico also detects the standard L+R+Start+Select return combination. This runs even
    // when the service never came up, so the combination always gets you back.
    if((initialized && service.return_requested) || !pmMainLoop()) {
        if(pmHasResetJumpTarget()) {
            dsidev_log("returning to loader");
            if(exit_callback) exit_callback();
            if(initialized) dev_service_close(&service);
            release_wifi();
            fflush(NULL);
            exit(0);
        }
        // A direct/manual launch might not have a return stub. Do not promise one.
        if(initialized) service.return_requested=false;
    }
}
#endif
