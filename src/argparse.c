// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018 Gabriele N. Tornetta <phoenix1987@gmail.com>.
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#define ARGPARSE_C

#include "platform.h"

#ifdef PL_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "argparse.h"
#include "austin.h"
#include "hints.h"
#include "platform.h"

#define DEFAULT_SAMPLING_INTERVAL 100
#define DEFAULT_INIT_TIMEOUT_MS   3000 // 3 second

// Globals for command line arguments
parsed_args_t pargs = {
    /* t_sampling_interval */ DEFAULT_SAMPLING_INTERVAL,
    /* timeout             */ DEFAULT_INIT_TIMEOUT_MS * 1000,
    /* attach_pid          */ 0,
    /* cmd                 */ NULL,
    /* where               */ 0,
    /* cpu                 */ 0,
    /* full                */ 0,
    /* memory              */ 0,
    /* output_file         */ NULL,
    /* output_filename     */ NULL,
    /* children            */ 0,
    /* exposure            */ 0,
    /* pipe                */ 0,
    /* gc                  */ 0,
/* native              */
#ifdef AUSTINP
    true, // native is always on for austinp
#else
    false,
#endif
#ifdef AUSTINP
    /* kernel              */ 0,
#endif
};

// ---- PRIVATE ---------------------------------------------------------------

// ----------------------------------------------------------------------------
static int
str_to_num(char* str, long* num) {
    char* p_err;

    *num = strtol(str, &p_err, 10);

    return (p_err == str || *p_err != '\0') ? 1 : 0;
}

/**
 * Parse the interval argument.
 *
 * This accepts s, ms and us as units. The result is in microseconds.
 */
static int
parse_interval(char* str, long* num) {
    char* p_err;

    *num = strtol(str, &p_err, 10);

    if (p_err == str)
        FAIL;

    switch (*p_err) {
    case '\0':
        SUCCESS;
    case 's':
        if (*(p_err + 1) != '\0')
            FAIL;
        *num = *num * 1000000;
        break;
    case 'm':
        if (*(p_err + 1) != 's' || *(p_err + 2) != '\0')
            FAIL;
        *num = *num * 1000;
    case 'u':
        if (*(p_err + 1) != 's' || *(p_err + 2) != '\0')
            FAIL;
        break;
    default:
        FAIL;
    }

    SUCCESS;
}

/**
 * Parse the timeout argument.
 *
 * This accepts s and ms as units. The result is in milliseconds.
 */
static int
parse_timeout(char* str, long* num) {
    char* p_err;

    *num = strtol(str, &p_err, 10);

    if (p_err == str)
        FAIL;

    switch (*p_err) {
    case '\0':
        SUCCESS;
    case 's':
        if (*(p_err + 1) != '\0')
            FAIL;
        *num = *num * 1000;
        break;
    case 'm':
        if (*(p_err + 1) != 's' || *(p_err + 2) != '\0')
            FAIL;
    default:
        FAIL;
    }

    SUCCESS;
}

// ---- Argument parser -------------------------------------------------------

#define ARG_USAGE -1

typedef struct {
    const char* long_name;
    int         opt;
    const char* has_arg;
    int         flags; // reserved
    const char* doc;
} arg_option;

// clang-format off
static arg_option options[] = {
  {
    "children",  'C', NULL,    0,
    "Attach to child processes."
  },
  {
    "cpu",       'c', NULL,    0,
    "Sample on-CPU stacks only."
  },
  {
    "exposure",  'x', "n_sec", 0,
    "Sample for n_sec seconds only."
  },
  {
    "full",      'f', NULL,    0,
    "Produce the full set of metrics (time +mem -mem)."
  },
  {
    "gc",        'g', NULL,    0,
    "Sample the garbage collector state."
  },
  {
    "interval",  'i', "n_us",  0,
    "Sampling interval in microseconds (default is 100). Accepted units: s, ms, us."
  },
#ifdef AUSTINP
  {
    "kernel",    'k', NULL,    0,
    "Sample the kernel call stack."
  },
#endif
  {
    "memory",    'm', NULL,    0,
    "Profile memory usage."
  },
#ifndef AUSTINP
  {
    "native",    'n', NULL,    0,
    "Collect native call stacks alongside Python stacks."
  },
#endif
  {
    "output",    'o', "FILE",  0,
    "Specify an output file for the collected samples."
  },
  {
    "pid",       'p', "PID",   0,
    "Attach to the process with the given PID."
  },
  {
    "pipe",      'P', NULL,    0,
    "Pipe mode. Use when piping Austin output."
  },
  {
    "timeout",   't', "n_ms",  0,
    "Start up wait time in milliseconds (default is 3000). Accepted units: s, ms."
  },
  {
    "where",     'w', "PID",   0,
    "Dump the stacks of all the threads within the process with the given PID."
  },
  {
    "help",      '?', NULL,    0,
    "Give this help list."
  },
  {
    "usage",  ARG_USAGE, NULL, 0,
    "Give a short usage message."
  },
  {
    "version",   'V', NULL,    0,
    "Print program version."
  },
  {0}
};
// clang-format on

