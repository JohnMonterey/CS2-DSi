// SPDX-License-Identifier: MIT
#include "storage.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool durable_close(FILE *f) {
    bool ok=fflush(f)==0;
    if (ok) ok=fsync(fileno(f))==0;
    if (fclose(f)!=0) ok=false;
    return ok;
}
static bool requires_nds(int kind) {
    return kind==DEV_KIND_APP || kind==DEV_KIND_LOADER;
}
uint32_t dev_store_limit(int kind) {
    switch (kind) {
    case DEV_KIND_ASSET: return DEV_MAX_ASSET;
    case DEV_KIND_FILE:  return DEV_MAX_FILE;
    default:             return DEV_MAX_NDS;
    }
}
// s->name must already hold the validated name when kind is DEV_KIND_FILE.
static void paths(DevStore *s, int kind, unsigned slot, char *data, char *record) {
    switch (kind) {
    case DEV_KIND_ASSET:
        snprintf(data,DEV_PATH_SIZE,"%s/asset%u.bin",s->root,slot);
        snprintf(record,DEV_PATH_SIZE,"%s/asset%u.rec",s->root,slot);
        break;
    case DEV_KIND_FILE:
        snprintf(data,DEV_PATH_SIZE,"%s/%s",s->root,s->name);
        snprintf(record,DEV_PATH_SIZE,"%s/%s.rec",s->root,s->name);
        break;
    case DEV_KIND_LOADER:
        // The loader sits beside its own directory: sd:/dsidev -> sd:/dsidev.nds
        snprintf(data,DEV_PATH_SIZE,"%s.nds",s->root);
        snprintf(record,DEV_PATH_SIZE,"%s/loader.rec",s->root);
        break;
    default:
        snprintf(data,DEV_PATH_SIZE,"%s/app%u.nds",s->root,slot);
        snprintf(record,DEV_PATH_SIZE,"%s/app%u.rec",s->root,slot);
        break;
    }
}
static bool accept_name(DevStore *s, int kind, const char *name) {
    if (kind!=DEV_KIND_FILE) {s->name[0]=0; return true;}
    if (!name || !dev_name_valid(name)) return false;
    strcpy(s->name,name);
    return true;
}
static bool read_record(const char *path, uint32_t *generation, uint32_t *size, uint32_t *crc) {
    FILE *f=fopen(path,"rb");
    if (!f) return false;
    unsigned char rec[24];
    bool ok=fread(rec,1,sizeof(rec),f)==sizeof(rec) && fgetc(f)==EOF;
    if (fclose(f)!=0) ok=false;
    if (!ok || memcmp(rec,"DSIREC1",8) || dev_u32(rec+20)!=dev_crc32(0,rec,20)) return false;
    *generation=dev_u32(rec+8); *size=dev_u32(rec+12); *crc=dev_u32(rec+16);
    return true;
}
static bool write_record(const char *path, uint32_t generation, uint32_t size, uint32_t crc) {
    unsigned char rec[24]={0};
    memcpy(rec,"DSIREC1",8);
    dev_put32(rec+8,generation);dev_put32(rec+12,size);dev_put32(rec+16,crc);
    dev_put32(rec+20,dev_crc32(0,rec,20));
    FILE *f=fopen(path,"wb");
    if (!f) return false;
    bool ok=fwrite(rec,1,sizeof(rec),f)==sizeof(rec);
    if (!durable_close(f)) ok=false;
    if (!ok) remove(path);
    return ok;
}
bool dev_image_verify(const DevImage *image, bool require_nds) {
    FILE *f=fopen(image->path,"rb");
    if (!f) return false;
    unsigned char buffer[8192];
    uint32_t crc=0,total=0;
    bool ok=true;
    size_t n;
    while ((n=fread(buffer,1,sizeof(buffer),f))) {
        if (require_nds && total==0 && (n<512 || !dev_nds_valid(buffer,image->size))) {ok=false;break;}
        if (total>image->size || n>image->size-total) {ok=false;break;}
        total+=n; crc=dev_crc32(crc,buffer,n);
    }
    if (ferror(f)) ok=false;
    if (fclose(f)!=0) ok=false;
    return ok && total==image->size && crc==image->crc;
}
static DevImage recover(DevStore *s, int kind) {
    DevImage best={0};
    for (unsigned slot=0;slot<2;slot++) {
        DevImage image={.slot=slot};
        char recpath[DEV_PATH_SIZE];
        paths(s,kind,slot,image.path,recpath);
        if (!read_record(recpath,&image.generation,&image.size,&image.crc)) continue;
        if (!image.size || image.size>dev_store_limit(kind)) continue;
        if (!dev_image_verify(&image,requires_nds(kind))) continue;
        image.valid=true;
        if (!best.valid || image.generation>best.generation) best=image;
    }
    return best;
}
bool dev_store_init(DevStore *s, const char *root) {
    memset(s,0,sizeof(*s));
    if (strlen(root)>=DEV_ROOT_SIZE) return false;
    strcpy(s->root,root);
    if (mkdir(root,0777)!=0 && errno!=EEXIST) return false;
    s->app=recover(s,DEV_KIND_APP); s->asset=recover(s,DEV_KIND_ASSET);
    return true;
}
void dev_store_abort(DevStore *s) {
    if (s->file) {fclose(s->file);s->file=NULL;}
    if (s->part[0]) remove(s->part);
    s->part[0]=0;
}
bool dev_store_present(DevStore *s, int kind, const char *name, uint32_t size, uint32_t crc) {
    if (kind==DEV_KIND_APP) return s->app.valid && s->app.size==size && s->app.crc==crc;
    if (kind==DEV_KIND_ASSET) return s->asset.valid && s->asset.size==size && s->asset.crc==crc;
    if (!accept_name(s,kind,name)) return false;
    // Named files are matched by their record: the contents were verified when written.
    char data[DEV_PATH_SIZE],record[DEV_PATH_SIZE];
    uint32_t generation,stored_size,stored_crc;
    paths(s,kind,0,data,record);
    return read_record(record,&generation,&stored_size,&stored_crc)
        && stored_size==size && stored_crc==crc && access(data,F_OK)==0;
}
bool dev_store_begin(DevStore *s, int kind, const char *name, uint32_t size, uint32_t crc) {
    if (!size || size>dev_store_limit(kind)) return false;
    dev_store_abort(s);
    if (!accept_name(s,kind,name)) return false;
    s->kind=kind;s->size=size;s->expected_crc=crc;s->received=0;s->crc=0;
    if (kind==DEV_KIND_APP || kind==DEV_KIND_ASSET) {
        DevImage *current=kind==DEV_KIND_ASSET?&s->asset:&s->app;
        if (current->valid && current->generation==UINT32_MAX) return false;
        s->slot=current->valid?1-current->slot:0;
        s->generation=current->valid?current->generation+1:1;
    } else {
        char data[DEV_PATH_SIZE],record[DEV_PATH_SIZE];
        uint32_t generation,stored_size,stored_crc;
        paths(s,kind,0,data,record);
        s->slot=0;
        s->generation=read_record(record,&generation,&stored_size,&stored_crc)&&generation<UINT32_MAX
            ? generation+1 : 1;
    }
    paths(s,kind,s->slot,s->destination,s->record);
    snprintf(s->part,sizeof(s->part),"%s/upload.part",s->root);
    s->file=fopen(s->part,"wb");
    return s->file!=NULL;
}
bool dev_store_write(DevStore *s, const void *data, size_t size) {
    if (!s->file || size>s->size-s->received || fwrite(data,1,size,s->file)!=size) return false;
    s->crc=dev_crc32(s->crc,data,size);s->received+=size;
    return true;
}
unsigned dev_store_commit(DevStore *s) {
    if (!s->file) return DEV_IO_ERROR;
    bool ok=durable_close(s->file);s->file=NULL;
    if (!ok) return DEV_IO_ERROR;
    if (s->received!=s->size || s->crc!=s->expected_crc) return DEV_BAD_CRC;
    bool nds=requires_nds(s->kind);
    DevImage image={.valid=true,.slot=s->slot,.generation=s->generation,.size=s->size,.crc=s->crc};
    strcpy(image.path,s->part);
    // Reopen and verify the closed file before publishing it.
    if (!dev_image_verify(&image,nds)) return nds?DEV_BAD_NDS:DEV_BAD_CRC;
    // For A/B kinds this only touches the inactive slot; the live copy is never at risk.
    if (remove(s->record)!=0 && errno!=ENOENT) return DEV_IO_ERROR;
    if (remove(s->destination)!=0 && errno!=ENOENT) return DEV_IO_ERROR;
    if (rename(s->part,s->destination)!=0) return DEV_IO_ERROR;
    s->part[0]=0;
    if (!write_record(s->record,s->generation,s->size,s->crc)) return DEV_IO_ERROR;
    strcpy(image.path,s->destination);
    if (s->kind==DEV_KIND_APP) s->app=image;
    else if (s->kind==DEV_KIND_ASSET) s->asset=image;
    return DEV_COMMITTED;
}
