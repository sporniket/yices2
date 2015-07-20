/*
 * The Yices SMT Solver. Copyright 2015 SRI International.
 *
 * This program may only be used subject to the noncommercial end user
 * license agreement which is downloadable along with this program.
 */

/*
 * Yices solver: using the Yices language
 * - read-eval loop
 */

#if defined(CYGWIN) || defined(MINGW)
#ifndef __YICES_DLLSPEC__
#define __YICES_DLLSPEC__ __declspec(dllexport)
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>

#include "api/context_config.h"
#include "api/smt_logic_codes.h"
#include "api/yices_extensions.h"
#include "api/yices_globals.h"
#include "context/context.h"
#include "context/dump_context.h"
#include "exists_forall/ef_client.h"
#include "frontend/common.h"
#include "frontend/yices/arith_solver_codes.h"
#include "frontend/yices/yices_help.h"
#include "frontend/yices/yices_lexer.h"
#include "frontend/yices/yices_parser.h"
#include "frontend/yices/yices_reval.h"
#include "frontend/yices/yices_tstack_ops.h"
#include "io/concrete_value_printer.h"
#include "model/model_eval.h"
#include "model/models.h"
#include "parser_utils/term_stack2.h"
#include "parser_utils/tstack_internals.h"
#include "solvers/bv/bvsolver.h"
#include "solvers/bv/dimacs_printer.h"
#include "solvers/funs/fun_solver.h"
#include "solvers/simplex/simplex.h"
#include "terms/rationals.h"
#include "utils/command_line.h"
#include "utils/cputime.h"
#include "utils/memsize.h"
#include "utils/string_utils.h"
#include "utils/timeout.h"

#include "yices.h"
#include "yices_exit_codes.h"




/********************
 *  GLOBAL OBJECTS  *
 *******************/

/*
 * PARSING/TERM CONSTRUCTION
 * - input_filename: name of the input file.
 *   If input_filename is NULL, we run in interactive mode and get input from stdin.
 * - lexer, parser, term_stack: to process the input commands
 * - include_depth = number of nested (include ...) commands being processed
 * - tracer: initialized to (stderr, 2) in verbose mode (otherwise NULL)
 *
 * GLOBAL FLAGS:
 * - interactive: true if no input file is given on the command line
 * - verbosity: verbosity level
 * - done: set to true when exit is called, or if there's an error and
 *   interactive is false (i.e., we exit on the first error unless we're
 *   in the interactive mode).
 *
 * OTHER
 * - timeout: timeout value in second (applies to check)
 *   timeout value = 0 means no timeout
 * - timeout_initialized: true once init_timeout is called
 *
 * COMMAND-LINE OPTIONS:
 * - logic_name: logic to use (option --logic=xxx)
 * - arith_name: arithmetic solver to use (option --arith-solver=xxx)
 * - mode_name:  option --mode=xxx
 *   by default, these are NULL
 *
 * CONTEXT CONFIGURATION
 * - logic_code = code for the logic_name (default is SMT_UNKNNOW)
 * - arith_code = code for the arithemtic solver (default is ARITH_SIMPLEX)
 * - mode_code = code for the mode (the default depends on the solver/logic)
 * - iflag = true if the integer solver is required
 * - qflag = true if support for quantifiers is required
 * - efmode = true to enable the exists/forall solver
 * - efdone = true after the first call to efsolve
 */
static char *input_filename;
static lexer_t lexer;
static parser_t parser;
static tstack_t stack;
static uint32_t include_depth;

static bool interactive;
static bool done;
static int32_t verbosity;
static tracer_t *tracer;

static uint32_t timeout;
static bool timeout_initialized;

static char *logic_name;
static char *arith_name;
static char *mode_name;

static smt_logic_t logic_code;
static arith_code_t arith_code;
static context_arch_t arch;
static context_mode_t mode;
static bool iflag;
static bool qflag;

/*
 * Context, model, and solver parameters
 */
static context_t *context;
static model_t *model;
static param_t parameters;

/* flag to indicate we are in exists forall mode. */
static bool efmode;


/*
 * If mode = one-shot or ef
 * - all assertions are pushed into this vector
 * - all assertions are then processed at once on the call to
 *   (check) or (ef-solve)
 */
static ivector_t delayed_assertions;

/*
 * Counters for run-time statistics
 * - ready_time = run time after the context is initialized
 * - check_process_time = total time of the last call to check
 */
static double ready_time, check_process_time;


/*
 * More parameters for preprocessing and simplifications
 * - these parameters are stored in the context but
 *   we want to keep a copy when esolver is true (since then
 *   context is NULL).
 */
typedef struct ctx_param_s {
  bool var_elim;
  bool arith_elim;
  bool bvarith_elim;
  bool flatten_or;
  bool eq_abstraction;
  bool keep_ite;
  bool splx_eager_lemmas;
  bool splx_periodic_icheck;
} ctx_param_t;

static ctx_param_t ctx_parameters;


/*
 * The exists forall client globals
 */
static ef_client_t ef_client_globals;


/**************************
 *  COMMAND-LINE OPTIONS  *
 *************************/

enum {
  logic_option,
  arith_option,
  mode_option,
  version_flag,
  help_flag,
  verbosity_option,
};

#define NUM_OPTIONS (verbosity_option+1)

static option_desc_t options[NUM_OPTIONS] = {
  { "logic", '\0', MANDATORY_STRING, logic_option },
  { "arith-solver", '\0', MANDATORY_STRING, arith_option },
  { "mode", '\0', MANDATORY_STRING, mode_option },
  { "version", 'V', FLAG_OPTION, version_flag },
  { "help", 'h', FLAG_OPTION, help_flag },
  { "verbosity", 'v', MANDATORY_INT, verbosity_option },
};



/*
 * Version and help
 */
static void print_version(FILE *f) {
  fprintf(f,
          "Yices %s\n"
          "Copyright SRI International.\n"
          "Linked with GMP %s\n"
	  "Copyright Free Software Foundation, Inc.\n"
          "Build date: %s\n"
          "Platform: %s (%s)\n",
          yices_version,
          gmp_version,
          yices_build_date,
          yices_build_arch,
          yices_build_mode);
  fflush(f);
}

static void print_help(char *progname) {
  printf("Usage: %s [options] filename\n\n", progname);
  printf("Options:\n"
         "  --version, -V             Display version and exit\n"
         "  --help, -h                Display this information\n"
	 "  --verbosity=<level>       Set verbosity level (default = 0)\n"
	 "           -v <level>\n"
         "  --logic=<name>            Configure for the given logic\n"
         "                             <name> must be an SMT-LIB logic code (e.g., QF_UFLIA)\n"
         "                                    or 'NONE' for propositional logic\n"
         "  --arith-solver=<solver>   Select the arithmetic solver\n"
         "                             <solver> may be either 'simplex' or 'floyd-warshall' or 'auto'\n"
         "  --mode=<mode>             Select the usage mode\n"
         "                             <mode> maybe 'one-shot' or 'multi-checks' or 'interactive'\n"
	 "                                    or 'push-pop' or 'ef'\n"
	 "\n"
	 "The mode are as follows:\n"
	 "\n"
	 "  one-shot: only one call to (check) is allowed\n"
	 "    no assertions are allowed after (check)\n"
	 "    (push) and (pop) are not supported\n"
	 "\n"
	 "  multi-checks: several calls (check) are allowed\n"
	 "    adding assertions after check is allowed\n"
	 "    (push) and (pop) are not supported\n"
	 "\n"
	 "  push-pop: like multi-check but with support for (push) and (pop)\n"
	 "\n"
	 "  interactive: like push-pop. In addition, Yices restores the context\n"
	 "    to a clean state if (check) is interrupted\n"
	 "\n"
	 "  ef: enable the exist-forall solver\n"
	 "    In this mode, (ef-solve) can be used\n"
	 "    This is like one-shot in that only one call to (ef-solve) is allowed\n"
         "\n"
         "For reporting bugs and other information, please see http://yices.csl.sri.com/\n");
  fflush(stdout);
}


/*
 * Print this if there's an error in the command line arguments
 */
static void print_usage(char *progname) {
  fprintf(stderr, "Try '%s --help' for more information\n", progname);
}

/*
 * Parse a mode string:
 * - return a code form CTX_MODE_ONECHEK to CTX_MODE_INTERACTIVE
 *   or the special code EFSOLVER_MODE
 * - return -1 if the mode is not recognized
 */
enum {
  EFSOLVER_MODE = 10,
};


/*
 * Parse a mode:
 * - return -1 if the mode is not recognized
 */
static int32_t context_mode_code(const char *name) {
  int32_t x;

  x = -1;
  if (strcmp(name, "one-shot") == 0) {
    x = CTX_MODE_ONECHECK;
  } else if (strcmp(name, "interactive") == 0) {
    x = CTX_MODE_INTERACTIVE;
  } else if (strcmp(name, "push-pop") == 0) {
    x = CTX_MODE_PUSHPOP;
  } else if (strcmp(name, "multi-checks") == 0) {
    x = CTX_MODE_MULTICHECKS;
  } else if (strcmp(name, "ef") == 0) {
    x = EFSOLVER_MODE;
  }

  return x;
}


/*
 * Processing of the command-line flags
 * - set input_filename, logic_name, and arith_name
 *   input_filename = NULL means no filename on the command line
 *   same thing for logic_name and arith_name.
 * - deal with --help, --version or with errors
 */
