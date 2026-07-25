/**
 * Copyright 2025 Tim Post
 * License: Apache 2 (MIT available upon request to timthepost@protonmail.com)
 *
 * @file splinter_cli_main.c
 * @brief Splinter CLI main entry / completion & hint setup / parsing / executing
 */

 #ifndef _GNU_SOURCE
// for getopt extensions
#define _GNU_SOURCE 
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <getopt.h>
#include <errno.h>

#include "splinter_cli.h"
#include "config.h"
#include "linenoise.h"

/**
 * We deviate from linenoise default here (which hides these)
 * but we need direct access to the history array for non-repl
 * cleanup on exit (non-repl gets logged in history, too, but 
 * different handlers when not getting direct input). 
 */
extern char **history;
extern void freeHistory(void);

enum mode {
    MODE_REPL,
    MODE_NO_REPL
};

// whether we run one-shot or in REPL
static enum mode m = MODE_REPL;

// holds all of the commands
cli_module_t command_modules[] = {
    {
        0,
        "clear",
        5,
        "Clears the screen.",
        -1,
        &cmd_clear,
        &help_cmd_clear
    },
    {
        1,
        "cls",
        3,
        "Alias of 'clear'",
        0,
        &cmd_clear,
        &help_cmd_clear
    },
    {
        2,
        "config",
        6,
        "Access Splinter bus and slot metadata.",
        -1,
        &cmd_config,
        &help_cmd_config
    },
    {
        3,
        "get",
        3,
        "Retrieve the value of a key in the store.",
        -1,
        &cmd_get,
        &help_cmd_get
    },
    {
        4,
        "head",
        4,
        "Retrieve just the metadata of a key in the store.",
        -1,
        &cmd_head,
        &help_cmd_head
    },
    {
        5,
        "help",
        4,
        "Help with commands and features.",
        -1,
        &cmd_help,
	    &help_cmd_help
    },
    {
        6,
        "hist",
        4,
        "View and clear command history.",
        -1,
        &cmd_hist,
        &help_cmd_hist
    },
    {
        7,
        "list",
        4,
        "List keys in the current store.",
        -1,
        &cmd_list, 
        &help_cmd_list
    },
    {
        8,
        "set",
        3,
        "Set a key in the store to a specified value.",
        -1,
        &cmd_set,
        &help_cmd_set
    },
    {
        9,
        "unset",
        5,
        "Unset a key in the store (deletes the key).",
        -1,
        &cmd_unset,
        &help_cmd_unset
    },
    {
        10,
        "use",
        3,
        "Opens a Splinter store by name or path.",
        -1,
        &cmd_use,
        &help_cmd_use
    },
    {
        11,
        "watch",
        5,
        "Observes a key for changes and prints updated contents.",
        -1,
        &cmd_watch,
        &help_cmd_watch
    },
    {
        12,
        "init",
        4,
        "Initialize Splinter stores.",
        -1,
        &cmd_init,
        &help_cmd_init
    },
    {
        13,
        "export",
        6,
        "Export Splinter data in various formats.",
        -1,
        &cmd_export,
        &help_cmd_export
    },
    {
        14,
        "type",
        4,
        "Display or set the named slot type for a key.",
        -1,
        &cmd_type, 
        &help_cmd_type
    },
    {
        15,
        "math",
        4,
        "Perform incr/decr and bitwise ops on named biguint keys.",
        -1,
        &cmd_math,
        &help_cmd_math
    },
    {
        16,
        "label",
        5,
        "Label a key with bits for its bloom filter",
        -1,
        &cmd_label,
        &help_cmd_label
    },
    {
        17,
        "orders",
        6,
        "Manage standard vector orders of a key",
        -1,
        &cmd_orders,
        &help_cmd_orders
    },
    {
        18,
        "bind",
        4,
        "Bind bloom labels to signal arena groups",
        -1,
        &cmd_bind, 
        &help_cmd_bind
    },
    {
        19,
        "bump",
        4,
        "Bump a slot's epoch without doing any other work",
        -1,
        &cmd_bump,
        &help_cmd_bump
    },
    {
        20, 
        "append",
        6,
        "Append a value to an existing key (do not truncate prior to writing)",
        -1,
        &cmd_append,
        &help_cmd_append
    },
    {
        21,
        "uuid",
        4,
        "Generate a UUID v4 unique identifier",
        -1,
        &cmd_uuid,
        &help_cmd_uuid
    },
    {
        22,
        "caps",
        4,
        "Print version, build, and compiled-in feature flags",
        -1,
        &cmd_caps,
        &help_cmd_caps
    },
    {
        23,
        "shard",
        5,
        "Inspect/seed the Logic Shard bid table (cooperative memory advisement)",
        -1,
        &cmd_shard,
        &help_cmd_shard
    },
    {
        24,
        "retrain",
        7,
        "Zero a key's vectors and flip its epoch back to 4 to republish it",
        -1,
        &cmd_retrain,
        &help_cmd_retrain
    },
#ifdef HAVE_EMBEDDINGS
    {
        25,
        "search",
        6,
        "Search embedded keys by semantic similarity and distance",
        -1,
        &cmd_search,
        &help_cmd_search
    },
    {
        26,
        "ingest",
        6,
        "Ingest a file or stdin as chunked tandem slots for splinference",
        -1,
        &cmd_ingest,
        &help_cmd_ingest
    },
#endif
#ifdef HAVE_WASM
    {
#ifdef HAVE_EMBEDDINGS
        27,
#else
        25,
#endif
        "wasm",
        4,
        "Run WASM via WASMEdge",
        -1,
        &cmd_wasm,
        &help_cmd_wasm
    },
#endif // HAVE_WASM
#ifdef HAVE_LUA
    {
        /* id == array index: 25 base (incl. retrain) + 2 if embeddings (search,ingest) + 1 if wasm */
#if defined(HAVE_EMBEDDINGS) && defined(HAVE_WASM)
        28,
#elif defined(HAVE_EMBEDDINGS)
        27,
#elif defined(HAVE_WASM)
        26,
#else
        25,
#endif
        "lua",
        3,
        "Run a lua script",
        -1,
        &cmd_lua,
        &help_cmd_lua
    },
#endif // HAVE_LUA
// The last null-filled element 
    { 0, NULL, 0, NULL, -1,  NULL , NULL }
};