// ---- Help formatter --------------------------------------------------------

#ifdef PL_WIN
#include <io.h>
#define _isatty(fd) _isatty(fd)
#else
#include <unistd.h>
#define _isatty(fd) isatty(fd)
#endif

#include "ansi.h"

// Column at which option doc strings start.
#define OPT_COL    30
// Maximum line width before wrapping.
#define LINE_WIDTH 80

static const char* _prog_doc = "Austin is a frame stack sampler for CPython that is used to extract "
                               "profiling data out of a running Python process (and all its children, "
                               "if required) that requires no instrumentation and has practically no "
                               "impact on the tracee.";

// Returns true if color output is appropriate: stdout is a TTY, TERM is not
// "dumb", and the NO_COLOR environment variable is not set.
static int
_use_color(void) {
    if (!_isatty(STDOUT_FILENO))
        return 0;
    char* term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0)
        return 0;
    if (getenv("NO_COLOR"))
        return 0;
    return 1;
}

// Emit a color escape only when appropriate. Cached on first call.
#define C(code) (_color ? (code) : "")

// Print plain text with word-wrapping. col is the current cursor column;
// indent is the column to resume on after each line break.
static void
_print_text(const char* text, int col, int indent) {
    const char* p = text;

    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;

        const char* word = p;
        while (*p && *p != ' ')
            p++;
        int wlen = (int)(p - word);

        if (col > indent && col + 1 + wlen > LINE_WIDTH) {
            putchar('\n');
            for (int i = 0; i < indent; i++)
                putchar(' ');
            col = indent;
        } else if (col > indent) {
            putchar(' ');
            col++;
        }

        fwrite(word, 1, wlen, stdout);
        col += wlen;
    }

    putchar('\n');
}

static void
print_usage(void) {
    int         _color     = _use_color();
    // Build a compact synopsis: flag-only options grouped as [-abc],
    // then options with arguments as [-x ARG], then the positional.
    const char* prefix     = "Usage: ";
    int         prefix_len = (int)(strlen(prefix) + strlen(PROGRAM_NAME) + 1);
    int         indent     = prefix_len;
    int         col        = indent;

    printf("%s%s%s%s", C(BOLD), prefix, PROGRAM_NAME, C(CRESET));

    // Flags: short options without arguments
    char flags[64];
    int  nflags = 0;
    for (int i = 0; options[i].opt != 0; i++) {
        if (options[i].opt > 0 && options[i].has_arg == NULL)
            flags[nflags++] = (char)options[i].opt;
    }
    if (nflags > 0) {
        flags[nflags] = '\0';
        char token[72];
        sprintf(token, " [-%s]", flags);
        int tlen = (int)strlen(token);
        if (col + tlen > LINE_WIDTH) {
            printf("\n%*s", indent, "");
            col = indent;
        }
        printf("%s%s%s", C(HBLK), token, C(CRESET));
        col += tlen;
    }

    // Options with arguments
    for (int i = 0; options[i].opt != 0; i++) {
        if (options[i].has_arg == NULL)
            continue;
        char plain[64];
        if (options[i].opt > 0)
            sprintf(plain, " [-%c %s]", (char)options[i].opt, options[i].has_arg);
        else
            sprintf(plain, " [--%s=%s]", options[i].long_name, options[i].has_arg);
        int tlen = (int)strlen(plain);
        if (col + tlen > LINE_WIDTH) {
            printf("\n%*s", indent, "");
            col = indent;
        }
        printf("%s%s%s", C(HBLK), plain, C(CRESET));
        col += tlen;
    }

    // Positional
    const char* positional = " command [ARG...]";
    if (col + (int)strlen(positional) > LINE_WIDTH)
        printf("\n%*s", indent, "");
    printf("%s%s%s\n", C(HBLK), positional, C(CRESET));
}