static void process_command_line(int argc, char *argv[]) {
  cmdline_parser_t parser;
  cmdline_elem_t elem;
  int32_t arch_code;
  int32_t mode_code;
  int32_t v;

  // set all options to their default value
  input_filename = NULL;
  logic_name = NULL;
  arith_name = NULL;
  mode_name = NULL;
  verbosity = 0;
  tracer = NULL;
  logic_code = SMT_UNKNOWN;
  arith_code = ARITH_SIMPLEX;
  mode_code = -1; // means not set
  efmode = false;

  init_cmdline_parser(&parser, options, NUM_OPTIONS, argv, argc);

  for (;;) {
    cmdline_parse_element(&parser, &elem);
    switch (elem.status) {
    case cmdline_done:
      goto done;

    case cmdline_argument:
      if (input_filename == NULL) {
        input_filename = elem.arg;
      } else {
        fprintf(stderr, "%s: can't have several input files\n", parser.command_name);
        goto bad_usage;
      }
      break;

    case cmdline_option:
      switch (elem.key) {
      case logic_option:
        if (logic_name == NULL) {
          logic_name = elem.s_value;
          logic_code = smt_logic_code(logic_name);
          if (logic_code == SMT_UNKNOWN) {
            fprintf(stderr, "%s: invalid logic %s\n", parser.command_name, logic_name);
            goto bad_usage;
          }
        } else if (strcmp(logic_name, elem.s_value) != 0) {
          fprintf(stderr, "%s: only one logic can be specified\n", parser.command_name);
          goto bad_usage;
        }
        break;

      case arith_option:
        if (arith_name == NULL) {
          arith_name = elem.s_value;
          arith_code = arith_solver_code(arith_name);
          if (arith_code == ARITH_UNKNOWN) {
            fprintf(stderr, "%s: invalid arithmetic solver %s\n", parser.command_name, arith_name);
            goto bad_usage;
          }
        } else if (strcmp(arith_name, elem.s_value) != 0) {
          fprintf(stderr, "%s: only one arithmetic solver can be specified\n", parser.command_name);
          goto bad_usage;
        }
        break;

      case mode_option:
        if (mode_name == NULL) {
          mode_name = elem.s_value;
          mode_code = context_mode_code(mode_name);
          if (mode_code < 0) {
            fprintf(stderr, "%s: invalid mode %s\n", parser.command_name, mode_name);
            goto bad_usage;
          }
        } else if (strcmp(mode_name, elem.s_value) != 0) {
          fprintf(stderr, "%s: only one mode can be specified\n", parser.command_name);
          goto bad_usage;
        }
        break;

      case version_flag:
        print_version(stdout);
        goto quick_exit;

      case help_flag:
        print_help(parser.command_name);
        goto quick_exit;

      case verbosity_option:
	v = elem.i_value;
	if (v < 0) {
	  fprintf(stderr, "%s: the verbosity level must be non-negative\n", parser.command_name);
	  goto bad_usage;
	}
        verbosity = v;
        break;

      default:
        assert(false);
        break;
      }
      break;

    case cmdline_error:
      cmdline_print_error(&parser, &elem);
      goto bad_usage;
    }
  }

 done:
  /*
   * convert logic and arith solver codes to context architecture + mode
   * also set iflag and qflag
   */
  switch (logic_code) {
  case SMT_UNKNOWN:
    if (arith_code == ARITH_FLOYD_WARSHALL) {
      fprintf(stderr, "%s: please specify the logic (either QF_IDL or QF_RDL)\n", parser.command_name);
      goto bad_usage;
    }
    // use default settings
    arch = CTX_ARCH_EGFUNSPLXBV;
    iflag = true;
    qflag = false;
    break;

  case QF_IDL:
    if (arith_code == ARITH_SIMPLEX) {
      arch = CTX_ARCH_SPLX;
    } else if (arith_code == ARITH_FLOYD_WARSHALL) {
      arch = CTX_ARCH_IFW;
    } else {
      arch = CTX_ARCH_AUTO_IDL;
    }
    iflag = false; // not relevant in IDL
    qflag = false;
    break;

  case QF_RDL:
    if (arith_code == ARITH_SIMPLEX) {
      arch = CTX_ARCH_SPLX;
    } else if (arith_code == ARITH_FLOYD_WARSHALL) {
      arch = CTX_ARCH_RFW;
    } else {
      arch = CTX_ARCH_AUTO_RDL;
    }
    iflag = false;
    qflag = false;
    break;


  default:
    assert(logic_name != NULL && 0 <= logic_code && logic_code < NUM_SMT_LOGICS);
    arch_code = arch_for_logic(logic_code);
    if (arch_code < 0) {
      fprintf(stderr, "%s: logic %s is not supported\n", parser.command_name, logic_name);
      exit(YICES_EXIT_ERROR);
    }
    arch = (context_arch_t) arch_code;
    iflag = iflag_for_logic(logic_code);
    qflag = qflag_for_logic(logic_code);
    break;
  }

  /*
   * Set the mode
   */
  if (mode_code < 0) {
    if ((logic_code == QF_IDL || logic_code == QF_RDL) && arch != CTX_ARCH_SPLX) {
      // Floyd-Warshall or 'Auto' --> mode must be one-shot
      mode = CTX_MODE_ONECHECK;
    } else if (input_filename != NULL) {
      mode = CTX_MODE_PUSHPOP; // non-interactive
    } else {
      mode = CTX_MODE_INTERACTIVE; // no input given: interactive mode
    }
  } else if (mode_code == EFSOLVER_MODE) {
    /*
     * EF-Solver enabled:
     * we set mode to ONE_CHECK so that assertions get added to the delayed_assertion vector
     */
    mode = CTX_MODE_ONECHECK;
    efmode = true;
  } else {
    assert(CTX_MODE_ONECHECK <= mode_code && mode_code <= CTX_MODE_INTERACTIVE);
    mode = (context_mode_t) mode_code;
    if ((logic_code == QF_IDL || logic_code == QF_RDL) && arch != CTX_ARCH_SPLX) {
      if (mode != CTX_MODE_ONECHECK) {
        fprintf(stderr, "%s: the Floyd-Warshall solvers support only mode='one-shot'\n", parser.command_name);
        goto bad_usage;
      }
    }
  }

  return;

 quick_exit:
  exit(YICES_EXIT_SUCCESS);

 bad_usage:
  print_usage(parser.command_name);
  exit(YICES_EXIT_USAGE);
}




/********************
 *  SIGNAL HANDLER  *
 *******************/

/*
 * On SIGINT: call stop_search if the context is SEARCHING
 *
 * On Windows (mingw) and Solaris, the signal handler is
 * reset to SIG_DFL before 'sigint_handler' is called.
 * So we must call signal inside this handler.
 */
static void sigint_handler(int signum) {
#if defined(SOLARIS) || defined(MINGW)
  void (*saved_handler)(int);
#endif

  assert(context != NULL);
  if (verbosity > 0) {
    fprintf(stderr, "\nInterrupted by signal %d\n", signum);
    fflush(stderr);
  }
  if (context_status(context) == STATUS_SEARCHING) {
    context_stop_search(context);
  }

#if defined(SOLARIS) || defined(MINGW)
  saved_handler = signal(SIGINT, sigint_handler);
  if (saved_handler == SIG_ERR) {
    perror("Yices: failed to install SIG_INT handler: ");
    exit(YICES_EXIT_INTERNAL_ERROR);
  }
#endif
}


/*
 * Other interrupts: exit with code INTERRUPTED
 */
static void default_handler(int signum) {
  if (verbosity > 0) {
    fprintf(stderr, "\nInterrupted by signal %d\n", signum);
    fflush(stderr);
  }
  exit(YICES_EXIT_INTERRUPTED);
}


/*
 * Initialize the signal handlers
 */
static void init_handlers(void) {
  signal(SIGINT, sigint_handler);
  signal(SIGABRT, default_handler);
#ifndef MINGW
  signal(SIGXCPU, default_handler);
#endif
}


/*
 * Reset the default handlers
 */
static void reset_handlers(void) {
  signal(SIGINT, SIG_DFL);
  signal(SIGABRT, SIG_DFL);
#ifndef MINGW
  signal(SIGXCPU, SIG_DFL);
#endif
}




/********************
 *  ERROR MESSAGES  *
 *******************/

/*
 * Error in the input: print an error message + location
 * - force exit if we're not in interactive mode
 */
static void report_error(const char *s) {
  reader_t *rd;

  rd = &parser.lex->reader;
  if (rd->name != NULL) {
    fprintf(stderr, "%s: ", rd->name);
  }
  fprintf(stderr, "%s (line %"PRId32", column %"PRId32")\n", s, reader_line(rd), reader_column(rd));
  done = !interactive;
}


/*
 * System error: call perror + print location
 * - force exit if we're not in interactive mode
 */
static void report_system_error(const char *s) {
  reader_t *rd;

  rd = &parser.lex->reader;
  if (rd->name != NULL) {
    fprintf(stderr, "%s: ", rd->name);
  }
  fprintf(stderr, "error at line %"PRId32": ", reader_line(rd));
  perror(s);

  done = !interactive;
}


/*
 * Error in parameter assignment:
 * - name = parameter name
 * - reason = more explanation
 */
static void report_invalid_param(const char *name) {
  reader_t *rd;

  rd = &parser.lex->reader;
  if (rd->name != NULL) {
    fprintf(stderr, "%s: ", rd->name);
  }
  fprintf(stderr, "invalid parameter %s (line %"PRId32", column %"PRId32")\n",
          name, reader_line(rd), reader_column(rd));
  done = !interactive;
}

static void report_invalid_param_value(const char *name, const char *reason) {
  reader_t *rd;

  rd = &parser.lex->reader;
  if (rd->name != NULL) {
    fprintf(stderr, "%s: ", rd->name);
  }
  fprintf(stderr, "invalid value for parameter %s: %s (line %"PRId32", column %"PRId32")\n",
          name, reason, reader_line(rd), reader_column(rd));
  done = !interactive;
}

static void report_negative_timeout(int32_t val) {
  reader_t *rd;

  rd = &parser.lex->reader;
  if (rd->name != NULL) {
    fprintf(stderr, "%s: ", rd->name);
  }
  fprintf(stderr, "invalid timeout value %d (line %"PRId32", column %"PRId32")\n", val, reader_line(rd), reader_column(rd));
  done = !interactive;
}


/*
 * Report that the previous command was executed (if verbose)
 */
static void print_ok(void) {
  if (verbosity > 0 && interactive && include_depth == 0) {
    fprintf(stderr, "ok\n");
    fflush(stderr);
  }
}


/*
 * Print the translation code returned by assert
 */
static void print_internalization_code(int32_t code) {
  assert(-NUM_INTERNALIZATION_ERRORS < code && code <= TRIVIALLY_UNSAT);
  if (code == TRIVIALLY_UNSAT) {
    fprintf(stderr, "unsat\n");
    fflush(stderr);
  } else if (verbosity > 0 && code == CTX_NO_ERROR) {
    print_ok();
  } else if (code < 0) {
    code = - code;
    report_error(code2error[code]);
  }
}


/*
 * Print the translation code returned by ef_analyze
 */
static void print_ef_analyze_code(ef_code_t code) {
  if (code == EF_NO_ERROR) {
    print_ok();
  } else {
    report_error(efcode2error[code]);
  }
}



/*
 * Error code from eval_in_model:
 * - code is an error code defined in model_eval.h
 */
