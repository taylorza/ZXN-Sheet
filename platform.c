#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <z80.h>
#include <arch/zxn.h>
#include <arch/zxn/esxdos.h>

#include "platform.h"
#include "crtio.h"

uint8_t oldspeed;

struct esx_cat cat;
struct esx_lfn lfn;
char *filename = &lfn.filename[0];
char tmpbuffer[256];


void cleanup(void) {
    screen_restore();
    ZXN_NEXTREGA(0x07, oldspeed);
}

void init(void) {
    atexit(cleanup);
    oldspeed = ZXN_READ_REG(0x07) & 0x03;
    ZXN_NEXTREG(0x07, 3);
}

const char* get_lfn(const char* filepath) {
    cat.filter = ESX_CAT_FILTER_SYSTEM | ESX_CAT_FILTER_LFN;
    p3dos_copy_cstr_to_pstr(filename, filepath);
    cat.filename = filename;
    cat.cat_sz = 2;
    
    if (esx_dos_catalog(&cat) == 1) {
        lfn.cat = &cat;
        esx_ide_get_lfn(&lfn, &cat.cat[1]);
        char *p = filepath + strlen(filepath);
        while (p > filepath && *(p - 1) != '/' && *(p - 1) != '\\') --p;
        strcpy(p, filename);            
    }
    strncpy(filename, filepath, 250);
    return &filename[0];
}

void* open_file(const char* filename) {
    errno = 0;
    unsigned char f = esxdos_f_open(filename, ESXDOS_MODE_R | ESXDOS_MODE_OE);
    if (errno) return NULL;
    return (void*)f;
}

void* create_file(const char* filename) {
    errno = 0;
    unsigned char f = esxdos_f_open(filename, ESXDOS_MODE_W | ESXDOS_MODE_CT);
    if (errno) return NULL;
    return (void*)f;
}

void close_file(void* file) {
    if (file) {
        esxdos_f_close((unsigned char)file);
    }
}

int read_file(void* file, char* buffer, size_t size) {
    if (file) {
        return esxdos_f_read((unsigned char)file, buffer, size);
    }
    return 0;
}

int write_file(void* file, const char* buffer, size_t size) {
    if (file) {
        return esxdos_f_write((unsigned char)file, buffer, size);
    }
    return 0;
}

void rename_file(const char* oldname, const char* newname) {
    esx_f_unlink(newname); // remove old file if exists
    errno = 0;
    esx_f_rename(oldname, newname);
}