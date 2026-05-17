#ifndef __STRING_H
#define __STRING_H

#include "types.h"

int             memcmp(const void*, const void*, uint);
void*           memmove(void*, const void*, uint);
void*           memset(void*, int, uint);
char*           safestrcpy(char*, const char*, int);
int             strlen(const char*);
int             strncmp(const char*, const char*, uint);
char*           strncpy(char*, const char*, int);
char*           strchr(const char *s, char c);
void            snstr(char *dst, wchar const *src, int len);

#endif