static void report_eval_error(int32_t code) {
  switch (code) {
  case MDL_EVAL_UNKNOWN_TERM:
    report_error("eval failed: term is not defined in the model\n");
    break;

  case MDL_EVAL_QUANTIFIER:
    report_error("eval failed: can't evaluate quantifiers\n");
    break;

  case MDL_EVAL_LAMBDA:
    report_error("eval failed: can't evaluate lambdas\n");
    break;

  case MDL_EVAL_FAILED:
    report_error("eval failed: high-order terms\n");
    break;

  case MDL_EVAL_INTERNAL_ERROR:
    freport_bug(stderr, "Internal error in 'eval'");
    break;

  case MDL_EVAL_FREEVAR_IN_TERM:
  default:
    freport_bug(stderr,"Unexpected error code %"PRId32" in 'eval'", code);
    break;
  }
}

/*
 * Error code from show-implicant
 */
static void report_show_implicant_error(error_code_t code) {
  switch (code) {
  case EVAL_UNKNOWN_TERM:
    report_error("eval failed: encountered term undefined in the model\n");
    break;

  case EVAL_QUANTIFIER:
    report_error("eval failed: quantified terms\n");
    break;

  case EVAL_LAMBDA:
    report_error("eval failed: lambda terms\n");
    break;

  case EVAL_FREEVAR_IN_TERM:
  case EVAL_OVERFLOW:
  case EVAL_FAILED:
  case EVAL_CONVERSION_FAILED:
  case EVAL_NO_IMPLICANT:
  default:
    freport_bug(stderr,"Unexpected error code %"PRId32" in 'show-implicant'", code);
    break;
  }
}

/***************************
 *  MODEL ALLOCATION/FREE  *
 **************************/

/*
 * Allocate and initialize a model
 * - set has_alias true
 */
static model_t *new_model(void) {
  model_t *tmp;

  tmp = (model_t *) safe_malloc(sizeof(model_t));
  init_model(tmp, __yices_globals.terms, true);
  return tmp;
}


/*
 * Free model
 */
static void free_model(model_t *model) {
  delete_model(model);
  safe_free(model);
}



/****************************
 *  CONTEXT INITIALIZATION  *
 ***************************/

/*
 * Copy the context's parameters into ctx_params
 */
static void save_ctx_params(void) {
  assert(context != NULL);
  ctx_parameters.var_elim = context_var_elim_enabled(context);
  ctx_parameters.arith_elim = context_arith_elim_enabled(context);
  ctx_parameters.bvarith_elim = context_bvarith_elim_enabled(context);
  ctx_parameters.flatten_or = context_flatten_or_enabled(context);
  ctx_parameters.eq_abstraction = context_eq_abstraction_enabled(context);
  ctx_parameters.keep_ite = context_keep_ite_enabled(context);
  ctx_parameters.splx_eager_lemmas = splx_eager_lemmas_enabled(context);
  ctx_parameters.splx_periodic_icheck = splx_periodic_icheck_enabled(context);
}


/*
 * If there's no context: use some defaults for both ctx_parameters and parameters
 * - arch + logic are derived from the command-line options
 */
static void default_ctx_params(smt_logic_t logic, context_arch_t arch) {
  ctx_parameters.var_elim = true;
  ctx_parameters.arith_elim = true;
  ctx_parameters.bvarith_elim = true;
  ctx_parameters.flatten_or = true;
  ctx_parameters.eq_abstraction = true;
  ctx_parameters.keep_ite = false;
  ctx_parameters.splx_eager_lemmas = false;
  ctx_parameters.splx_periodic_icheck = false;

  //  init_params_to_defaults(&parameters);
  yices_set_default_params(&parameters, logic_code, arch);
}


/*
 * Allocate and initialize the global context and model.
 * Initialize the parameter table with default values.
 * Set the signal handlers.
 * - logic = SMT_UNKNOWN or an SMT code
 * - arch = architecture to use
 * - mode = which optional features are supported
 * - iflag = true to active the integer solver
 * - qflag = true to support quantifiers
 */
static void init_ctx(smt_logic_t logic, context_arch_t arch, context_mode_t mode, bool iflag, bool qflag) {
  model = NULL;
  context = yices_create_context(logic, arch, mode, iflag, qflag);
  yices_default_params_for_context(context, &parameters);
  save_ctx_params();
  if (tracer != NULL) {
    context_set_trace(context, tracer);
  }

  init_handlers();
}


/*
 * On exit: cleanup
 */
static void delete_ctx(void) {
  assert(context != NULL);
  reset_handlers();
  if (model != NULL) {
    free_model(model);
    model = NULL;
  }
  yices_free_context(context);
  context = NULL;
}

/***************************************
 *  UTILITIES TO DEAL WITH PARAMETERS  *
 **************************************/

/*
 * Tables for converting parameter id to parameter name
 * and branching code to branching name. One more table
 * for converting from EF generalization codes to strings.
 */

#define NUM_EF_GEN_MODES 4

static const char *param2string[NUM_PARAMETERS];
static const char *branching2string[NUM_BRANCHING_MODES];
static const char *efgen2string[NUM_EF_GEN_MODES];

/*
 * Print a parameter name and its value
 * - n = number of character for name + spaces (for alignment)
 */
// print 'name:    ' with as many spaces as required for n characters
// if name is more than n character long, print 'name:'
static void show_param_name(const char *name, uint32_t n) {
  uint32_t l;

  l = strlen(name) + 1;
  fputs(name, stdout);
  fputc(':', stdout);
  while (n > l) {
    fputc(' ', stdout);
    n --;
  }
}

static void show_bool_param(const char *name, bool value, uint32_t n) {
  show_param_name(name, n);
  if (value) {
    fputs(" true\n", stdout);
  } else {
    fputs(" false\n", stdout);
  }
}

static void show_pos32_param(const char *name, uint32_t value, uint32_t n) {
  show_param_name(name, n);
  printf(" %"PRIu32"\n", value);
}

static void show_float_param(const char *name, double value, uint32_t n) {
  show_param_name(name, n);
  if (value < 1.0) {
    printf(" %.4f\n", value);
  } else {
    printf(" %.2f\n", value);
  }
}

static void show_string_param(const char *name, const char *value, uint32_t n) {
  show_param_name(name, n);
  fputc(' ', stdout);
  fputs(value, stdout);
  fputc('\n', stdout);
}

// main function to display a parameter value
// p = parameter id
// n = size for alignment (as used in show_param_name)
static void show_param(yices_param_t p, uint32_t n) {
  switch (p) {
  case PARAM_VAR_ELIM:
    show_bool_param(param2string[p], ctx_parameters.var_elim, n);
    break;

  case PARAM_ARITH_ELIM:
    show_bool_param(param2string[p], ctx_parameters.arith_elim, n);
    break;

  case PARAM_BVARITH_ELIM:
    show_bool_param(param2string[p], ctx_parameters.bvarith_elim, n);
    break;

  case PARAM_FLATTEN:
    // this activates both flatten or and flatten diseq.
    show_bool_param(param2string[p], ctx_parameters.flatten_or, n);
    break;

  case PARAM_LEARN_EQ:
    show_bool_param(param2string[p], ctx_parameters.eq_abstraction, n);
    break;

  case PARAM_KEEP_ITE:
    show_bool_param(param2string[p], ctx_parameters.keep_ite, n);
    break;

  case PARAM_FAST_RESTARTS:
    show_bool_param(param2string[p], parameters.fast_restart, n);
    break;

  case PARAM_C_THRESHOLD:
    show_pos32_param(param2string[p], parameters.c_threshold, n);
    break;

  case PARAM_C_FACTOR:
    show_float_param(param2string[p], parameters.c_factor, n);
    break;

  case PARAM_D_THRESHOLD:
    show_pos32_param(param2string[p], parameters.d_threshold, n);
    break;

  case PARAM_D_FACTOR:
    show_float_param(param2string[p], parameters.c_factor, n);
    break;

  case PARAM_R_THRESHOLD:
    show_pos32_param(param2string[p], parameters.r_threshold, n);
    break;

  case PARAM_R_FRACTION:
    show_float_param(param2string[p], parameters.r_fraction, n);
    break;

  case PARAM_R_FACTOR:
    show_float_param(param2string[p], parameters.r_factor, n);
    break;

  case PARAM_VAR_DECAY:
    show_float_param(param2string[p], parameters.var_decay, n);
    break;

  case PARAM_RANDOMNESS:
    show_float_param(param2string[p], parameters.randomness, n);
    break;

  case PARAM_RANDOM_SEED:
    show_pos32_param(param2string[p], parameters.random_seed, n);
    break;

  case PARAM_BRANCHING:
    show_string_param(param2string[p], branching2string[parameters.branching], n);
    break;

  case PARAM_CLAUSE_DECAY:
    show_float_param(param2string[p], parameters.clause_decay, n);
    break;

  case PARAM_CACHE_TCLAUSES:
    show_bool_param(param2string[p], parameters.cache_tclauses, n);
    break;

  case PARAM_TCLAUSE_SIZE:
    show_pos32_param(param2string[p], parameters.tclause_size, n);
    break;

  case PARAM_DYN_ACK:
    show_bool_param(param2string[p], parameters.use_dyn_ack, n);
    break;

  case PARAM_DYN_BOOL_ACK:
    show_bool_param(param2string[p], parameters.use_bool_dyn_ack, n);
    break;

  case PARAM_OPTIMISTIC_FCHECK:
    show_bool_param(param2string[p], parameters.use_optimistic_fcheck, n);
    break;

  case PARAM_MAX_ACK:
    show_pos32_param(param2string[p], parameters.max_ackermann, n);
    break;

  case PARAM_MAX_BOOL_ACK:
    show_pos32_param(param2string[p], parameters.max_boolackermann, n);
    break;

  case PARAM_AUX_EQ_QUOTA:
    show_pos32_param(param2string[p], parameters.aux_eq_quota, n);
    break;

  case PARAM_AUX_EQ_RATIO:
    show_float_param(param2string[p], parameters.aux_eq_ratio, n);
    break;

  case PARAM_DYN_ACK_THRESHOLD:
    show_pos32_param(param2string[p], (uint32_t) parameters.dyn_ack_threshold, n);
    break;

  case PARAM_DYN_BOOL_ACK_THRESHOLD:
    show_pos32_param(param2string[p], (uint32_t) parameters.dyn_bool_ack_threshold, n);
    break;

  case PARAM_MAX_INTERFACE_EQS:
    show_pos32_param(param2string[p], parameters.max_interface_eqs, n);
    break;

  case PARAM_EAGER_LEMMAS:
    show_bool_param(param2string[p], ctx_parameters.splx_eager_lemmas, n);
    break;

  case PARAM_SIMPLEX_PROP:
    show_bool_param(param2string[p], parameters.use_simplex_prop, n);
    break;

  case PARAM_SIMPLEX_ADJUST:
    show_bool_param(param2string[p], parameters.adjust_simplex_model, n);
    break;

  case PARAM_PROP_THRESHOLD:
    show_pos32_param(param2string[p], parameters.max_prop_row_size, n);
    break;

  case PARAM_BLAND_THRESHOLD:
    show_pos32_param(param2string[p], parameters.bland_threshold, n);
    break;

  case PARAM_ICHECK:
    show_bool_param(param2string[p], ctx_parameters.splx_periodic_icheck, n);
    break;

  case PARAM_ICHECK_PERIOD:
    show_pos32_param(param2string[p], parameters.integer_check_period, n);
    break;

  case PARAM_MAX_UPDATE_CONFLICTS:
    show_pos32_param(param2string[p], parameters.max_update_conflicts, n);
    break;

  case PARAM_MAX_EXTENSIONALITY:
    show_pos32_param(param2string[p], parameters.max_extensionality, n);
    break;

  case PARAM_EF_FLATTEN_IFF:
    show_bool_param(param2string[p], ef_client_globals.ef_parameters.flatten_iff, n);
    break;

  case PARAM_EF_FLATTEN_ITE:
    show_bool_param(param2string[p], ef_client_globals.ef_parameters.flatten_ite, n);
    break;

  case PARAM_EF_GEN_MODE:
    show_string_param(param2string[p], efgen2string[ef_client_globals.ef_parameters.gen_mode], n);
    break;

  case PARAM_EF_MAX_SAMPLES:
    show_pos32_param(param2string[p], ef_client_globals.ef_parameters.max_samples, n);
    break;

  case PARAM_EF_MAX_ITERS:
    show_pos32_param(param2string[p], ef_client_globals.ef_parameters.max_iters, n);
    break;

  case PARAM_UNKNOWN:
  default:
    freport_bug(stderr,"invalid parameter id in 'show_param'");
    break;
  }
}





