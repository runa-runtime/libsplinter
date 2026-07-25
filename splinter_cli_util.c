/**
 * Copyright 2025 Tim Post
 * License: Apache 2 (MIT available upon request to timthepost@protonmail.com)
 *
 * @file splinter_cli_util.c
 * @brief Splinter CLI utilties for working with modules
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <linux/limits.h>
#include "splinter_cli.h"

// Give some approximation of what bloom bits are set
static const char *fmt_binary(uint64_t mask) {
    static char b[65];
    b[64] = '\0';
    for (int i = 0; i < 64; i++) {
        b[63 - i] = (mask & (1ULL << i)) ? '1' : '0';
    }
    // Return pointer to first '1' or the last '0' to avoid a full 
    // bloom64 firehose.
    char *p = b;
    while (*p == '0' && *(p + 1) != '\0') p++;
    return p;
}

/**
 * Record the store this session is attached to. Returns 0 on success, or 1 if
 * the path will not fit, setting errno to ENAMETOOLONG.
 *
 * Every write to thisuser.store goes through here. Truncating a store path is
 * never safe: what is left is usually still a valid path, just a different
 * one, so a shortened path reads and writes someone else's store -- or creates
 * a brand new one in the wrong directory -- with nothing to indicate anything
 * went wrong. Refusing is the only correct behavior.
 */
int cli_set_store(const char *path) {
    size_t len;

    if (! path) {
        errno = EINVAL;
        return 1;
    }

    len = strlen(path);
    if (len >= sizeof(thisuser.store)) {
        fprintf(stderr, "splinterctl: store path too long (%zu bytes, max %zu)\n",
            len, sizeof(thisuser.store) - 1);
        errno = ENAMETOOLONG;
        return 1;
    }

    // The length check above is what makes this safe and always terminated;
    // strncpy(dst, src, sizeof(dst) - 1) would leave the final byte untouched.
    memcpy(thisuser.store, path, len + 1);

    return 0;
}

/**
 * Returns the index of the module in the modules array given its
 * name, or -1 if not found.
 */
int cli_find_module(const char *name) {
    unsigned int i;
    
    for (i = 0; command_modules[i].name != NULL; i++) {
        if (! strncmp(name, command_modules[i].name, command_modules[i].name_len))
            return command_modules[i].id;
    }

    errno = EINVAL;

    return -1;
}

/**
 * See if the module at index idx is an alias by returning the index
 * of the module its aliased to, or -1 if not an alias. 
 */
int cli_find_alias(int idx) {
    if (! command_modules[idx].name) {
        errno = EINVAL;
        return -1;
    }
    return command_modules[idx].alias_of;
}

/**
 * Run the module at the specified index by its defined entry point and 
 * proxy its return value. Sets errno if idx is invalid.
 */
int cli_run_module(int idx, int argc, char *argv[]) {
    if (! command_modules[idx].name || ! command_modules[idx].entry) {
        errno = EINVAL;
        return -1;
    }
    return command_modules[idx].entry(argc, argv);
}

/**
 * Show a modules help (to the specified level) by index.
 */
void cli_show_module_help(int idx, unsigned int level) {
    if (! command_modules[idx].name || ! command_modules[idx].help) {
        errno = EINVAL;
        return;
    }
    command_modules[idx].help(level);
    return;
}

/**
 * A simple way to list modules
 */
#define LIST_BAR "--------------------"
void cli_show_modules(void) {
    int i;

    fprintf(stdout, "\n %-10s | %-60s\n%s%s%s%s\n",
        "Module", 
        "Description",
        LIST_BAR,
        LIST_BAR,
        LIST_BAR,
        LIST_BAR);

    for (i = 0; command_modules[i].name; i++) {
        fprintf(stdout, " %-10s | %-60s\n",
            command_modules[i].name,
            command_modules[i].description);
    }
    
    return;
}

/**
 * See if a key is eligible to be printed out to the console without
 * being serialized (and without creating a problem)
 */
unsigned int cli_key_is_printable_unserialized(unsigned short flags) {
    switch(flags) {
        case SPL_SLOT_TYPE_BIGINT:
        case SPL_SLOT_TYPE_BIGUINT:
        case SPL_SLOT_TYPE_VARTEXT:
            return 1;
        default:
            return  0;
    }
}

/**
 * Take a given flag, and return a statically-allocated string representing
 * its symbol.
 */
char * cli_show_key_type(unsigned short flags) {
    if ((flags & SPL_SLOT_TYPE_BIGINT)  != 0) return "SPL_SLOT_TYPE_BIGINT";
    if ((flags & SPL_SLOT_TYPE_BIGUINT) != 0) return "SPL_SLOT_TYPE_BIGUINT";
    if ((flags & SPL_SLOT_TYPE_BINARY)  != 0) return "SPL_SLOT_TYPE_BINARY";
    if ((flags & SPL_SLOT_TYPE_IMGDATA) != 0) return "SPL_SLOT_TYPE_IMGDATA";
    if ((flags & SPL_SLOT_TYPE_VARTEXT) != 0) return "SPL_SLOT_TYPE_VARTEXT";
    if ((flags & SPL_SLOT_TYPE_AUDIO)   != 0) return "SPL_SLOT_TYPE_AUDIO";
    if ((flags & SPL_SLOT_TYPE_JSON)    != 0) return "SPL_SLOT_TYPE_JSON";
    if ((flags & SPL_SLOT_TYPE_VOID)    != 0) return "SPL_SLOT_TYPE_VOID";

    return "UNNAMED";
}

