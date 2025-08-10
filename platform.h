#ifndef PLATFORM_H__
#define PLATFORM_H__

#include <z80.h>
#include <arch/zxn.h>

#ifdef __SDCC
#define MYCC __sdcccall(1)
#else
#define MYCC
#endif

void cleanup(void);
void init(void);

const char* get_lfn(const char* filepath);

void* open_file(const char* filename);
void* create_file(const char* filename);
void close_file(void* file);
int read_file(void* file, char* buffer, size_t size);
int write_file(void* file, const char* buffer, size_t size);
void rename_file(const char* oldname, const char* newname);

extern char *filename;
extern char tmpbuffer[256];

#endif //PLATFORM_H__