/**************
 *  COMMANDS  *
 *************/

/*
 * Exit: close the current include file or exit the toplevel
 */
static void yices_exit_cmd(void) {
  if (include_depth > 0) {
    parser_pop_lexer(&parser);
    include_depth --;
  } else {
    if (verbosity > 0) {
      fputs("exiting\n", stderr);
      fflush(stderr);
    }
    done = true;
  }
}


/*
 * Echo
 */
static void yices_echo_cmd(const char *s) {
  fputs(s, stdout);
  fflush(stdout);
}


/*
 * Include a file
 */
static void yices_include_cmd(const char *s) {
  if (parser_push_lexer(&parser, s) < 0) {
    report_system_error(s);
  } else {
    include_depth ++;
  }
}


/*
 * Parameter assignment
 */
static void yices_setparam_cmd(const char *param, const param_val_t *val) {
  bool tt;
  int32_t n;
  double x;
  branch_t b;
  ef_gen_option_t g;
  char* reason;

  reason = NULL;
  
  switch (find_param(param)) {
  case PARAM_VAR_ELIM:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ctx_parameters.var_elim = tt;
      if (context != NULL) {
	if (tt) {
	  enable_variable_elimination(context);
	} else {
	  disable_variable_elimination(context);
	}
      }
      print_ok();
    }
    break;

  case PARAM_ARITH_ELIM:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ctx_parameters.arith_elim = tt;
      if (context != NULL) {
	if (tt) {
	  enable_arith_elimination(context);
	} else {
	  disable_arith_elimination(context);
	}
      }
      print_ok();
    }
    break;

  case PARAM_BVARITH_ELIM:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ctx_parameters.bvarith_elim = tt;
      if (context != NULL) {
	if (tt) {
	  enable_bvarith_elimination(context);
	} else {
	  disable_bvarith_elimination(context);
	}
      }
      print_ok();
    }
    break;

  case PARAM_FLATTEN:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ctx_parameters.flatten_or = tt;
      if (context != NULL) {
	if (tt) {
	  enable_diseq_and_or_flattening(context);
	} else {
	  disable_diseq_and_or_flattening(context);
	}
      }
      print_ok();
    }
    break;

  case PARAM_LEARN_EQ:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ctx_parameters.eq_abstraction = tt;
      if (context != NULL) {
	if (tt) {
	  enable_eq_abstraction(context);
	} else {
	  disable_eq_abstraction(context);
	}
      }
      print_ok();
    }
    break;

  case PARAM_KEEP_ITE:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ctx_parameters.keep_ite = tt;
      if (context != NULL) {
	if (tt) {
	  enable_keep_ite(context);
	} else {
	  disable_keep_ite(context);
	}
      }
      print_ok();
    }
    break;

  case PARAM_FAST_RESTARTS:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      parameters.fast_restart = tt;
      print_ok();
    }
    break;

  case PARAM_C_THRESHOLD:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.c_threshold = n;
      print_ok();
    }
    break;

  case PARAM_C_FACTOR:
    if (param_val_to_factor(param, val, &x, &reason)) {
      parameters.c_factor = x;
      print_ok();
    }
    break;

  case PARAM_D_THRESHOLD:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.d_threshold = n;
      print_ok();
    }
    break;

  case PARAM_D_FACTOR:
    if (param_val_to_factor(param, val, &x, &reason)) {
      parameters.d_factor = x;
      print_ok();
    }
    break;

  case PARAM_R_THRESHOLD:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.r_threshold = n;
      print_ok();
    }
    break;

  case PARAM_R_FRACTION:
    if (param_val_to_ratio(param, val, &x, &reason)) {
      parameters.r_fraction = x;
      print_ok();
    }
    break;

  case PARAM_R_FACTOR:
    if (param_val_to_factor(param, val, &x, &reason)) {
      parameters.r_factor = x;
      print_ok();
    }
    break;

  case PARAM_VAR_DECAY:
    if (param_val_to_ratio(param, val, &x, &reason)) {
      parameters.var_decay = x;
      print_ok();
    }
    break;

  case PARAM_RANDOMNESS:
    if (param_val_to_ratio(param, val, &x, &reason)) {
      parameters.randomness = x;
      print_ok();
    }
    break;

  case PARAM_RANDOM_SEED:
    if (param_val_to_int32(param, val, &n, &reason)) {
      parameters.random_seed = (uint32_t) n;
      print_ok();
    }
    break;

  case PARAM_BRANCHING:
    if (param_val_to_branching(param, val, &b, &reason)) {
      parameters.branching = b;
      print_ok();
    }
    break;

  case PARAM_CLAUSE_DECAY:
    if (param_val_to_ratio(param, val, &x, &reason)) {
      parameters.clause_decay = x;
      print_ok();
    }
    break;

  case PARAM_CACHE_TCLAUSES:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      parameters.cache_tclauses = tt;
      print_ok();
    }
    break;

  case PARAM_TCLAUSE_SIZE:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.tclause_size = n;
      print_ok();
    }
    break;

  case PARAM_DYN_ACK:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      parameters.use_dyn_ack = tt;
      print_ok();
    }
    break;

  case PARAM_DYN_BOOL_ACK:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      parameters.use_bool_dyn_ack = tt;
      print_ok();
    }
    break;

  case PARAM_OPTIMISTIC_FCHECK:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      parameters.use_optimistic_fcheck = tt;
      print_ok();
    }
    break;

  case PARAM_MAX_ACK:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.max_ackermann = n;
      print_ok();
    }
    break;

  case PARAM_MAX_BOOL_ACK:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.max_boolackermann = n;
      print_ok();
    }
    break;

  case PARAM_AUX_EQ_QUOTA:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.aux_eq_quota = n;
      print_ok();
    }
    break;

  case PARAM_AUX_EQ_RATIO:
    if (param_val_to_posfloat(param, val, &x, &reason)) {
      parameters.aux_eq_ratio = x;
      print_ok();
    }
    break;

  case PARAM_DYN_ACK_THRESHOLD:
    if (param_val_to_pos16(param, val, &n, &reason)) {
      parameters.dyn_ack_threshold = (uint16_t) n;
      print_ok();
    }
    break;

  case PARAM_DYN_BOOL_ACK_THRESHOLD:
    if (param_val_to_pos16(param, val, &n, &reason)) {
      parameters.dyn_bool_ack_threshold = (uint16_t) n;
      print_ok();
    }
    break;

  case PARAM_MAX_INTERFACE_EQS:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.max_interface_eqs = n;
      print_ok();
    }
    break;

  case PARAM_EAGER_LEMMAS:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ctx_parameters.splx_eager_lemmas = tt;
      if (context != NULL) {
	if (tt) {
	  enable_splx_eager_lemmas(context);
	} else {
	  disable_splx_eager_lemmas(context);
	}
      }
      print_ok();
    }
    break;

  case PARAM_SIMPLEX_PROP:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      parameters.use_simplex_prop = tt;
      print_ok();
    }
    break;

  case PARAM_SIMPLEX_ADJUST:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      parameters.adjust_simplex_model = tt;
      print_ok();
    }
    break;

  case PARAM_PROP_THRESHOLD:
    if (param_val_to_nonneg32(param, val, &n, &reason)) {
      parameters.max_prop_row_size = n;
      print_ok();
    }
    break;

  case PARAM_BLAND_THRESHOLD:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.bland_threshold = n;
      print_ok();
    }
    break;

  case PARAM_ICHECK:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ctx_parameters.splx_periodic_icheck = tt;
      if (context != NULL) {
	if (tt) {
	  enable_splx_periodic_icheck(context);
	} else {
	  disable_splx_periodic_icheck(context);
	}
      }
      print_ok();
    }
    break;

  case PARAM_ICHECK_PERIOD:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.integer_check_period = n;
      print_ok();
    }
    break;

  case PARAM_MAX_UPDATE_CONFLICTS:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.max_update_conflicts = n;
      print_ok();
    }
    break;

  case PARAM_MAX_EXTENSIONALITY:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      parameters.max_extensionality = n;
      print_ok();
    }
    break;

  case PARAM_EF_FLATTEN_IFF:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ef_client_globals.ef_parameters.flatten_iff = tt;
      print_ok();
    }
    break;

  case PARAM_EF_FLATTEN_ITE:
    if (param_val_to_bool(param, val, &tt, &reason)) {
      ef_client_globals.ef_parameters.flatten_ite = tt;
      print_ok();
    }
    break;

  case PARAM_EF_GEN_MODE:
    if (param_val_to_genmode(param, val, &g, &reason)) {
      ef_client_globals.ef_parameters.gen_mode = g;
      print_ok();;
    }
    break;

  case PARAM_EF_MAX_SAMPLES:
    if (param_val_to_nonneg32(param, val, &n, &reason)) {
      ef_client_globals.ef_parameters.max_samples = n;
      print_ok();
    }
    break;

  case PARAM_EF_MAX_ITERS:
    if (param_val_to_pos32(param, val, &n, &reason)) {
      ef_client_globals.ef_parameters.max_iters = n;
      print_ok();
    }
    break;

  case PARAM_UNKNOWN:
  default:
    report_invalid_param(param);
    break;
  }
  if (reason != NULL){
    report_invalid_param_value(param, reason);
  }
}



