// SPDX-License-Identifier: MIT
#include "protocol.h"

uint32_t dev_crc32(uint32_t crc, const void *data, size_t size) {
    static uint32_t table[256];
    static bool initialized;
    if (!initialized) {
        for (unsigned i=0;i<256;i++) {
            uint32_t c=i;
            for (unsigned j=0;j<8;j++) c=(c>>1)^((c&1)?0xedb88320u:0);
            table[i]=c;
        }
        initialized=true;
    }
    const unsigned char *p=data;
    crc=~crc;
    while (size--) crc=table[(crc^*p++)&255]^(crc>>8);
    return ~crc;
}

bool dev_name_valid(const char *name) {
    size_t length=0;
    for (const char *p=name;*p;p++,length++) {
        char c=*p;
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-')) return false;
    }
    // A leading dot also rules out "." and "..", so no name can escape the directory.
    return length>0 && length<=DEV_MAX_NAME && name[0]!='.';
}

bool dev_parse_token(const char *hex, unsigned char token[16]) {
    for (unsigned i=0;i<32;i++) {
        // A short string stops here on its terminator, so this never reads past the end.
        char c=hex[i];
        unsigned nibble=c>='0'&&c<='9'?(unsigned)(c-'0'):(c>='a'&&c<='f'?(unsigned)(c-'a'+10):16u);
        if (nibble>15) return false;
        token[i/2]=(i&1)?(unsigned char)((token[i/2]<<4)|nibble):(unsigned char)nibble;
    }
    return hex[32]=='\0';
}

static uint32_t little32(const unsigned char *p) {
    return (uint32_t)p[3]<<24 | (uint32_t)p[2]<<16 | (uint32_t)p[1]<<8 | p[0];
}

bool dev_nds_valid(const unsigned char *h, uint32_t size) {
    if (size<512) return false;
    uint16_t crc=0xffff;
    for (unsigned i=0;i<0x15e;i++) {
        crc^=h[i];
        for (unsigned b=0;b<8;b++) crc=(crc>>1)^((crc&1)?0xa001:0);
    }
    if (crc!=(uint16_t)(h[0x15e]|h[0x15f]<<8)) return false;
    for (unsigned cpu=0;cpu<2;cpu++) {
        const unsigned char *p=h+0x20+cpu*16;
        uint32_t offset=little32(p), length=little32(p+12);
        if (offset<512 || !length || offset>size || length>size-offset) return false;
    }
    // DSi-enhanced ARM9i/ARM7i sections, if present, must also fit the file.
    if (h[0x12]&2) {
        for (unsigned cpu=0;cpu<2;cpu++) {
            const unsigned char *p=h+0x1c0+cpu*16;
            uint32_t offset=little32(p), length=little32(p+12);
            if (length && (offset<512 || offset>size || length>size-offset)) return false;
        }
    }
    return true;
}