/**
 * Take a string representing a type bitmask, and turn it into the appropriate
 * bitmask.
 */
uint16_t cli_type_to_bitmask(const char *type) {
    if (!strncmp(type, "bigint", 6)) return SPL_SLOT_TYPE_BIGINT;
    if (!strncmp(type, "biguint", 7)) return SPL_SLOT_TYPE_BIGUINT; 
    if (!strncmp(type, "binary", 6)) return SPL_SLOT_TYPE_BINARY;
    if (!strncmp(type, "imgdata", 7)) return SPL_SLOT_TYPE_IMGDATA;
    if (!strncmp(type, "vartext", 7)) return SPL_SLOT_TYPE_VARTEXT;
    if (!strncmp(type, "audio", 5)) return SPL_SLOT_TYPE_AUDIO;
    if (!strncmp(type, "json", 4)) return SPL_SLOT_TYPE_JSON;
    if (!strncmp(type, "void", 4)) return SPL_SLOT_TYPE_VOID;

    return 0; // not found
}

#ifdef HAVE_EMBEDDINGS
// Helper to check if the vector is actually populated
static double calculate_magnitude(const float *vec, size_t len) {
    double sum = 0.0;
    for (size_t i = 0; i < len; i++) {
        sum += vec[i] * vec[i];
    }
    return sqrt(sum);
}

// Simple XOR-sum to see if values shifted without printing all 768
static uint32_t calculate_vec_checksum(const float *vec, size_t len) {
    uint32_t check = 0;
    for (size_t i = 0; i < len; i++) {
        union { float f; uint32_t u; } cast = { .f = vec[i] };
        check ^= cast.u;
    }
    return check;
}
#endif // HAVE_EMBEDDINGS

/**
 * Dump a key's config
 */
void cli_show_key_config(const char *key, const char *caller) {
    splinter_slot_snapshot_t snap = { 0 };

    splinter_get_slot_snapshot(key, &snap);
    if (snap.epoch == 0) {
        fprintf(stderr, "%s: invalid key: %s\n", caller == NULL ? "Splinter; " : caller, key);
        return;
    }

    printf("hash:       %lu\n", snap.hash);
    printf("epoch:      %lu\n", snap.epoch);
    printf("bloom:      %lu (0b%s)\n", snap.bloom, fmt_binary(snap.bloom));
    printf("val_off:    %u\n", snap.val_off);
    printf("val_len:    %u\n", snap.val_len);
    printf("ctime:      %lu\n", snap.ctime);
    printf("atime:      %lu\n", snap.atime);
    printf("type:       %s\n", cli_show_key_type(snap.type_flag));
    printf("key:        %s\n", snap.key);

#ifdef HAVE_EMBEDDINGS
    double mag = calculate_magnitude(snap.embedding, SPLINTER_EMBED_DIM);
    uint32_t vcheck = calculate_vec_checksum(snap.embedding, SPLINTER_EMBED_DIM);
    
    printf("embed:      DIM=%d, Mag=%.4f, Checksum=0x%08x\n", 
           SPLINTER_EMBED_DIM, mag, vcheck);
    
    // Show the first few elements for easy visual update comparison
    // (not always guaranteed to be helpful but it is what it is)
    printf("vec[0..2]:  [%.3f, %.3f, %.3f, ...]\n", 
           snap.embedding[0], snap.embedding[1], snap.embedding[2]);
#endif

    puts("");
    return;
}

/**
 * Don't let atoi() ruin our day without having to call strtol each time.
 */
int cli_safer_atoi(const char *string) {

    char *buff;
    long l;

    l = strtol(string, &buff, 10);
    if (l <= INT_MAX) {
        return (int) l;
    } else {
        fprintf(stderr, "Value or argument would overflow an integer. Exiting.\n");
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Parses ~/.splinterrc to populate the label map in thisuser.
 * Format: LABEL_NAME 0xMASK (e.g., ANGRY 0x1)
 */
void cli_load_config(const char *rcpath) {
    char path[PATH_MAX];

    if (!rcpath || rcpath[0] == '\0') {
        char *home = getenv("HOME");
        if (!home) return;
        snprintf(path, sizeof(path), "%s/.splinterrc", home);
    } else {
        snprintf(path, sizeof(path), "%s", rcpath);
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (rcpath && rcpath[0] != '\0')
            fprintf(stderr, "splinterctl: cannot open rc file '%s': %s\n", path, strerror(errno));
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), fp) && thisuser.label_count < 63) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        char name[32], mask_str[22];
        if (sscanf(line, "%31s %21s", name, mask_str) == 2) {
            char *end;
            errno = 0;
            uint64_t mask = strtoull(mask_str, &end, 0);
            if (errno != 0 || end == mask_str) continue;
            memcpy(thisuser.labels[thisuser.label_count].name, name, 32);
            thisuser.labels[thisuser.label_count].mask = mask;
            thisuser.label_count++;
        }
    }
    fclose(fp);
}
