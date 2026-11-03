/* C interface to the embedded Perl interpreter (Perl's headers are C only). */
#ifndef ICES_PERL_GLUE_H
#define ICES_PERL_GLUE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start an interpreter and "use <module>". Returns 0 on success; on failure
 * a message is written to errbuf. */
int ices_perl_start(const char* module, char* errbuf, size_t errlen);
/* Destroy the interpreter (safe to call when not started). */
void ices_perl_stop(void);
/* Whether a sub of this name exists in package main. */
int ices_perl_has_sub(const char* name);
/* Call a sub in scalar context. Returns a malloc()ed string (caller frees)
 * or NULL if it died or returned undef. *died is set when it died; the error
 * text is written to errbuf. */
char* ices_perl_call(const char* name, int* died, char* errbuf, size_t errlen);
/* Process-wide teardown; call once at exit. */
void ices_perl_term(void);

#ifdef __cplusplus
}
#endif

#endif
