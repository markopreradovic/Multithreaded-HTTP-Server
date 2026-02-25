#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

extern const char* document_root;

char* get_ini_value(const char* filename, const char* section,
                    const char* key, const char* default_val);

#endif // CONFIG_H