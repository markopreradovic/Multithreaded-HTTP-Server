#include "config.h"

const char* document_root = "./www";

char* get_ini_value(const char* filename, const char* section,
                    const char* key, const char* default_val) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        printf("[Config] Cannot open %s, using defaults\n", filename);
        return strdup(default_val);
    }

    char line[256];
    char* value = NULL;
    int in_section = 0;

    while (fgets(line, sizeof(line), fp)) {
        char* p = line;
        while (*p && isspace(*p)) p++;
        char* end = p + strlen(p) - 1;
        while (end >= p && isspace(*end)) *end-- = '\0';

        if (*p == '[') {
            char sec[64];
            sscanf(p, "[%63[^]]]", sec);
            in_section = (strcmp(sec, section) == 0);
            continue;
        }

        if (!in_section) continue;

        char k[64], v[128];
        if (sscanf(p, "%63[^=] = %127[^\n]", k, v) == 2) {
            char* kp = k;
            while (*kp && isspace(*kp)) kp++;
            char* kend = kp + strlen(kp) - 1;
            while (kend >= kp && isspace(*kend)) *kend-- = '\0';

            if (strcmp(kp, key) == 0) {
                value = strdup(v);
                break;
            }
        }
    }

    fclose(fp);
    return value ? value : strdup(default_val);
}