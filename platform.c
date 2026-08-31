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


void cleanup(void) MYCC {
    screen_restore();
    ZXN_NEXTREGA(0x07, oldspeed);
}

void init(void) MYCC {
    atexit(cleanup);
    oldspeed = ZXN_READ_REG(0x07) & 0x03;
    ZXN_NEXTREG(0x07, 3);
}

const char* get_lfn(const char* filepath) MYCC {
    cat.filter = ESX_CAT_FILTER_SYSTEM | ESX_CAT_FILTER_LFN;
    p3dos_copy_cstr_to_pstr(filename, filepath);
    cat.filename = filename;
    cat.cat_sz = 2;
    
    if (esx_dos_catalog(&cat) == 1) {
        char lfn_name[251];
        const char* sep1;
        const char* sep2;
        const char* sep;
        size_t dir_len;

        lfn.cat = &cat;
        esx_ide_get_lfn(&lfn, &cat.cat[1]);

        strncpy(lfn_name, filename, sizeof(lfn_name) - 1);
        lfn_name[sizeof(lfn_name) - 1] = 0;

        sep1 = strrchr(filepath, '/');
        sep2 = strrchr(filepath, '\\');
        sep = sep1;
        if (sep2 && (!sep || sep2 > sep)) {
            sep = sep2;
        }

        dir_len = sep ? (size_t)(sep - filepath + 1) : 0;
        if (dir_len > 250) {
            dir_len = 250;
        }

        if (dir_len > 0) {
            memcpy(filename, filepath, dir_len);
        }
        filename[dir_len] = 0;

        strncat(filename, lfn_name, 250 - dir_len);
        filename[250] = 0;

        return &filename[0];
    }

    strncpy(filename, filepath, 250);
    filename[250] = 0;
    return &filename[0];
}

void* open_file(const char* filename) MYCC {
    errno = 0;
    unsigned char f = esxdos_f_open(filename, ESXDOS_MODE_R | ESXDOS_MODE_OE);
    if (errno) return NULL;
    return (void*)f;
}

void* create_file(const char* filename) MYCC {
    errno = 0;
    unsigned char f = esxdos_f_open(filename, ESXDOS_MODE_W | ESXDOS_MODE_CT);
    if (errno) return NULL;
    return (void*)f;
}

void close_file(void* file) MYCC {
    if (file) {
        esxdos_f_close((unsigned char)file);
    }
}

int read_file(void* file, char* buffer, size_t size) MYCC {
    if (file) {
        return esxdos_f_read((unsigned char)file, buffer, size);
    }
    return 0;
}

int write_file(void* file, const char* buffer, size_t size) MYCC {
    if (file) {
        return esxdos_f_write((unsigned char)file, buffer, size);
    }
    return 0;
}

void rename_file(const char* oldname, const char* newname) MYCC {
    esx_f_unlink(newname); // remove old file if exists
    errno = 0;
    esx_f_rename(oldname, newname);
}