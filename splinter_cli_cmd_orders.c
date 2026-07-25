/**
 * Copyright 2025 Tim Post
 * License: Apache 2 (MIT available upon request to timthepost@protonmail.com)
 *
 * @file splinter_cli_cmd_orders.c
 * @brief Implements the CLI 'orders' command.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "splinter_cli.h"

static const char *modname = "orders";

void help_cmd_orders(unsigned int level) {
    (void) level;
    printf("%s manages standard orders of a key for coupled vector storage.\n", modname);
    printf("Usage: %s <set|unset> <key> <count>\n", modname);
    return;
}

int cmd_orders(int argc, char *argv[]) {
    if (argc < 4) {
        help_cmd_orders(1);
        return 1;
    }

    const char *mode = argv[1];
    const char *key = argv[2];
    uint8_t count = (uint8_t)atoi(argv[3]);
    int rc = 0;

    if (strcmp(mode, "set") == 0) {
        char val_buf[1024];
        const void *vals[count];
        size_t lens[count];

        for (int i = 0; i < count; i++) {
            snprintf(val_buf, sizeof(val_buf), "%s_%d", SPL_ORDER_ACCESSOR, i);
            vals[i] = strdup(val_buf);
            lens[i] = strlen(val_buf);
        }

        int ret = splinter_client_set_tandem(key, vals, lens, count);
        printf("Tandem set for %s with %d orders: %s\n", key, count, ret == 0 ? "OK" : "FAIL");

        /* The printed word and the exit code must never disagree: a caller that
         * trusts $? alone would read a FAIL (no free slots, for instance) as a
         * success and carry on writing into a tandem that was never set. */
        if (ret != 0)
            rc = 1;

        for (int i = 0; i < count; i++) {
            if (vals[i] && strlen(vals[i])) {
                free((void *)vals[i]);
            }
        }
    } 
    else if (strcmp(mode, "unset") == 0) {
        splinter_client_unset_tandem(key, count);
        printf("Tandem unset for %s (%d orders) executed.\n", key, count);
    } else {
        fprintf(stderr,"Unsupported mode: %s ('set' or 'unset' are supported)\n", mode);
        return 1;
    }

    return rc;
}