/*
 * Print parameter of the given name
 */
static void yices_showparam_cmd(const char *param) {
  yices_param_t i;

  i = find_param(param);
  if (i != PARAM_UNKNOWN) {
    assert(0 <= i && i < NUM_PARAMETERS);
    show_param(i, 20);
    fflush(stdout);
  } else {
    report_invalid_param(param);
  }
}


/*
 * Print all the parameter settings
 */
static void yices_showparams_cmd(void) {
  uint32_t i;

  for (i=0; i<NUM_PARAMETERS; i++) {
    show_param(i, 20);
  }
  fputc('\n', stdout);
  fflush(stdout);
}


/*
 * Show-stats: print statistics
 */
static void show_stats(dpll_stats_t *stat) {
  printf("Core\n");
  printf(" restarts                : %"PRIu32"\n", stat->restarts);
  printf(" simplify db             : %"PRIu32"\n", stat->simplify_calls);
  printf(" reduce db               : %"PRIu32"\n", stat->reduce_calls);
  printf(" decisions               : %"PRIu64"\n", stat->decisions);
  printf(" random decisions        : %"PRIu64"\n", stat->random_decisions);
  printf(" propagations            : %"PRIu64"\n", stat->propagations);
  printf(" conflicts               : %"PRIu64"\n", stat->conflicts);
  printf(" theory propagations     : %"PRIu32"\n", stat->th_props);
  printf(" propagation-lemmas      : %"PRIu32"\n", stat->th_prop_lemmas);
  printf(" theory conflicts        : %"PRIu32"\n", stat->th_conflicts);
  printf(" conflict-lemmas         : %"PRIu32"\n", stat->th_conflict_lemmas);

  printf(" lits in pb. clauses     : %"PRIu64"\n", stat->prob_literals);
  printf(" lits in learned clauses : %"PRIu64"\n", stat->learned_literals);
  printf(" total lits. in learned  : %"PRIu64"\n", stat->literals_before_simpl);
  printf(" subsumed lits.          : %"PRIu64"\n", stat->subsumed_literals);
  printf(" deleted pb. clauses     : %"PRIu64"\n", stat->prob_clauses_deleted);
  printf(" deleted learned clauses : %"PRIu64"\n", stat->learned_clauses_deleted);
  printf(" deleted binary clauses  : %"PRIu64"\n", stat->bin_clauses_deleted);
}

static void show_egraph_stats(egraph_stats_t *stat) {
  printf("Egraph\n");
  printf(" prop. to core           : %"PRIu32"\n", stat->th_props);
  printf(" conflicts               : %"PRIu32"\n", stat->th_conflicts);
  printf(" non-distinct lemmas     : %"PRIu32"\n", stat->nd_lemmas);
  printf(" auxiliary eqs. created  : %"PRIu32"\n", stat->aux_eqs);
  printf(" dyn boolack. lemmas     : %"PRIu32"\n", stat->boolack_lemmas);
  printf(" other dyn ack.lemmas    : %"PRIu32"\n", stat->ack_lemmas);
  printf(" final checks            : %"PRIu32"\n", stat->final_checks);
  printf(" interface equalities    : %"PRIu32"\n", stat->interface_eqs);
}

static void show_funsolver_stats(fun_solver_stats_t *stat) {
  printf("Arrays\n");
  printf(" init. variables         : %"PRIu32"\n", stat->num_init_vars);
  printf(" init. edges             : %"PRIu32"\n", stat->num_init_edges);
  printf(" update axiom1           : %"PRIu32"\n", stat->num_update_axiom1);
  printf(" update axiom2           : %"PRIu32"\n", stat->num_update_axiom2);
  printf(" extensionality axioms   : %"PRIu32"\n", stat->num_extensionality_axiom);
}

static void show_simplex_stats(simplex_stats_t *stat) {
  printf("Simplex\n");
  printf(" init. variables         : %"PRIu32"\n", stat->num_init_vars);
  printf(" init. rows              : %"PRIu32"\n", stat->num_init_rows);
  printf(" init. atoms             : %"PRIu32"\n", stat->num_atoms);
  printf(" end atoms               : %"PRIu32"\n", stat->num_end_atoms);
  printf(" elim. candidates        : %"PRIu32"\n", stat->num_elim_candidates);
  printf(" elim. rows              : %"PRIu32"\n", stat->num_elim_rows);
  printf(" fixed vars after simpl. : %"PRIu32"\n", stat->num_simpl_fvars);
  printf(" rows after simpl.       : %"PRIu32"\n", stat->num_simpl_rows);
  printf(" fixed vars              : %"PRIu32"\n", stat->num_fixed_vars);
  printf(" rows in init. tableau   : %"PRIu32"\n", stat->num_rows);
  printf(" rows in final tableau   : %"PRIu32"\n", stat->num_end_rows);
  printf(" calls to make_feasible  : %"PRIu32"\n", stat->num_make_feasible);
  printf(" pivots                  : %"PRIu32"\n", stat->num_pivots);
  printf(" bland-rule activations  : %"PRIu32"\n", stat->num_blands);
  printf(" simple lemmas           : %"PRIu32"\n", stat->num_binary_lemmas);
  printf(" prop. to core           : %"PRIu32"\n", stat->num_props);
  printf(" derived bounds          : %"PRIu32"\n", stat->num_bound_props);
  printf(" productive propagations : %"PRIu32"\n", stat->num_prop_expl);
  printf(" conflicts               : %"PRIu32"\n", stat->num_conflicts);
  printf(" interface lemmas        : %"PRIu32"\n", stat->num_interface_lemmas);
  printf(" reduced inter. lemmas   : %"PRIu32"\n", stat->num_reduced_inter_lemmas);
  printf(" trichotomy lemmas       : %"PRIu32"\n", stat->num_tricho_lemmas);
  printf(" reduced tricho. lemmas  : %"PRIu32"\n", stat->num_reduced_tricho);
  if (stat->num_make_intfeasible > 0 || stat->num_dioph_checks > 0) {
    printf("Integer arithmetic\n");
    printf("Integer arithmetic\n");
    printf(" make integer feasible   : %"PRIu32"\n", stat->num_make_intfeasible);
    printf(" branch atoms            : %"PRIu32"\n", stat->num_branch_atoms);
    printf("bound strengthening\n");
    printf(" conflicts               : %"PRIu32"\n", stat->num_bound_conflicts);
    printf(" recheck conflicts       : %"PRIu32"\n", stat->num_bound_recheck_conflicts);
    printf("integrality tests\n");
    printf(" conflicts               : %"PRIu32"\n", stat->num_itest_conflicts);
    printf(" bound conflicts         : %"PRIu32"\n", stat->num_itest_bound_conflicts);
    printf(" recheck conflicts       : %"PRIu32"\n", stat->num_itest_recheck_conflicts);
    printf("diohpantine solver\n");
    printf(" gcd conflicts           : %"PRIu32"\n", stat->num_dioph_gcd_conflicts);
    printf(" dioph checks            : %"PRIu32"\n", stat->num_dioph_checks);
    printf(" dioph conflicts         : %"PRIu32"\n", stat->num_dioph_conflicts);
    printf(" bound conflicts         : %"PRIu32"\n", stat->num_dioph_bound_conflicts);
    printf(" recheck conflicts       : %"PRIu32"\n", stat->num_dioph_recheck_conflicts);
  }
}

static void show_bvsolver_stats(bv_solver_t *solver) {
  printf("Bit-vectors\n");
  printf(" variables               : %"PRIu32"\n", bv_solver_num_vars(solver));
  printf(" atoms                   : %"PRIu32"\n", bv_solver_num_atoms(solver));
  printf(" eq. atoms               : %"PRIu32"\n", bv_solver_num_eq_atoms(solver));
  printf(" dyn eq. atoms           : %"PRIu32"\n", solver->stats.on_the_fly_atoms);
  printf(" ge atoms                : %"PRIu32"\n", bv_solver_num_ge_atoms(solver));
  printf(" sge atoms               : %"PRIu32"\n", bv_solver_num_sge_atoms(solver));
  printf(" equiv lemmas            : %"PRIu32"\n", solver->stats.equiv_lemmas);
  printf(" interface lemmas        : %"PRIu32"\n", solver->stats.interface_lemmas);
}


static void yices_showstats_cmd(void) {
  smt_core_t *core;
  egraph_t *egraph;
  simplex_solver_t *simplex;
  fun_solver_t *fsolver;
  double run_time;
  double mem_used;

  run_time = get_cpu_time() - ready_time;
  if (run_time < 0.0) {
    run_time = 0.0;
  }

  if (!efmode) {
    core = context->core;

    show_stats(&core->stats);
    printf(" boolean variables       : %"PRIu32"\n", core->nvars);
    printf(" atoms                   : %"PRIu32"\n", core->atoms.natoms);

    egraph = context->egraph;
    if (egraph != NULL) {
      show_egraph_stats(&egraph->stats);
      printf(" egraph terms            : %"PRIu32"\n", egraph->terms.nterms);
      printf(" egraph eq_quota         : %"PRIu32"\n", egraph->aux_eq_quota);
      if (context_has_fun_solver(context)) {
	fsolver = context->fun_solver;
	show_funsolver_stats(&fsolver->stats);
      }
    }

    if (context_has_simplex_solver(context)) {
      simplex = (simplex_solver_t *) context->arith_solver;
      simplex_collect_statistics(simplex);
      show_simplex_stats(&simplex->stats);
    }

    if (context_has_bv_solver(context)) {
      show_bvsolver_stats(context->bv_solver);
    }
    fputc('\n', stdout);
    printf("Runtime of '(check)'     : %.4f s\n", check_process_time);
  }

  printf("Total runtime            : %.4f s\n", run_time);
  mem_used = mem_size() / (1024 * 1024);
  if (mem_used > 0) {
    printf("Memory used              : %.2f MB\n", mem_used);
  }
  fputc('\n', stdout);
  fflush(stdout);
}


