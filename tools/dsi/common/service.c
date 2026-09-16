// SPDX-License-Identifier: MIT
#include "service.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifdef ARM9
// sgIP sockets are not newlib file descriptors.
#define socket_close closesocket
#else
#define socket_close close
#endif

enum { HEADER, NAME, READY, BODY, FINAL, GO };
static bool again(void) {return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR;}
static void drop(DevService *s) {
    if (s->client>=0) {shutdown(s->client,SHUT_RDWR);socket_close(s->client);s->client=-1;}
    dev_store_abort(&s->store);
    s->used=s->sent=0;s->state=HEADER;
}
static int bound_socket(int type) {
    int fd=socket(AF_INET,type,0),one=1;
    if(fd<0) return -1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons(DEV_PORT);addr.sin_addr.s_addr=INADDR_ANY;
    if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0 || ioctl(fd,FIONBIO,&one)<0) {socket_close(fd);return -1;}
    return fd;
}
bool dev_service_init(DevService *s, const char *root, const unsigned char token[16], bool loader) {
    memset(s,0,sizeof(*s));s->tcp=s->udp=s->client=-1;s->loader=loader;
    memcpy(s->token,token,16);
    if (!dev_store_init(&s->store,root)) return false;
    s->tcp=bound_socket(SOCK_STREAM);s->udp=bound_socket(SOCK_DGRAM);
    if(s->tcp<0 || s->udp<0 || listen(s->tcp,1)<0) {dev_service_close(s);return false;}
    return true;
}
void dev_service_close(DevService *s) {
    drop(s);
    if(s->tcp>=0) socket_close(s->tcp);
    if(s->udp>=0) socket_close(s->udp);
    s->tcp=s->udp=-1;
}
void dev_service_log(DevService *s, const char *text) {
    if(s->udp<0 || !s->host.s_addr) return;
    static char line[768];
    int n=snprintf(line,sizeof(line),"DSILOG1 %08lx %u %s",(unsigned long)s->build_crc,s->log_sequence++,text);
    if(n<0) return;
    if((size_t)n>=sizeof(line)) n=sizeof(line)-1;
    struct sockaddr_in dest={0};dest.sin_family=AF_INET;dest.sin_port=htons(DEV_LOG_PORT);dest.sin_addr=s->host;
    sendto(s->udp,line,n,0,(struct sockaddr*)&dest,sizeof(dest));
}
static void udp_poll(DevService *s) {
    for(unsigned i=0;i<4;i++) {
        unsigned char data[128];struct sockaddr_in peer;socklen_t len=sizeof(peer);
        int n=recvfrom(s->udp,data,sizeof(data),0,(struct sockaddr*)&peer,&len);
        if(n<0) break;
        if(n==8 && !memcmp(data,DEV_QUERY,8)) {
            char reply[96];int size=snprintf(reply,sizeof(reply),"DSIDEV1 %s %08lx %lu",
                s->loader?"loader":"app",(unsigned long)(s->loader?s->store.app.crc:s->build_crc),
                (unsigned long)s->store.app.generation);
            sendto(s->udp,reply,size,0,(struct sockaddr*)&peer,len);
        } else if(n==24 && !memcmp(data,DEV_RESET,8) && !memcmp(data+8,s->token,16)) {
            sendto(s->udp,"DSIRETURN",9,0,(struct sockaddr*)&peer,len);
            if(!s->loader) s->return_requested=true;
        }
    }
}
static void ack(DevService *s,unsigned code,uint32_t detail,bool ready) {
    memcpy(s->ack,DEV_ACK,8);dev_put32(s->ack+8,code);dev_put32(s->ack+12,detail);
    s->sent=0;s->state=ready?READY:FINAL;
}
static int kind_of(uint32_t op) {
    switch(op) {
    case DEV_ASSET: return DEV_KIND_ASSET;
    case DEV_FILE: return DEV_KIND_FILE;
    case DEV_LOADER: return DEV_KIND_LOADER;
    default: return DEV_KIND_APP;
    }
}
// Runs once everything the destination depends on is known: immediately for the fixed
// kinds, and after the name has been read for DEV_FILE.
static void start(DevService *s) {
    uint32_t size=dev_u32(s->header+12),crc=dev_u32(s->header+16);
    int kind=kind_of(s->op);
    const char *name=s->op==DEV_FILE?s->name:NULL;
    if(s->op==DEV_FILE && !dev_name_valid(name)) {ack(s,DEV_BAD_NAME,0,false);return;}
    if(!size || size>dev_store_limit(kind)) {ack(s,DEV_BAD_REQUEST,0,false);return;}
    if(dev_store_present(&s->store,kind,name,size,crc)) {ack(s,DEV_UNCHANGED,crc,false);return;}
    if(!dev_store_begin(&s->store,kind,name,size,crc)) {ack(s,DEV_IO_ERROR,0,false);return;}
    ack(s,DEV_READY,0,true);
}
static void header(DevService *s) {
    uint32_t extra=dev_u32(s->header+20);
    s->op=dev_u32(s->header+8);
    if(memcmp(s->header,DEV_MAGIC,8) || s->op<DEV_RUN || s->op>DEV_LOADER) {
        ack(s,DEV_BAD_REQUEST,0,false);return;
    }
    if(memcmp(s->header+24,s->token,16)) {ack(s,DEV_UNAUTHORIZED,0,false);return;}
    // A running build only accepts assets; everything else goes through the loader, which
    // is not competing with the game for the SD card.
    if(!s->loader && s->op!=DEV_ASSET) {ack(s,DEV_USE_LOADER,0,false);return;}
    if(s->op==DEV_FILE) {
        if(extra<1 || extra>DEV_MAX_NAME) {ack(s,DEV_BAD_NAME,0,false);return;}
        s->extra=extra;s->used=0;s->state=NAME;return;
    }
    if(extra) {ack(s,DEV_BAD_REQUEST,0,false);return;}
    if(s->op==DEV_LAUNCH) {
        ack(s,s->store.app.valid?DEV_UNCHANGED:DEV_BAD_REQUEST,s->store.app.crc,false);return;
    }
    start(s);
}
void dev_service_poll(DevService *s,size_t budget) {
    udp_poll(s);
    if(s->client<0) {
        struct sockaddr_in peer;socklen_t len=sizeof(peer);
        s->client=accept(s->tcp,(struct sockaddr*)&peer,&len);
        if(s->client<0) return;
        int one=1;
        if(ioctl(s->client,FIONBIO,&one)<0) {drop(s);return;}
        s->host=peer.sin_addr;s->state=HEADER;s->used=0;s->activity=time(NULL);
    }
    if(time(NULL)-s->activity>15) {drop(s);return;}
    static unsigned char buffer[8192];
    for(unsigned steps=0;steps<32 && budget;steps++) {
        if(s->state==READY || s->state==FINAL) {
            int n=send(s->client,s->ack+s->sent,DEV_ACK_SIZE-s->sent,0);
            if(n<0 && again()) break;
            if(n<=0) {drop(s);break;}
            s->sent+=n;s->activity=time(NULL);
            if(s->sent<DEV_ACK_SIZE) break;
            if(s->state==READY) s->state=BODY;
            else if(dev_u32(s->ack+8)<100 && (s->op==DEV_RUN || s->op==DEV_LAUNCH)) s->state=GO;
            else {drop(s);break;}
        } else if(s->state==GO) {
            char go;int n=recv(s->client,&go,1,0);
            if(n<0 && again()) break;
            if(n==1 && go=='G') s->launch=true;
            drop(s);break;
        } else if(s->state==NAME) {
            int n=recv(s->client,s->name+s->used,s->extra-s->used,0);
            if(n<0 && again()) break;
            if(n<=0) {drop(s);break;}
            s->used+=n;s->activity=time(NULL);
            if(s->used==s->extra) {s->name[s->extra]=0;start(s);}
        } else if(s->state==HEADER) {
            int n=recv(s->client,s->header+s->used,DEV_HEADER_SIZE-s->used,0);
            if(n<0 && again()) break;
            if(n<=0) {drop(s);break;}
            s->used+=n;s->activity=time(NULL);
            if(s->used==DEV_HEADER_SIZE) header(s);
        } else {
            size_t wanted=s->store.size-s->store.received;
            if(wanted>sizeof(buffer)) wanted=sizeof(buffer);
            if(wanted>budget) wanted=budget;
            int n=recv(s->client,buffer,wanted,0);
            if(n<0 && again()) break;
            if(n<=0) {drop(s);break;}
            s->activity=time(NULL);budget-=n;
            if(!dev_store_write(&s->store,buffer,n)) {ack(s,DEV_IO_ERROR,0,false);continue;}
            if(s->store.received==s->store.size) {
                unsigned code=dev_store_commit(&s->store);
                if(code==DEV_COMMITTED && s->op==DEV_ASSET) s->asset_changed=true;
                ack(s,code,s->store.crc,false);
            }
        }
    }
}