// Initialize a shared session structure
// (declared as extern in splinter_cli.h)
cli_user_t thisuser = {
    // char [] storing the name of the connected store
    { 0 },
    // is the user connected?
    false,
    // has the user requested abort?
    0,
    // termios structure for the user
    { 0 },
    // exit status of the last command
    0,
    // errno after the last command
    0,
    // map of bloom labels 
    {},
    // how many bloom labels they have 
    0
};

// We raise thisuser.abort on SIGUSER1 (so far)
static void cli_handle_signal(int signum) {
    switch (signum) {
        // This arrangement allows using them as either an emergency stop, 
        // or stop / start button. 
        case SIGUSR1:
            thisuser.abort = 1;
            break;
        case SIGUSR2:
            thisuser.abort = 0;
            break;
        // don't interfere with linenoise
        default:
            break;
    }
}

static int cli_set_signal_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_handler = cli_handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    int rc = 0;

    rc += sigaction(SIGUSR1, &sa, NULL);
    rc += sigaction(SIGUSR2, &sa, NULL);

    return rc;
}

// Safely set mode from invoked name
// See POSIX exec family standards regarding argv[0] not being guaranteed,
// so we're extremely defensive about allowing the 'splinterctl' shortcut 
// to the --no-repl / --non-interactive modes.  
enum mode select_mode(const char *argv0) {
    char tmp[256];
    const char *prog = NULL;

    if (!argv0) return MODE_REPL;
    