/*
 * Reset statistics
 */
static void yices_resetstats_cmd(void) {
  check_process_time = 0.0;
  print_ok();
}


/*
 * Set a timeout
 */
static void yices_settimeout_cmd(int32_t val) {
  if (val < 0) {
    report_negative_timeout(val);
  } else {
    // set the timeout: 0 means no timeout
    timeout = (uint32_t) val;
    print_ok();
  }
}


/*
 * Show the timeout value
 */
static void yices_showtimeout_cmd(void) {
  if (timeout == 0) {
    printf("no timeout set\n");
  } else {
    printf("timeout = %"PRIu32" s\n", timeout);
  }
  fflush(stdout);
}


static void yices_dump_cmd(void) {
  if (efmode) {
    report_error("(dump-context) is not supported by the exists/forall solver");
  } else {
    assert(context != NULL);
    dump_context(stdout, context);
  }
}


/*
 * Help
 */
static void yices_help_cmd(const char *topic) {
  show_help(stdout, topic);
  printf("\n");
}


/*
 * Reset
 */
static void yices_reset_cmd(void) {
  if (efmode) {
    delete_ef_client(&ef_client_globals);
    ivector_reset(&delayed_assertions);
    model = NULL;
  } else {
    if (model != NULL) {
      free_model(model);
      model = NULL;
    }
    ivector_reset(&delayed_assertions);
    reset_context(context);
  }
  print_ok();
}


/*
 * Push
 */
static void yices_push_cmd(void) {
  if (efmode) {
    report_error("(push) is not supported by the exists/forall solver");
  } else if (! context_supports_pushpop(context)) {
    report_error("push/pop not supported by this context");
  } else {
    switch (context_status(context)) {
    case STATUS_UNKNOWN:
    case STATUS_SAT:
      // cleanup model and return to IDLE
      if (model != NULL) {
        free_model(model);
        model = NULL;
      }
      context_clear(context);
      assert(context_status(context) == STATUS_IDLE);
      // fall-through intended.

    case STATUS_IDLE:
      context_push(context);
      print_ok();
      break;

    case STATUS_UNSAT:
      // cannot take (push)
      fputs("The context is unsat; (push) is not allowed\n", stderr);
      fflush(stderr);
      break;

    case STATUS_SEARCHING:
    case STATUS_INTERRUPTED:
    default:
      // should not happen
      freport_bug(stderr,"unexpected context status in 'push'");
      break;
    }
  }
}



/*
 * Pop
 */
static void yices_pop_cmd(void) {
  if (efmode) {
    report_error("(pop) is not supported by the exists/forall solver");
  } else if (! context_supports_pushpop(context)) {
    report_error("push/pop not supported by this context");
  } else if (context_base_level(context) == 0) {
    report_error("pop not allowed at bottom level");
  } else {
    switch (context_status(context)) {
    case STATUS_UNKNOWN:
    case STATUS_SAT:
      // delete the model first
      if (model != NULL) {
        free_model(model);
        model = NULL;
      }
      context_clear(context);
      assert(context_status(context) == STATUS_IDLE);
      // fall-through intended
    case STATUS_IDLE:
      context_pop(context);
      print_ok();
      break;

    case STATUS_UNSAT:
      context_clear_unsat(context);
      context_pop(context);
      print_ok();
      break;

    case STATUS_SEARCHING:
    case STATUS_INTERRUPTED:
    default:
      freport_bug(stderr,"unexpected context status in 'pop'");
      break;
    }
  }
}


/*
 * Assert formula f
 */
static void yices_assert_cmd(term_t f) {
  smt_status_t status;
  int32_t code;

  /*
   * If efmode is true, we add f to the delayed assertions vector
   */
  if (efmode) {
    if (ef_client_globals.efdone) {
      report_error("more assertions are not allowed after (ef-solve)");
    } else if (yices_term_is_bool(f)) {
      ivector_push(&delayed_assertions, f);
      print_ok();
    } else {
      report_error("type error in assert: boolean term required");
    }

  } else {
    status = context_status(context);
    if (status != STATUS_IDLE && !context_supports_multichecks(context)) {
      report_error("more assertions are not allowed");
    } else {
      switch (status) {
      case STATUS_UNKNOWN:
      case STATUS_SAT:
	// cleanup then return to the idle state
	if (model != NULL) {
	  free_model(model);
	  model = NULL;
	}
	context_clear(context);
	assert(context_status(context) == STATUS_IDLE);
	// fall-through intended

      case STATUS_IDLE:
	if (yices_term_is_bool(f)) {
	  if (mode == CTX_MODE_ONECHECK) {
	    // delayed assertion
	    ivector_push(&delayed_assertions, f);
	    code = CTX_NO_ERROR;
	  } else {
	    code = assert_formula(context, f);
	  }
	  print_internalization_code(code);
	} else {
	  report_error("type error in assert: boolean term required");
	}
	break;

      case STATUS_UNSAT:
	// cannot take more assertions
	if (context_base_level(context) == 0) {
	  fputs("The context is unsat. Try (reset).\n", stderr);
	} else {
	  fputs("The context is unsat. Try (pop) or (reset).\n", stderr);
	}
	fflush(stderr);
	break;

      case STATUS_SEARCHING:
      case STATUS_INTERRUPTED:
      default:
	// should not happen
	freport_bug(stderr,"unexpected context status in 'assert'");
	break;
      }
    }
  }
}


/*
 * Timeout handler: call stop_search when triggered
 * - data = pointer to the context
 */
static void timeout_handler(void *data) {
  assert(data == context && context != NULL);
  if (context_status(data) == STATUS_SEARCHING) {
    context_stop_search(data);
    if (verbosity > 0) {
      fputs("\nTimeout\n", stderr);
      fflush(stderr);
    }
  }
}


/*
 * Auxiliary function: call check on the context and return the result
 * - collect run time for statistics
 * - set a timeout if timeout > 0
 */
static smt_status_t do_check(void) {
  double check_start_time;
  smt_status_t stat;

  /*
   * Set a timeout if requested.
   * We call init_timeout only now because the internal timeout
   * consumes resources even if it's never used.
   */
  if (timeout > 0) {
    if (!timeout_initialized) {
      init_timeout();
      timeout_initialized = true;
    }
    start_timeout(timeout, timeout_handler, context);
  }

  /*
   * Collect runtime statistics + call check
   */
  check_start_time = get_cpu_time();
  stat = check_context(context, &parameters);
  check_process_time = get_cpu_time() - check_start_time;
  if (check_process_time < 0.0) {
    check_process_time = 0.0;
  }

  /*
   * Clear timeout and reset it to 0
   */
  if (timeout > 0) {
    assert(timeout_initialized);
    clear_timeout();
    timeout = 0;
  }

  return stat;
}

/*
 * Check whether the context is satisfiable
 */
static void yices_check_cmd(void) {
  smt_status_t stat;
  int code;

  if (mode == CTX_MODE_ONECHECK) {
    if (efmode) {
      report_error("(check) is not supported by the exist/forall solver");
      return;
    } else {
      code = assert_formulas(context, delayed_assertions.size, delayed_assertions.data);
      if (code < 0) {
	print_internalization_code(code);
	return;
      }
    }
  }

  stat = context_status(context);
  switch (stat) {
  case STATUS_UNKNOWN:
  case STATUS_UNSAT:
  case STATUS_SAT:
    // already solved: print the status
    fputs(status2string[stat], stdout);
    fputc('\n', stdout);
    fflush(stdout);
    timeout = 0;  // clear timeout to be consistent
    break;

  case STATUS_IDLE:
    // call check than print the result
    // if the search was interrupted, cleanup
    stat = do_check();
    fputs(status2string[stat], stdout);
    fputc('\n', stdout);
    if (stat == STATUS_INTERRUPTED) {
      if (mode == CTX_MODE_INTERACTIVE) {
        context_cleanup(context);
        assert(context_status(context) == STATUS_IDLE);
      } else {
        // force quit
        done = true;
      }
    }
    fflush(stdout);
    break;

  case STATUS_SEARCHING:
  case STATUS_INTERRUPTED:
  default:
    // this should not happen
    freport_bug(stderr,"unexpected context status in 'check'");
    break;
  }
}


/*
 * Helper function for show-model, eval, and show-implicant:
 * - return true if the context's status is SAT or UNKNOWN
 *   also build the model if it does not exist
 * - return false and print an error message if the status
 *   is UNSAT or something else
 * - cmd_name = string for error reporting (name of the command executed)
 */
static bool context_has_model(const char *cmd_name) {
  bool has_model;

  has_model = false;
  switch (context_status(context)) {
  case STATUS_UNKNOWN:
  case STATUS_SAT:
    if (model == NULL) {
      model = new_model();
      context_build_model(model, context);
    }
    has_model = true;
    break;

  case STATUS_UNSAT:
    fputs("The context is unsat. No model.\n", stderr);
    fflush(stderr);
    break;

  case STATUS_IDLE:
    fputs("Can't build a model. Call (check) first.\n", stderr);
    fflush(stderr);
    break;

  case STATUS_SEARCHING:
  case STATUS_INTERRUPTED:
  default:
    // this should not happen
    freport_bug(stderr,"unexpected context status in '%s'", cmd_name);
    break;
  }

  return has_model;
}


/*
 * Build model if needed and display it
 */
static void yices_showmodel_cmd(void) {
  model_t *mdl;
  int32_t code;
  
  if (efmode) {
    code = 0;
    mdl = ef_get_model(&ef_client_globals, &code);
    if(mdl != NULL){
      if (yices_pp_model(stdout, mdl, 140, UINT32_MAX, 0) < 0) {
	report_system_error("stdout");
      }
    } else {
      fputs(efmodelcode2error[code], stderr);
      fflush(stderr);
    }
  } else if (context_has_model("show-model")) {
    // model_print(stdout, model);
    // model_print_full(stdout, model);
    if (yices_pp_model(stdout, model, 140, UINT32_MAX, 0) < 0) {
      report_system_error("stdout");
    }
    fflush(stdout);
  }
}



/*
 * Print the value of t in model
 */