static void
print_help(void) {
    int _color = _use_color();
    print_usage();
    putchar('\n');
    _print_text(_prog_doc, 0, 0);

    // Options section
    printf("\n%sOptions:%s\n", C(BYEL), C(CRESET));

    for (int i = 0; options[i].opt != 0; i++) {
        arg_option* o   = &options[i];
        int         col = 0;

        fputs("  ", stdout);
        col += 2;

        // Short option
        if (o->opt > 0) {
            printf("%s-%c%s, ", C(BCYN), (char)o->opt, C(CRESET));
            col += 4;
        } else {
            fputs("    ", stdout);
            col += 4;
        }

        // Long option
        printf("%s--%s%s", C(BCYN), o->long_name, C(CRESET));
        col += 2 + (int)strlen(o->long_name);

        // Metavar
        if (o->has_arg) {
            printf("%s <%s>%s", C(HBLK), o->has_arg, C(CRESET));
            col += 3 + (int)strlen(o->has_arg);
        }

        // Doc string
        if (o->doc) {
            if (col >= OPT_COL) {
                putchar('\n');
                for (int j = 0; j < OPT_COL; j++)
                    putchar(' ');
            } else {
                for (int j = col; j < OPT_COL; j++)
                    putchar(' ');
            }
            _print_text(o->doc, OPT_COL, OPT_COL);
        } else {
            putchar('\n');
        }
    }

    putchar('\n');
    printf("%sReport bugs at%s https://github.com/P403n1x87/austin/issues\n", C(HBLK), C(CRESET));
}

// ---- Parser ----------------------------------------------------------------

// Argument callback. Called on every argument parser event.
//
// The first argument is the option character, or 0 for a non-option argument.
// The second argument is either the argument of the option, if one is required,
// or NULL, when the first argument is not null, or the value of the non-option
// argument.
//
// Return 0 to continue parsing, or otherwise to stop.
typedef int (*arg_callback)(const int opt, const char* arg, const int index, char** argv);

// ----------------------------------------------------------------------------
static arg_option*
_find_long_opt(arg_option* opts, const char* opt_name) {
    arg_option*  retval = NULL;
    register int i      = 0;

    while (retval == NULL && opts[i].opt != 0) {
        if (opts[i].long_name != NULL) {
            char* equal = strchr(opt_name, '=');
            if (equal)
                *equal = 0;
            if (strcmp(opt_name, opts[i].long_name) == 0)
                retval = &opts[i];
            if (equal)
                *equal = '=';
        }

        i++;
    }

    return retval;
}

// ----------------------------------------------------------------------------
static arg_option*
_find_opt(arg_option* opts, char opt) {
    register int i = 0;

    while (opts[i].opt != 0) {
        if (opts[i].opt == opt)
            return &opts[i];

        i++;
    }

    return NULL;
}

// ----------------------------------------------------------------------------
static int
_handle_opt(arg_option* opt, arg_callback cb, int argi, int argc, char** argv) {
    char* opt_arg = NULL;

    if (opt) {
        char* equal = strchr(argv[argi], '=');
        if (opt->has_arg) {
            if (equal == NULL && (argi >= argc - 1 || argv[argi + 1][0] == '-'))
                return ARG_MISSING_OPT_ARG;

            opt_arg = equal ? equal + 1 : ((char*)argv[argi + 1]);
        } else if (equal != NULL)
            return ARG_UNEXPECTED_OPT_ARG;

        return cb(opt->opt, opt_arg, argi, argv);
    }

    return ARG_UNRECOGNISED_LONG_OPT;
}

// ----------------------------------------------------------------------------
static int
_handle_long_opt(arg_option* opts, arg_callback cb, int* argi, int argc, char** argv) {
    arg_option* opt    = _find_long_opt(opts, &argv[*argi][2]);
    int         cb_res = _handle_opt(opt, cb, *argi, argc, argv);
    if (cb_res)
        return cb_res;

    *argi += opt->has_arg && strchr(argv[*argi], '=') == NULL ? 2 : 1;

    return 0;
}

// ----------------------------------------------------------------------------
static int
_handle_opts(arg_option* opts, arg_callback cb, int* argi, int argc, char** argv) {
    const char* opt_str  = &argv[*argi][1];
    int         n_opts   = strlen(opt_str);
    arg_option* curr_opt = NULL;
    const char* equal    = strchr(argv[*argi], '=');

    for (register int i = 0; i < n_opts; i++) {
        if (opt_str[i] == '=')
            break;
        curr_opt = _find_opt(opts, opt_str[i]);
        if (curr_opt == NULL)
            return ARG_UNRECOGNISED_OPT;

        if (curr_opt->has_arg && (equal == NULL && i < n_opts - 1))
            return ARG_MISSING_OPT_ARG;
        int cb_res = _handle_opt(curr_opt, cb, *argi, argc, argv);
        if (cb_res)
            return cb_res;
    }

    if (isvalid(curr_opt))
        *argi += curr_opt->has_arg && equal == NULL ? 2 : 1;

    return 0;
}

// ----------------------------------------------------------------------------
static void
arg_error(const char* message) {
    fputs(PROGRAM_NAME ": ", stderr);
    fputs(message, stderr);
    fputc('\n', stderr);
    fputs("Try `" PROGRAM_NAME " --help' or `" PROGRAM_NAME " --usage' for more information.\n", stderr);
    exit(ARG_ERR_EXIT_STATUS);
}

