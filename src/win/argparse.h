#ifndef ARGPARSE_H
#define ARGPARSE_H

#include <stdlib.h>


#define ARG_STOP_PARSING               1
#define ARG_CONTINUE_PARSING           0
#define ARG_MISSING_OPT_ARG           -1
#define ARG_UNRECOGNISED_LONG_OPT     -2
#define ARG_UNRECOGNISED_OPT          -3
#define ARG_INVALID_VALUE             -4


typedef struct {
  const char *        long_name;
  const char          opt;
  int                 has_arg;
} arg_option;


// Argument callback. Called on every argument parser event.
//
// The first argument is the option character, or 0 for a non-option argument.
// The second argument is either the argument of the option, if one is required,
// or NULL, when the first argument is not null, or the value of the non-option
// argument.
//
// Return 0 to continue parsing the arguments, or otherwise to stop.
typedef int (*arg_callback)(const char opt, const char * arg);


// Return 0 if all the arguments have been parsed. If interrupted, returns the
// number of arguments consumed so far. Otherwise return an error code.
int arg_parse(arg_option * opts, arg_callback cb, int argc, char ** argv);

// TODO: Implement error.

#endif
