/**
* Copyright 2025 Tim Post
* License: Apache 2 (MIT available upon request to timthepost@protonmail.com)
*
* @file splinter_cli_cmd_init.c
* @brief Implements the CLI 'init' command.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdalign.h>
#include "config.h"
#include "splinter.h"
#include "splinter_cli.h"
#include "argparse.h"

static const char *modname = "init";

static const char *const usages[] = {
    "init [options] [[--] args]",
    "init [options]",
    NULL,
};

void help_cmd_init(unsigned int level)
{

    (void)level;
    printf("Usage: %s [store_name] [--slots num_slots] [--maxlen max_val_len]\n", modname);
    printf("%s creates a Splinter store to default or specific geometry.\n", modname);
    puts("If arguments are omitted, these compiled-in defaults are used:");
    printf("\nname: %s\nslots: %lu\nmaxlen: %lu\nalignment: %zu\n",
           DEFAULT_BUS,
           (unsigned long)DEFAULT_SLOTS,
           (unsigned long)DEFAULT_VAL_MAXLEN,
           alignof(struct splinter_slot)
    );
    return;
}

int cmd_init(int argc, char *argv[])
{
    char save[PATH_MAX] = {0}, store[PATH_MAX] = {0};
    int rc = 0;
    unsigned int prev_conn = 0;
    unsigned long max_slots = DEFAULT_SLOTS, max_val = DEFAULT_VAL_MAXLEN;

    if (thisuser.store_conn) {
        // Always fits: thisuser.store is the same size and cli_set_store()
        // has already length-checked whatever is in it.
        snprintf(save, sizeof(save), "%s", thisuser.store);
        prev_conn = 1;
    }

    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_INTEGER('s', "slots", &max_slots, "Maximum Slots", NULL, 0, 0),
        OPT_INTEGER('l', "length", &max_val, "Maximum Value Length", NULL, 0, 0),
        OPT_END(),
    };

    struct argparse argparse;
    argparse_init(&argparse, options, usages, 0);
    argparse_describe(&argparse, "\nInitialize a store", "\nCreates a new store to the specified geometry.");
    argc = argparse_parse(&argparse, argc, (const char **)argv);

    if (argc != 0) {
        // Refuse rather than truncate. A truncated store path is usually still
        // a creatable path, so this used to hand splinter_create() a shortened
        // name and report success -- creating the store in the wrong place,
        // silently, while the caller found nothing where it asked for it.
        size_t len = strlen(argv[argc-1]);

        if (len >= sizeof(store)) {
            fprintf(stderr, "%s: store path too long (%zu bytes, max %zu)\n",
                modname, len, sizeof(store) - 1);
            errno = ENAMETOOLONG;
            return 1;
        }
        snprintf(store, sizeof(store), "%s", argv[argc-1]);
    }

    if (!store[0])
        snprintf(store, sizeof(store), "%s", DEFAULT_BUS);

    size_t slot_sz = sizeof(struct splinter_slot);
    size_t arena_sz = max_slots * max_val;
    size_t total_est = sizeof(struct splinter_header) + (max_slots * slot_sz) + arena_sz;

    printf("Initializing store: %s\n", store);
    printf(" - Slots: %lu (%zu bytes each, %zu byte alignment)\n",
           max_slots,
           max_val,
           alignof(struct splinter_slot));
    printf(" - Value Arena: %lu bytes, SRS: %zu bytes (~%.2f MB)\n",
           arena_sz,
           total_est,
           (double)total_est / 1048576.0);
    rc = splinter_create(store, max_slots, max_val);

    if (rc < 0)
        perror("splinter_create");

    splinter_close();
    goto restore_conn;

restore_conn:
    if (prev_conn) {
        rc = splinter_open(save);
        if (rc != 0)
        {
            perror("splinter_open");
            fprintf(stderr,
                    "warning: could not re-attach to %s, did something else remove it?",
                    save);
            thisuser.store_conn = 0;
            return rc;
        } else {
            // Cannot fail: `save` was copied out of thisuser.store, which is
            // the same size and already length-checked.
            (void) cli_set_store(save);
            thisuser.store_conn = 1;
        }
    }
    return rc;
}