// ----------------------------------------------------------------------------
static void
arg_parse(arg_option* opts, arg_callback cb, int argc, char** argv) {
    int a      = 1;
    int cb_res = 0;

    if (argc <= 1) {
        print_usage();
        exit(0);
    }

    while (a < argc) {
        if (argv[a][0] == '-') {
            if (argv[a][1] == '-')
                cb_res = _handle_long_opt(opts, cb, &a, argc, argv);
            else
                cb_res = _handle_opts(opts, cb, &a, argc, argv);
        } else {
            cb_res = cb(ARG_ARGUMENT, argv[a], a, argv);
            a++;
        }

        if (cb_res == ARG_STOP_PARSING)
            return;

        if (cb_res != ARG_CONTINUE_PARSING) {
            switch (cb_res) {
            case ARG_MISSING_OPT_ARG:
                arg_error("option requires an argument");
            case ARG_UNRECOGNISED_OPT:
            case ARG_UNRECOGNISED_LONG_OPT:
                arg_error("unrecognised option");
            case ARG_UNEXPECTED_OPT_ARG:
                arg_error("option takes no argument");
            default:
                arg_error("invalid argument");
            }
        }
    }
}

// ----------------------------------------------------------------------------
static int
cb(const int opt, const char* arg, const int index, char** argv) {
    switch (opt) {
    case 'i':
        if (fail(parse_interval((char*)arg, (long*)&(pargs.t_sampling_interval)))
            || pargs.t_sampling_interval > LONG_MAX)
            arg_error("the sampling interval must be a positive integer");
        break;

    case 't':
        if (fail(parse_timeout((char*)arg, (long*)&(pargs.timeout))) || pargs.timeout > LONG_MAX / 1000)
            arg_error("the timeout must be a positive integer");
        pargs.timeout *= 1000;
        break;

    case 'c':
        pargs.cpu = true;
        break;

    case 'm':
        pargs.memory = true;
        break;

    case 'f':
        pargs.full = true;
        break;

    case 'p':
        if (str_to_num((char*)arg, (long*)&pargs.attach_pid) == 1 || pargs.attach_pid <= 0)
            arg_error("invalid PID");
        break;

    case 'w':
        if (str_to_num((char*)arg, (long*)&pargs.attach_pid) == 1 || pargs.attach_pid <= 0)
            arg_error("invalid PID");
        pargs.where = true;
        break;

    case 'o':
        pargs.output_filename = (char*)arg;
        break;

    case 'C':
        pargs.children = true;
        break;

    case 'x':
        if (str_to_num((char*)arg, (long*)&(pargs.exposure)) == 1 || pargs.exposure > LONG_MAX)
            arg_error("the exposure must be a positive integer");
        break;

    case 'P':
        pargs.pipe = true;
        break;

    case 'g':
        pargs.gc = true;
        break;

#ifndef AUSTINP
    case 'n':
#if defined(PL_LINUX) && !defined(__x86_64__) && !defined(__aarch64__)
        fprintf(
            stderr, PROGRAM_NAME ": warning: native mode is not supported on this architecture "
                                 "and will be ignored (consider using austinp instead)\n"
        );
#elif defined(PL_WIN) && !defined(_M_X64)
        fprintf(
            stderr, PROGRAM_NAME ": warning: native mode is not supported on this architecture "
                                 "and will be ignored\n"
        );
#else
        pargs.native = true;
#endif
        break;
#endif

#ifdef AUSTINP
    case 'k':
        pargs.kernel = true;
        break;
#endif

    case '?':
        print_help();
        exit(0);

    case 'V':
        puts(PROGRAM_NAME " " VERSION);
        exit(0);

    case ARG_USAGE:
        print_usage();
        exit(0);

    case ARG_ARGUMENT:
        pargs.cmd = &argv[index];
        if (pargs.attach_pid != 0 && isvalid(pargs.cmd))
            arg_error("the -p option is incompatible with the command argument");
        return ARG_STOP_PARSING;

    default:
        print_usage();
        exit(ARG_UNRECOGNISED_OPT);
    }

    return ARG_CONTINUE_PARSING;
}

// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
int
parse_args(int argc, char** argv) {
    pargs.output_file = stdout;

    arg_parse(options, cb, argc, argv);

    if ((!isvalid(pargs.cmd) || !isvalid(*pargs.cmd)) && pargs.attach_pid == 0) {
        set_error(CMDLINE, "No command nor process ID provided");
        FAIL;
    }

    if (isvalid(pargs.output_filename)) {
        pargs.output_file = fopen(pargs.output_filename, "wb");
        if (pargs.output_file == NULL) {
            set_error(IO, "Cannot open output file");
            FAIL;
        }
    }
#ifdef PL_WIN
    else {
        // Set binary mode to prevent CR/LF conversion
        setmode(fileno(pargs.output_file), O_BINARY);
    }
#endif

    SUCCESS;
}
