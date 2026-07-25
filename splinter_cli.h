/**
 * Copyright 2025 Tim Post
 * License: Apache 2 (MIT available upon request to timthepost@protonmail.com)
 *
 * @file splinter_cli.h
 * @brief Splinter CLI global header
 */
#ifndef SPLINTER_CLI_H
#define SPLINTER_CLI_H

#include "splinter.h"
#include <stdbool.h>
#include <signal.h>
#include <termios.h>
#include <limits.h>
#include <linux/limits.h>

// Hard cap on viewable history length
#define CLI_HISTORY_MAX_LEN 1024

// Types for module command entry and help 
typedef int (*mod_entry_t)(int, char *[]);
typedef void (*mod_help_t)(unsigned int);

// A command (module)
typedef struct cli_module {
    // correlates to array position for easy lookup
    int id;
    // name of the command, e.g. set / get / help
    const char *name;
    // computed length of the command to speed up linear probing
    size_t name_len;
    // What it does
    const char *description;
    // Is this just another name for another command? Set this to its id.
    int alias_of;
    // If alias, set these to null (we resolve the root entry points)
    mod_entry_t entry;
    mod_help_t help;
} cli_module_t;

// A utility to map "ctags-style" labels to bitmasks for bloom filtering
typedef struct {
    char name[32];
    uint64_t mask;
} label_map_t;

// A utility class to store user session quirks
typedef struct cli_user {
    // name of the store connected to. Sized to hold any path the kernel will
    // accept: this was 64 bytes, which silently truncated longer store paths
    // into a different (often valid, and therefore silently wrong) path.
    // Only ever written through cli_set_store().
    char store[PATH_MAX];
    // is the user connected to a store?
    bool store_conn;
    // does the user want to abort whatever we're doing?
    volatile sig_atomic_t abort;
    // user's terminal
    struct termios term;
    // exit status of the last run command
    int lastexit;
    // errno after last run command
    int lasterrno;
    // 64 assignable labels from .splinterrc
    label_map_t labels[64];
    // how many are actually there
    int label_count;
} cli_user_t;

// We construct the user in cli_main
extern cli_user_t thisuser;

// Prototypes for runtime functions
char **cli_input_args(const char *prompt, int *argc);
char **cli_unroll_argv(const char *input, int *out_argc);
void cli_free_argv(char *argv[]);
int cli_set_store(const char *path);
int cli_find_module(const char *name);
int cli_find_alias(int idx);
int cli_run_module(int idx, int argc, char *argv[]);
void cli_show_module_help(int idx, unsigned int level);
char **cli_slice_args(char *const src[], int start_index, int count);
char *cli_rejoin_args(char *const src[]);
void cli_show_modules(void);
void cli_show_key_config(const char *key, const char *caller);
int cli_safer_atoi(const char *string);
char * cli_show_key_type(unsigned short flags);
uint16_t cli_type_to_bitmask(const char *type);
unsigned int cli_key_is_printable_unserialized(unsigned short flags);
void cli_load_config(const char *rcpath);

// Prototypes for individual command entry points
int cmd_help(int argc, char *argv[]);
void help_cmd_help(unsigned int level);

int cmd_clear(int argc, char *argv[]);
void help_cmd_clear(unsigned int level);

int cmd_config(int argc, char *argv[]);
void help_cmd_config(unsigned int level);

int cmd_use(int argc, char *argv[]);
void help_cmd_use(unsigned int level);

int cmd_watch(int argc, char *argv[]);
void help_cmd_watch(unsigned int level);

int cmd_unset(int argc, char *argv[]);
void help_cmd_unset(unsigned int level);

int cmd_set(int argc, char *argv[]);
void help_cmd_set(unsigned int level);

int cmd_get(int argc, char *argv[]);
void help_cmd_get(unsigned int level);

int cmd_list(int argc, char *argv[]);
void help_cmd_list(unsigned int level);

int cmd_hist(int argc, char *argv[]);
void help_cmd_hist(unsigned int level);

int cmd_head(int argc, char *argv[]);
void help_cmd_head(unsigned int level);

int cmd_init(int argc, char *argv[]);
void help_cmd_init(unsigned int level);

int cmd_export(int argc, char *argv[]);
void help_cmd_export(unsigned int level);

int cmd_type(int argc, char *argv[]);
void help_cmd_type(unsigned int level);

int cmd_math(int argc, char *argv[]);
void help_cmd_math(unsigned int level);

int cmd_label(int argc, char *argv[]);
void help_cmd_label(unsigned int level);

#ifdef HAVE_LUA
int cmd_lua(int argc, char *argv[]);
void help_cmd_lua(unsigned int level);
#endif // HAVE_LUA

int cmd_orders(int argc, char *argv[]);
void help_cmd_orders(unsigned int level);

int cmd_bind(int argc,  char *argv[]);
void help_cmd_bind(unsigned int level);

int cmd_bump(int argc, char *argv[]);
void help_cmd_bump(unsigned int level);

int cmd_retrain(int argc, char *argv[]);
void help_cmd_retrain(unsigned int level);

int cmd_append(int argc, char *argv[]);
void help_cmd_append(unsigned int level);

int cmd_uuid(int argc, char *argv[]);
void help_cmd_uuid(unsigned int level);

int cmd_caps(int argc, char *argv[]);
void help_cmd_caps(unsigned int level);

int cmd_shard(int argc, char *argv[]);
void help_cmd_shard(unsigned int level);

#ifdef HAVE_EMBEDDINGS
int cmd_search(int argc, char *argv[]);
void help_cmd_search(unsigned int level);

int cmd_ingest(int argc, char *argv[]);
void help_cmd_ingest(unsigned int level);
#endif // HAVE_EMBEDDINGS

#ifdef HAVE_WASM
int cmd_wasm(int argc, char *argv[]);
void help_cmd_wasm(unsigned int level);
#endif // HAVE_WASM

// And finally an array of modules to hold them all
extern cli_module_t command_modules[];

#endif // SPLINTER_CLI_H
