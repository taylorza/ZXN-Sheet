#ifndef PLATFORM_H__
#define PLATFORM_H__

#include <z80.h>
#include <arch/zxn.h>

#ifdef __SDCC
#define MYCC __sdcccall(1)
#else
#define MYCC
#endif

void cleanup(void) MYCC;
void init(void) MYCC;

const char* get_lfn(const char* filepath) MYCC;

void* open_file(const char* filename) MYCC;
void* create_file(const char* filename) MYCC;
void close_file(void* file) MYCC;
int read_file(void* file, char* buffer, size_t size) MYCC;
int write_file(void* file, const char* buffer, size_t size) MYCC;
void rename_file(const char* oldname, const char* newname) MYCC;

extern char *filename;
extern char tmpbuffer[256];

#endif //PLATFORM_H__

