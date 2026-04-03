#pragma once
#ifndef KICLI_ERROR_H
#define KICLI_ERROR_H

typedef enum {
    KICLI_OK = 0,
    KICLI_ERR_IO,
    KICLI_ERR_HTTP,
    KICLI_ERR_PARSE,
    KICLI_ERR_NOT_FOUND,
    KICLI_ERR_ALREADY_EXISTS,
    KICLI_ERR_INVALID_ARG,
    KICLI_ERR_UNSUPPORTED,
    KICLI_ERR_OOM,
    KICLI_ERR_SUBPROCESS,
    KICLI_ERR_NOT_IMPLEMENTED,
} kicli_err_t;

/* Thread-local last error message (max 512 chars) */
const char *kicli_last_error(void);
void        kicli_set_error(const char *fmt, ...);

#define KICLI_FAIL(code, ...) do { kicli_set_error(__VA_ARGS__); return (code); } while(0)

#endif /* KICLI_ERROR_H */