static void show_val_in_model(model_t *model, term_t t) {
  evaluator_t evaluator;
  value_table_t *vtbl;
  value_t v;

  init_evaluator(&evaluator, model);
  v = eval_in_model(&evaluator, t);
  if (v >= 0) {
    vtbl = model_get_vtbl(model);
    if (object_is_function(vtbl, v)) {
      vtbl_print_function(stdout, vtbl, v, true);
    } else if (object_is_update(vtbl, v)) {
      vtbl_normalize_and_print_update(stdout, vtbl, yices_get_term_name(t), v, true);
    } else {
      vtbl_print_object(stdout, model_get_vtbl(model), v);
      fputc('\n', stdout);
    }
  } else {
    report_eval_error(v);
  }
  fflush(stdout);
  delete_evaluator(&evaluator);
}


/*
 * Eval term t in the current model
 * - build the model if needed
 */
static void yices_eval_cmd(term_t t) {
  model_t *mdl;
  int32_t code;


  if (efmode) {
    code = 0;
    mdl = ef_get_model(&ef_client_globals, &code);
    if(mdl != NULL){
      show_val_in_model(mdl, t);
    } else {
      fputs(efmodelcode2error[code], stderr);
      fflush(stderr);
    }
  } else if (context_has_model("eval")) {
    show_val_in_model(model, t);
  }

}


/*
 * EF SOLVER
 */

/*
 * Print the efsolver status
 */
static void print_ef_status(void) {
  ef_status_t stat;
  int32_t error;
  ef_solver_t *efsolver;

  efsolver = ef_client_globals.efsolver;

  assert(efsolver != NULL && ef_client_globals.efdone);

  if (verbosity > 0) {
    printf("ef-solve: %"PRIu32" iterations\n", efsolver->iters);
  }

  stat = efsolver->status;
  error = efsolver->error_code;

  switch (stat) {
  case EF_STATUS_SAT:
  case EF_STATUS_UNKNOWN:
  case EF_STATUS_UNSAT:
  case EF_STATUS_INTERRUPTED:
    fputs(ef_status2string[stat], stdout);
    fputc('\n', stdout);
    if (verbosity > 0) {
      if (stat == EF_STATUS_SAT) {
        print_ef_solution(stdout, efsolver);
        fputc('\n', stdout);
      }
    }
    fflush(stdout);
    break;

  case EF_STATUS_SUBST_ERROR:
    if (error == -1) {
      report_error("ef-solve failed: degree overflow in substitution");
    } else {
      assert(error == -2);
      freport_bug(stderr, "ef-solve failed: internal error");
    }
    break;

  case EF_STATUS_ASSERT_ERROR:
    assert(error < 0);
    print_internalization_code(error);
    break;

  case EF_STATUS_MDL_ERROR:
  case EF_STATUS_IMPLICANT_ERROR:
  case EF_STATUS_PROJECTION_ERROR:
  case EF_STATUS_TVAL_ERROR:
  case EF_STATUS_CHECK_ERROR:
  case EF_STATUS_ERROR:
  case EF_STATUS_IDLE:
  case EF_STATUS_SEARCHING:
    freport_bug(stderr, "ef-status: %s\n", ef_status2string[stat]);
    break;

  }
}

/*
 * New command: ef-solve
 */
static void yices_efsolve_cmd(void) {

  if (efmode) {

    ef_solve(&ef_client_globals, &delayed_assertions, &parameters, logic_code, arch, tracer);

    if (ef_client_globals.efcode != EF_NO_ERROR) {
      
      // error in preprocessing
      print_ef_analyze_code(ef_client_globals.efcode);
      
    } else {

      print_ef_status();
      
    } 

  }  else {

    report_error("(ef-solve) not supported. Use option --mode=ef");
    
  }
}





/*
 * EXPORT TO DIMACS
 */

/*
 * Export ctx content in DIMACS format
 * - s = file name
 */
static void do_export(context_t *ctx, const char *s) {
  FILE *f;

  f = fopen(s, "w");
  if (f == NULL) {
    report_system_error(s);
  } else {
    dimacs_print_bvcontext(f, ctx);
    fclose(f);
    print_ok();
  }
}


/*
 * Force bitblasting then export
 * - s = filename
 * - ctx's status must be IDLE when this is called
 */
static void bitblast_then_export(context_t *ctx, const char *s) {
  smt_status_t stat;

  assert(context_status(ctx) == STATUS_IDLE);
  stat = precheck_context(ctx);
  switch (stat) {
  case STATUS_UNKNOWN:
    do_export(ctx, s);
    if (context_supports_multichecks(ctx)) {
      assert(ctx == context);
      context_clear(ctx); // restore to IDLE
    }
    break;

  case STATUS_UNSAT:
    do_export(ctx, s);
    break;

  case STATUS_INTERRUPTED:
    if (context_supports_cleaninterrupt(ctx)) {
      context_cleanup(ctx);
      assert(context_status(ctx) == STATUS_IDLE);
    }
    report_error("export-to-dimacs interrupted\n");
    break;

  default:
    freport_bug(stderr,"unexpected context status after pre-check");
    break;
  }
}


/*
 * Export the assertions --> use the ef_analyzer first
 * - then add all the assertions into a temporary context
 * - we disable variable elimination (could check the global parameters
 *   and do this optionally?)
 */
static void export_ef_problem(const char *s) {
  context_t *aux;
  ivector_t all_ef;
  int code;

  build_ef_problem(&ef_client_globals, &delayed_assertions);
  if (ef_client_globals.efcode != EF_NO_ERROR) {
    print_ef_analyze_code(ef_client_globals.efcode);
  } else {
    assert(ef_client_globals.efprob != NULL);

    // convert the ef-problem to a conjunction of formulas
    init_ivector(&all_ef, 10);
    ef_prob_collect_conjuncts(ef_client_globals.efprob, &all_ef);

    // assert these in a temporary context
    aux = yices_create_context(logic_code, arch, CTX_MODE_ONECHECK, false, false);
    disable_variable_elimination(aux);
    disable_bvarith_elimination(aux);
    code = assert_formulas(aux, all_ef.size, all_ef.data);
    if (code < 0) {
      print_internalization_code(code);
    } else {
      bitblast_then_export(aux, s);
    }

    yices_free_context(aux);
    delete_ivector(&all_ef);
  }
}


/*
 * Export the delayed assertions
 */
static void export_delayed_assertions(const char *s) {
  context_t *aux;
  int code;

  aux = yices_create_context(logic_code, arch, CTX_MODE_ONECHECK, false, false);
  disable_variable_elimination(aux);
  disable_bvarith_elimination(aux);
  code = assert_formulas(aux, delayed_assertions.size, delayed_assertions.data);
  if (code < 0) {
    print_internalization_code(code);
  } else {
    bitblast_then_export(aux, s);
  }
  yices_free_context(aux);
}


/*
 * s = FILE NAME
 */
static void yices_export_cmd(const char *s) {
  switch (logic_code) {
  case NONE:
  case QF_BV:
    if (efmode) {
      export_ef_problem(s);
    } else if (mode == CTX_MODE_ONECHECK) {
      export_delayed_assertions(s);
    } else {
      assert(context != NULL && (context->logic == NONE || context->logic == QF_BV));
      if (context_status(context) == STATUS_IDLE) {
	bitblast_then_export(context, s);
      } else {
	do_export(context, s);
      }
    }
    break;

  default:
    // Can't convert to DIMACS (at least not for sure)
    report_error("(export-to-dimacs ...) not supported. Use option --logic=NONE or --logic=QF_BV");;
    break;
  }

}


/*
 * Implicant computation
 */
static void yices_show_implicant_cmd(void) {
  term_vector_t v;
  int32_t code;

  if (efmode) {
    report_error("(show-implicant) is not supported by the exists/forall solver");
  } else if (mode != CTX_MODE_ONECHECK) {
    report_error("(show-implicant) is not supported. Use --mode=one-shot");
  } else if (context_has_model("show-implicant")) {
    yices_init_term_vector(&v);
    code = yices_implicant_for_formulas(model, delayed_assertions.size, delayed_assertions.data, &v);
    if (code < 0) {
      report_show_implicant_error(yices_error_code());
    } else {
      if (yices_pp_term_array(stdout, v.size, v.data, 140, UINT32_MAX, 0, 0) < 0) {
	/*
	 * Error in pp_term_array
	 */
	if (yices_error_code() == OUTPUT_ERROR) {
	  report_system_error("stdout");
	} else {
	  freport_bug(stderr, "invalid term in 'show-implicant'");
	}
      }
      fflush(stdout);
    }
    yices_delete_term_vector(&v);
  }
}


/*************************
 *  TERM STACK WRAPPERS  *
 ************************/

/*
 * Variants of define-term and define-type:
 * - call the default then print_ok()
 */
static void check_def_yices_type(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  call_tstack_check(stack, DEFINE_TYPE, f, n);
}

static void eval_def_yices_type(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  call_tstack_eval(stack, DEFINE_TYPE, f, n);
  print_ok();
}

static void check_def_yices_term(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  call_tstack_check(stack, DEFINE_TERM, f, n);
}

static void eval_def_yices_term(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  call_tstack_eval(stack, DEFINE_TERM, f, n);
  print_ok();
}


/*
 * [exit-cmd]
 */
static void check_exit_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, EXIT_CMD);
  check_size(stack, n == 0);
}

static void eval_exit_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_exit_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [check-cmd]
 */
static void check_check_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, CHECK_CMD);
  check_size(stack, n == 0);
}

static void eval_check_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_check_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [push-cmd]
 */
static void check_push_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, PUSH_CMD);
  check_size(stack, n == 0);
}

static void eval_push_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_push_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}



/*
 * [pop-cmd]
 */
static void check_pop_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, POP_CMD);
  check_size(stack, n == 0);
}

static void eval_pop_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_pop_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}



/*
 * [reset-cmd]
 */
static void check_reset_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, RESET_CMD);
  check_size(stack, n == 0);
}

static void eval_reset_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_reset_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}



/*
 * [showmodel-cmd]
 */
static void check_showmodel_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, SHOWMODEL_CMD);
  check_size(stack, n == 0);
}

static void eval_showmodel_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_showmodel_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [dump-cmd]
 */
static void check_dump_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, DUMP_CMD);
  check_size(stack, n == 0);
}

static void eval_dump_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_dump_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}



/*
 * [echo <string>]
 */
static void check_echo_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, ECHO_CMD);
  check_size(stack, n == 1);
  check_tag(stack, f, TAG_STRING);
}

static void eval_echo_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_echo_cmd(f->val.string);
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [include <string>]
 */
static void check_include_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, INCLUDE_CMD);
  check_size(stack, n == 1);
  check_tag(stack, f, TAG_STRING);
}

