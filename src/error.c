#include "kicli/error.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Simple thread-local via __thread (GCC/Clang) or __declspec(thread) (MSVC) */
#ifdef _MSC_VER
static __declspec(thread) char _last_error[512];
#else
static __thread char _last_error[512];
#endif

const char *kicli_last_error(void) { return _last_error; }

void kicli_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_last_error, sizeof(_last_error), fmt, ap);
    va_end(ap);
}
