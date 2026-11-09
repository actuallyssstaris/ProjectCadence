#ifndef OSU5_MD5_H
#define OSU5_MD5_H

#include <stdbool.h>

bool md5_file(const char *path, char hex[33]);
void md5_text(const char *s, char hex[33]);

#endif