static void eval_include_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_include_cmd(f->val.string);
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [assert <term>]
 */
static void check_assert_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, ASSERT_CMD);
  check_size(stack, n == 1);
}

static void eval_assert_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  term_t t;

  t = get_term(stack, f);
  yices_assert_cmd(t);
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [eval <term>]
 */
static void check_eval_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, EVAL_CMD);
  check_size(stack, n == 1);
}

static void eval_eval_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  term_t t;

  t = get_term(stack, f);
  yices_eval_cmd(t);
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [set-param <symbol> <value>]
 */
static void check_setparam_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, SET_PARAM_CMD);
  check_size(stack, n == 2);
  check_tag(stack, f, TAG_SYMBOL);
}

static void eval_setparam_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  stack_elem_t *e;
  param_val_t aux;

  e = f + 1;
  switch (e->tag) {
  case TAG_SYMBOL:
    aux.tag = PARAM_VAL_SYMBOL;
    aux.val.symbol = e->val.string;
    break;

  case TAG_RATIONAL:
    aux.tag = PARAM_VAL_RATIONAL;
    aux.val.rational = &e->val.rational;
    break;

  case TAG_TERM:
    if (e->val.term == yices_true()) {
      aux.tag = PARAM_VAL_TRUE;
    } else if (e->val.term == yices_false()) {
      aux.tag = PARAM_VAL_FALSE;
    } else {
      aux.tag = PARAM_VAL_ERROR;
    }
    break;

  default:
    aux.tag = PARAM_VAL_ERROR;
    break;
  }

  yices_setparam_cmd(f->val.string, &aux);

  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [show-param <symbol>]
 */
static void check_showparam_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, SHOW_PARAM_CMD);
  check_size(stack, n == 1);
  check_tag(stack, f, TAG_SYMBOL);
}

static void eval_showparam_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_showparam_cmd(f->val.string);
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [show-params]
 */
static void check_showparams_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, SHOW_PARAMS_CMD);
  check_size(stack, n == 0);
}

static void eval_showparams_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_showparams_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [show-stats]
 */
static void check_showstats_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, SHOW_STATS_CMD);
  check_size(stack, n == 0);
}

static void eval_showstats_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_showstats_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [reset-stats]
 */
static void check_resetstats_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, RESET_STATS_CMD);
  check_size(stack, n == 0);
}

static void eval_resetstats_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_resetstats_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}



/*
 * [show-timeout]
 */
static void check_showtimeout_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, SHOW_TIMEOUT_CMD);
  check_size(stack, n == 0);
}

static void eval_showtimeout_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_showtimeout_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [set-timeout <rational>]
 */
static void check_settimeout_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, SET_TIMEOUT_CMD);
  check_size(stack, n == 1);
  check_tag(stack, f, TAG_RATIONAL);
}


static void eval_settimeout_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  int32_t timeout;

  timeout = get_integer(stack, f);
  yices_settimeout_cmd(timeout); // will check for timeout < 0 etc.
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [help <topic>] or [help]
 */
static void check_help_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, HELP_CMD);
  check_size(stack, n <= 1);
  if (n == 1) {
    if (f->tag != TAG_STRING && f->tag != TAG_SYMBOL) {
      raise_exception(stack, f, TSTACK_NOT_A_STRING); // should use a different code for STING or SYMBOL
    }
  }
}

static void eval_help_cmd(tstack_t *stack,  stack_elem_t *f, uint32_t n) {
  char *topic;

  topic = NULL;
  if (n == 1) {
    topic = f->val.string;
  }
  yices_help_cmd(topic);
  tstack_pop_frame(stack);
  no_result(stack);
}



/*
 * [ef-solve]
 */
static void check_efsolve_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, EFSOLVE_CMD);
  check_size(stack, n == 0);
}

static void eval_efsolve_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_efsolve_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * [export <filename>]
 */
static void check_export_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, EXPORT_CMD);
  check_size(stack, n == 1);
  check_tag(stack, f, TAG_STRING);
}

static void eval_export_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_export_cmd(f->val.string);
  tstack_pop_frame(stack);
  no_result(stack);
}

/*
 * [show-implicant]
 */
static void check_showimplicant_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  check_op(stack, SHOW_IMPLICANT_CMD);
  check_size(stack, n == 0);
}

static void eval_showimplicant_cmd(tstack_t *stack, stack_elem_t *f, uint32_t n) {
  yices_show_implicant_cmd();
  tstack_pop_frame(stack);
  no_result(stack);
}


/*
 * Initialize the term stack and add these commmands
 */
static void init_yices_tstack(tstack_t *stack) {
  init_tstack(stack, NUM_YICES_OPCODES);
  tstack_add_op(stack, DEF_YICES_TYPE, false, eval_def_yices_type, check_def_yices_type);
  tstack_add_op(stack, DEF_YICES_TERM, false, eval_def_yices_term, check_def_yices_term);
  tstack_add_op(stack, EXIT_CMD, false, eval_exit_cmd, check_exit_cmd);
  tstack_add_op(stack, ASSERT_CMD, false, eval_assert_cmd, check_assert_cmd);
  tstack_add_op(stack, CHECK_CMD, false, eval_check_cmd, check_check_cmd);
  tstack_add_op(stack, SHOWMODEL_CMD, false, eval_showmodel_cmd, check_showmodel_cmd);
  tstack_add_op(stack, EVAL_CMD, false, eval_eval_cmd, check_eval_cmd);
  tstack_add_op(stack, PUSH_CMD, false, eval_push_cmd, check_push_cmd);
  tstack_add_op(stack, POP_CMD, false, eval_pop_cmd, check_pop_cmd);
  tstack_add_op(stack, RESET_CMD, false, eval_reset_cmd, check_reset_cmd);
  tstack_add_op(stack, ECHO_CMD, false, eval_echo_cmd, check_echo_cmd);
  tstack_add_op(stack, INCLUDE_CMD, false, eval_include_cmd, check_include_cmd);
  tstack_add_op(stack, SET_PARAM_CMD, false, eval_setparam_cmd, check_setparam_cmd);
  tstack_add_op(stack, SHOW_PARAM_CMD, false, eval_showparam_cmd, check_showparam_cmd);
  tstack_add_op(stack, SHOW_PARAMS_CMD, false, eval_showparams_cmd, check_showparams_cmd);
  tstack_add_op(stack, SHOW_STATS_CMD, false, eval_showstats_cmd, check_showstats_cmd);
  tstack_add_op(stack, RESET_STATS_CMD, false, eval_resetstats_cmd, check_resetstats_cmd);
  tstack_add_op(stack, SET_TIMEOUT_CMD, false, eval_settimeout_cmd, check_settimeout_cmd);
  tstack_add_op(stack, SHOW_TIMEOUT_CMD, false, eval_showtimeout_cmd, check_showtimeout_cmd);
  tstack_add_op(stack, HELP_CMD, false, eval_help_cmd, check_help_cmd);
  tstack_add_op(stack, EFSOLVE_CMD, false, eval_efsolve_cmd, check_efsolve_cmd);
  tstack_add_op(stack, EXPORT_CMD, false, eval_export_cmd, check_export_cmd);
  tstack_add_op(stack, SHOW_IMPLICANT_CMD, false, eval_showimplicant_cmd, check_showimplicant_cmd);
  tstack_add_op(stack, DUMP_CMD, false, eval_dump_cmd, check_dump_cmd);
}


/**********
 *  MAIN  *
 *********/

/*
 * This is a separate function so that we can invoke yices via foreign
 * a function call in LISP.
 */
int yices_main(int argc, char *argv[]) {
  int32_t code;
  int exit_code;

  // Deal with command-line options
  process_command_line(argc, argv);

  /*
   * Check the input file
   * - initialize the lexer
   * - set the interactive flag and timeout
   */
  interactive = false;
  timeout = 0;
  timeout_initialized = false;
  include_depth = 0;
  ready_time = 0.0;
  check_process_time = 0.0;

  if (input_filename == NULL) {
    init_yices_stdin_lexer(&lexer);
    interactive = true;
  } else if (init_yices_file_lexer(&lexer, input_filename) < 0) {
    perror(input_filename);
    exit(YICES_EXIT_FILE_NOT_FOUND);
  }

  /*
   * Create the tracer object
   */
  if (verbosity > 0) {
    tracer = (tracer_t *) safe_malloc(sizeof(tracer_t));
    init_trace(tracer);
    set_trace_vlevel(tracer, verbosity);
  }

  /*
   * The lexer is ready: initialize the other structures
   */
  init_ivector(&delayed_assertions, 10);
  yices_init();
  init_yices_tstack(&stack);
  init_parameter_name_table();

  init_ef_client(&ef_client_globals);
  

  init_parser(&parser, &lexer, &stack);
  if (verbosity > 0) {
    print_version(stderr);
  }

  if (efmode) {
    context = NULL;
    model = NULL;
    default_ctx_params(logic_code, arch);
  } else {
    init_ctx(logic_code, arch, mode, iflag, qflag);
  }
  ready_time = get_cpu_time();

  /*
   * Read-eval loop
   * - done is set to true when (exit) is called
   *   or on the first error if we're not in interactive mode
   * - exit_code is set to SUCCESS by default and to SYNTAX_ERROR
   */
  done = false;
  exit_code =  YICES_EXIT_SUCCESS;
  while (current_token(&lexer) != TK_EOS && !done) {
    if (interactive && include_depth == 0) {
      // prompt
      fputs("yices> ", stderr);
      fflush(stderr);
    }
    code = parse_yices_command(&parser, stderr);
    if (code < 0) {
      // error
      if (interactive) {
        /*
         * If the error occurs within an include file,
         * we close all included files and return to main
         */
        while (include_depth > 0) {
          parser_pop_lexer(&parser);
          include_depth --;
        }
        flush_lexer(&lexer);
      } else {
        done = true;
        exit_code = YICES_EXIT_SYNTAX_ERROR;
      }
    }
  }

  /*
   * Clean up
   */
  if (efmode) {
    delete_ef_client(&ef_client_globals);
  } else {
    delete_ctx();
  }


  delete_parser(&parser);
  if (interactive) {
    // keep stdin open
    close_lexer_only(&lexer);
  } else {
    close_lexer(&lexer);
  }
  delete_tstack(&stack);
  delete_ivector(&delayed_assertions);
  if (tracer != NULL) {
    delete_trace(tracer);
    safe_free(tracer);
    tracer = NULL;
  }

  yices_exit();

  if (timeout_initialized) {
    delete_timeout();
  }

  return exit_code;
}