    strncpy(tmp, argv0, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    prog = basename(tmp);

    if (strncmp(prog, "splinterctl", 11) == 0 || strncmp(prog, "splinterpctl", 12) == 0) {
        return MODE_NO_REPL;
    } else if (strncmp(prog, "splinter_cli", 13) == 0 || strncmp(prog, "splinterp_cli", 14) == 0) {
        return MODE_REPL;
    }

    // some kind of app armor messed with argv[0]?
    // Just default to the REPL.
    return MODE_REPL;
}

// cmake shim
#ifndef SPLINTER_VERSION
#define SPLINTER_VERSION 0XDEADBEEF
#endif

void print_version_info(char *progname) {
    fprintf(stderr,  "%s version %s build %s\n",
        progname, 
        SPLINTER_VERSION, 
        SPLINTER_BUILD);
    return;
}

void print_usage(char *progname) {
    fprintf(stderr, 
        "Usage:  %s [options] [arguments] *or*\n\t%s --no-repl <built in command> [arguments] *or*\n\t%s {no args for REPL}\n",
        progname, 
        progname,
        progname
    );
    fprintf(stderr, "%s provides a command line interface for Splinter bus interaction.\n", progname);
    fprintf(stderr, "Where [options] are:\n");
    fprintf(stderr, "  --help / -h                  Show this help display.\n");
    fprintf(stderr, "  --history-file / -H <path>   Set the CLI history file to <path>\n");
    fprintf(stderr, "  --history-len / -l  <len>    Set the CLI history length to <len>\n");
    fprintf(stderr, "  --list-modules / -L          List available commands.\n");
    fprintf(stderr, "  --no-repl / -n               Don't enter interactive mode.\n");
    fprintf(stderr, "  --prefix / -p <prefix>       Prepend <prefix> to read/write ops (namespace)\n");
    fprintf(stderr, "  --rc-file / -r <file>        Use a specific .splinterrc file.\n");
    fprintf(stderr, "  --use / -u <store>           Connect to <store> after starting.\n");
    fprintf(stderr, "  --version / -v               Print splinter version information and exit.\n");
    fprintf(stderr, "\n%s will look for SPLINTER_HISTORY_FILE and SPLINTER_HISTORY_LEN in the\n", progname);
    fprintf(stderr, "environment and use them. However, argument values will always take precedence.\n");
    fprintf(stderr, "\nIf invoked as \"splinterctl\", %s automatically turns on --no-repl.\n", progname);
    fprintf(stderr, "\nPrefixes (via --prefix) can contain any printing character but '='. This uses\n");
    fprintf(stderr, "the SPLINTER_NS_PREFIX environmental variable.\n");
    fprintf(stderr, "\nPlease report bugs at https://github.com/timthepost/libsplinter, or to the author");
    fprintf(stderr, "at <timthepost@protonmail.com>.");

    return;
} 

// fires whenever the user presses tab for tab / auto completion
static void completion(const char *buf, linenoiseCompletions *lc) {
    if (buf[0] == '\0') return;

    switch (buf[0]) {
        case 'a':
            linenoiseAddCompletion(lc, "append");
            break;
        case 'b':
            linenoiseAddCompletion(lc, "bind");
            linenoiseAddCompletion(lc, "bump");
            break;
        case 'c':
            linenoiseAddCompletion(lc, "caps");
            linenoiseAddCompletion(lc, "clear");
            linenoiseAddCompletion(lc, "config");
            break;
        case 'e':
            linenoiseAddCompletion(lc, "export");
            break;
        case 'g':
            linenoiseAddCompletion(lc, "get");
            break;
        case 'h':
            linenoiseAddCompletion(lc, "help");
            linenoiseAddCompletion(lc, "head");
            linenoiseAddCompletion(lc, "hist");
            break;
        case 'i':
            linenoiseAddCompletion(lc, "init");
#ifdef HAVE_EMBEDDINGS
            linenoiseAddCompletion(lc, "ingest");
#endif
            break;
        case 'l':
            linenoiseAddCompletion(lc, "list");
            linenoiseAddCompletion(lc, "label");
#ifdef HAVE_LUA
            linenoiseAddCompletion(lc, "lua");
#endif // HAVE_LUA        
            break;
        case 'm':
            linenoiseAddCompletion(lc, "math");
            break;
        case 'o':
            linenoiseAddCompletion(lc, "orders");
            break;
        case 'r':
            linenoiseAddCompletion(lc, "retrain");
            break;
        case 's':
            linenoiseAddCompletion(lc, "set");
#ifdef HAVE_EMBEDDINGS
            linenoiseAddCompletion(lc, "search");
#endif // HAVE_EMBEDDINGS
            break;
        case 't':
            linenoiseAddCompletion(lc, "type");
            break;
        case 'u':
            linenoiseAddCompletion(lc, "use");
            linenoiseAddCompletion(lc, "unset");
            linenoiseAddCompletion(lc, "uuid");
            break;
        case 'w':
            linenoiseAddCompletion(lc, "watch");
#ifdef HAVE_WASM
            linenoiseAddCompletion(lc, "wasm");
#endif // HAVE_WASM
            break;
        default:
            break;
    }

    return;
}

// callback that provides completion hints while typing
// TODO re-think how this works; it's onerous to maintain and 
// kinda clunky.
static char *hints(const char *buf, int *color, int *bold) { 
    /* foreground colors you can use:
     * red = 31
     * green = 32
     * yellow = 33
     * blue = 34
     * magenta = 35
     * cyan = 36
     * white = 37;
     */
    
    // the most frequent combo, outliers can be different
    // if they need to
    *color = 36;
    *bold = 1;

    if(!strncasecmp(buf, "a", 6)) {
        return "ppend ";
    }
    
    if(!strncasecmp(buf, "b", 4)) {
        return "ind ";
    }

    if(!strncasecmp(buf, "bu", 4)) {
        return "mp ";
    }

    if (!strncasecmp(buf, "ca", 4)) {
        return "ps";
    }

    if (!strncasecmp(buf, "cl", 3)) {
        *color = 35;
        *bold = 1;
        return "s";
    }

    if (!strncasecmp(buf, "cle", 5)) {
        return "ar";
    }

    if (!strncasecmp(buf, "con", 6)) {
        return "fig ";
    }

    if (!strncasecmp(buf, "ex", 6)) {
        return "port";
    }
    
    if (!strncasecmp(buf, "g", 3)) {
        return "et ";
    }

    if (!strncasecmp(buf, "hi", 4)) {
        return "st ";
    }

    if (!strncasecmp(buf, "hea", 4)) {
        return "d ";
    }

    if (!strncasecmp(buf, "hel", 4)) {
        return "p ";
    }

#ifdef HAVE_EMBEDDINGS
    if (!strncasecmp(buf, "ing", 6)) {
        return "est ";
    }
#endif

    if (!strncasecmp(buf, "i", 4)) {
        return "nit ";
    }

    if (!strncasecmp(buf, "li", 4)) {
        return "st ";
    }

    if (!strncasecmp(buf, "la", 5)) {
        return "bel ";
    }

#ifdef HAVE_LUA
    if (!strncasecmp(buf, "lu", 3)) {
        return "a ";
    }
#endif // HAVE_LUA

    if (!strncasecmp(buf, "m", 4)) {
        return "ath ";
    }

    if (!strncmp(buf, "or", 6)) {
        return "ders ";
    }

    if (!strncasecmp(buf, "r", 7)) {
        return "etrain ";
    }
#ifdef HAVE_EMBEDDINGS
    if (!strncasecmp(buf, "sea", 6)) {
        return "rch ";
    }
#endif // HAVE_EMBEDDINGS
    if (!strncasecmp(buf, "se", 4)) {
        return "t ";
    }

    if (!strncasecmp(buf, "s", 3)) {
        return "et ";
    }

    // 'time' may be a command soon, leaving room for it.
    
    if (!strncmp(buf, "ty", 4)) {
        return "pe ";
    }

    if (!strncasecmp(buf, "un", 5)) {
        return "set ";
    }

    if (!strncasecmp(buf, "uu", 4)) {
        return "id ";
    }

#ifdef HAVE_WASM
    if (!strncasecmp(buf, "was", 4)) {
        return "m ";
    }
#endif // HAVE_WASM

    if (!strncasecmp(buf, "wat", 5)) {
        return "ch ";
    }

    return NULL;
}

static const struct option long_options[] = {
    { "help", optional_argument, NULL, 'h' },
    { "history-file", required_argument, NULL, 'H' },
    { "history-len", required_argument, NULL, 'l' },
    { "list-modules", no_argument, NULL, 'L' },
    { "no-repl", no_argument, NULL, 'n' },
    { "rc-file", required_argument, NULL, 'r' },
    { "prefix", required_argument, NULL, 'p' },
    { "use", required_argument, NULL, 'u' },
    { "version", no_argument, NULL, 'v' },
    {NULL, 0, NULL, 0}
};

static const char *optstring = "+h::H:l:Lnp:r:u:v";

static void cli_at_exit(void) {
    if (thisuser.store_conn)
        splinter_close();
    if (m == MODE_REPL)
        // restore from possibly being set in non-blocking mode
        tcsetattr(STDIN_FILENO, TCSANOW, &thisuser.term);
}

int main (int argc, char *argv[]) {
    int rc = 0, _argc = 0, idx = -1, opt, historylen = -1, prefix_len = 0;
    char *progname = basename(argv[0]), *buff = NULL, *ptmp = NULL;
    char prompt[128] = { 0 };
    char **mod_args = { 0 };
    char rc_path[PATH_MAX] = { 0 };

    // These can also be set via command line. 
    char *historyfile = getenv("SPLINTER_HISTORY_FILE");
    const char *tmp = getenv("SPLINTER_HISTORY_LEN");

    // We set history length initially from env if appropriate
    if (tmp != NULL) historylen = cli_safer_atoi(tmp);

    if (historylen >= 0) linenoiseHistorySetMaxLen(historylen);
    if (historyfile != NULL && historylen > 0) linenoiseHistoryLoad(historyfile);

    // If you absolutely need to disable the implicit mode check based on invocation, 
    // comment out m = select_mode() and allow arguments to decide it exclusively.
    m = select_mode(progname);
    atexit(cli_at_exit);
    
    // Modules must know what this is in order to be able to restore it
    tcgetattr(STDIN_FILENO, &thisuser.term);

    opterr = 0;
    optind = 0;

    while ((opt = getopt_long(argc, argv, optstring, long_options, NULL)) != -1) {
        switch (opt) {
            // --help / -h
            case 'h':
                print_usage(progname);
                exit(EXIT_SUCCESS);
            // --history-file / -H
            case 'H':
                linenoiseHistoryLoad(optarg);
                break;
            // --history-len / -l
            case 'l':
                linenoiseHistorySetMaxLen(cli_safer_atoi(optarg));
                break;
            // --list-modules / -L
            case 'L':
                print_version_info(progname);
                cli_show_modules();
                exit(EXIT_SUCCESS);
            // --no-repl / -n
            case 'n':
                m = MODE_NO_REPL;
                break;
            // --prefix = ... 
            case 'p':
                if (setenv("SPLINTER_NS_PREFIX", optarg, 1) != 0) {
                    perror("setenv");
                    exit(EXIT_FAILURE);
                }
                break;
            // --rc-file / -r
            case 'r':
                snprintf(rc_path, sizeof(rc_path) - 1, "%s", optarg);
                break;
            // --use / -u
            case 'u':
                if (splinter_open(optarg) == 0) {
                    if (cli_set_store(optarg) != 0) {
                        splinter_close();
                        fprintf(stderr, "%s: will start disconnected.\n", progname);
                        break;
                    }
                    thisuser.store_conn = 1;
                } else {
                    fprintf(stderr, "%s: could not connect to '%s'; will start disconnected.\n",
                    progname, optarg);
                }
                break;
            // --version / -v
            case 'v':
                print_version_info(progname);
                exit(EXIT_SUCCESS);
            // argument requires option
            case ':':
            case '?':
                fprintf(stderr, "%s: option '-%c' requires an argument. Try %s --help for help.\n", 
                    progname, 
                    optopt, 
                    progname);
                exit(EXIT_FAILURE);
            // If this case is entered, it means getopt_long() is likely broken?
            default:
                fprintf(stderr, "%s: unknown error parsing argument '%c'. Try %s --help for help.\n", 
                    progname,
                    optopt, 
                    progname);
                exit(EXIT_FAILURE);
        }
    }

    // we have no active store
    if (thisuser.store_conn == 0) {
        ptmp = getenv("SPLINTER_DEFAULT_STORE");
        // okay, there's one actually specified - try it.        
        if (ptmp) {
            if (splinter_open(ptmp) == 0) {
                if (cli_set_store(ptmp) != 0) {
                    splinter_close();
                    fprintf(stderr, "%s: will start disconnected.\n", progname);
                } else {
                    thisuser.store_conn = 1;
                }
            } else {
                fprintf(stderr, "%s: could not connect to '%s'; will start disconnected.\n",
                progname, ptmp);
            }
            ptmp = NULL;
        }
    }

    // anticipate someone trying <uuid>::<uuid>::<uuid>::__SomethingLikeThis:: as a prefix.
    // perhaps not intentionally, but warn if it's getting close, or keys could be truncated
    ptmp = getenv("SPLINTER_NS_PREFIX");
    if (ptmp) {
        prefix_len = strnlen(ptmp, SPLINTER_KEY_MAX - 1);
        if (prefix_len && prefix_len >= (SPLINTER_KEY_MAX / 2)) {
            fprintf(stderr, 
                "%s warning! Prefix length of %d bytes is half or more of the field size of %d bytes.\n",
                progname,
                prefix_len,
                SPLINTER_KEY_MAX);
            fprintf(stderr, "Adjust the value of SPLINTER_KEY_MAX and recompile if this really can't be helped.\n");
            fprintf(stderr,"Keys will likely be truncated unless the value is increased, or the prefix length is reduced.\n");
            fflush(stderr);
        }
    }

    // cli_load_config knows if rc_path is null, no check needed here.
    cli_load_config(rc_path);
    
    if (m == MODE_REPL) {
        if (cli_set_signal_handlers() < 0) {
            fprintf(stderr, 
                "%s: failed to register signal handlers. Certain interactive features may malfunction.\n",
                progname);
        }
        
        print_version_info(progname);        
        fprintf(stderr,"To quit, press ctrl-c or ctrl-d.\n");
        
        linenoiseSetCompletionCallback(completion);
        linenoiseSetHintsCallback(hints);
        
        do {
            // The store name is bounded explicitly here: it can now be a full
            // PATH_MAX path, and nobody wants a 4 KB shell prompt. This is the
            // one place truncating it is correct -- it is cosmetic, and the
            // authoritative value is always thisuser.store itself.
            if (thisuser.lasterrno != 0) {
                snprintf(prompt, 128, "%d : %.100s # ",
                    thisuser.lasterrno,
                    thisuser.store[0] == '\0' ? "no-conn" : thisuser.store
                );
            } else {
                snprintf(prompt, 128, "%.100s # ",
                    thisuser.store[0] == '\0' ? "no-conn" : thisuser.store
                );
            }
            // unpack the arguments into an argv[] style array
            mod_args = cli_input_args(prompt, &_argc);
            
            // ctrl-c or ctrl-d
            if (mod_args == NULL) break;

            // user just pressed enter alone
            if (_argc == 0) {
                cli_free_argv(mod_args);
                continue;
            }

            // there actually might be something to do
            idx = cli_find_module(mod_args[0]);
            if (idx >= 0) {
                rc = cli_run_module(idx, _argc, mod_args);
            } else {
                fprintf(stderr, "Unknown command: %s\n", mod_args[0]);
                rc = 1;
            }

            thisuser.lasterrno = rc;

            // re-set for next turn
            cli_free_argv(mod_args);
            mod_args = NULL;
            _argc = 0;

            // End of REPL loop
        } while (1);
    } else {
        if (argc >= optind) { 
            _argc = argc - optind;
            mod_args = cli_slice_args(argv, optind, _argc);

            if (mod_args && mod_args[0]) {
                idx = cli_find_module(mod_args[0]);
                if (idx >= 0) {
                    buff = cli_rejoin_args(mod_args);
                    linenoiseHistoryAdd(buff);
                    free(buff);
                    buff = NULL;
                    rc = cli_run_module(idx, _argc, mod_args);
                } else {
                    fprintf(stderr, "Unknown command: %s\n", mod_args[0]);
                }
            }
            free(mod_args);
            mod_args = NULL;
            _argc = 0;
        } else {
            print_usage(progname);
            rc = 1;
        }
    }

    if (historyfile != NULL && historylen > 0) linenoiseHistorySave(historyfile);
    
    // Since we didn't invoke linenoise in non-repl mode, we won't benefit from its 
    // atexit function. So, we have to call it explicitly in non-repl mode. (this is
    // also a change I made to line noise, history was static (opaque))
    if (m == MODE_NO_REPL) {
        if (history) freeHistory();
    }
    
    return rc;
}
