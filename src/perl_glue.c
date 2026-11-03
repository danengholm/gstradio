/* Embedded Perl, following perlembed/perlcall (IceS 0.4 pm_perl.c). */
#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "perl_glue.h"

static PerlInterpreter* my_perl = NULL;
static int sys_initialized = 0;

EXTERN_C void boot_DynaLoader(pTHX_ CV* cv);

/* Needed so that modules used by the script can load XS extensions. */
static void xs_init(pTHX) {
  newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, __FILE__);
}

int ices_perl_start(const char* module, char* errbuf, size_t errlen) {
  static char arg0[] = "";
  static char inc_moddir[] = "-I" ICES_MODULEDIR;
  static char inc_cwd[] = "-I.";
  static char arg_e[] = "-e";
  static char code[512];
  char* argv[] = {arg0, inc_moddir, inc_cwd, arg_e, code, NULL};
  int argc = 5;

  if (!sys_initialized) {
    static char* sys_argv_storage[] = {arg0, NULL};
    char** sys_argv = sys_argv_storage;
    int sys_argc = 1;
    char** env = NULL;
    PERL_SYS_INIT3(&sys_argc, &sys_argv, &env);
    sys_initialized = 1;
  }

  /* Autoflush STDOUT so the module's print output isn't held back. */
  snprintf(code, sizeof(code), "BEGIN { $| = 1 } use %s;", module);

  if (!(my_perl = perl_alloc())) {
    snprintf(errbuf, errlen, "perl_alloc() error: (no memory!)");
    return -1;
  }
  PERL_SET_CONTEXT(my_perl);
  perl_construct(my_perl);
  PL_exit_flags |= PERL_EXIT_DESTRUCT_END;

  if (perl_parse(my_perl, xs_init, argc, argv, NULL) != 0) {
    snprintf(errbuf, errlen, "perl_parse() error: could not load module %s", module);
    ices_perl_stop();
    return -1;
  }
  if (perl_run(my_perl) != 0) {
    snprintf(errbuf, errlen, "perl_run() error in module %s", module);
    ices_perl_stop();
    return -1;
  }
  return 0;
}

void ices_perl_stop(void) {
  if (my_perl) {
    PERL_SET_CONTEXT(my_perl);
    perl_destruct(my_perl);
    perl_free(my_perl);
    my_perl = NULL;
  }
}

int ices_perl_has_sub(const char* name) {
  if (!my_perl) return 0;
  PERL_SET_CONTEXT(my_perl);
  return get_cv(name, 0) != NULL;
}

char* ices_perl_call(const char* name, int* died, char* errbuf, size_t errlen) {
  char* result = NULL;
  int count;
  *died = 0;
  if (!my_perl) return NULL;
  PERL_SET_CONTEXT(my_perl);
  {
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    PUTBACK;

    count = call_pv(name, G_SCALAR | G_EVAL | G_NOARGS);

    SPAGAIN;
    if (SvTRUE(ERRSV)) {
      *died = 1;
      snprintf(errbuf, errlen, "%s", SvPV_nolen(ERRSV));
      if (count) (void)POPs;
    } else if (count) {
      SV* sv = POPs;
      if (SvOK(sv)) result = strdup(SvPV_nolen(sv));
    }
    PUTBACK;
    FREETMPS;
    LEAVE;
  }
  return result;
}

void ices_perl_term(void) {
  ices_perl_stop();
  if (sys_initialized) {
    PERL_SYS_TERM();
    sys_initialized = 0;
  }
}
