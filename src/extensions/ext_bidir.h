/*
 * ext_bidir.h — Bidirectional subprocess communication + PATH lookup.
 */
#ifndef CBM_EXT_BIDIR_H
#define CBM_EXT_BIDIR_H

#include <stddef.h>

typedef struct cbm_bidir cbm_bidir_t;

enum { CBM_BIDIR_STDERR_DEVNULL = 0, CBM_BIDIR_STDERR_LOG = 1 };

cbm_bidir_t *cbm_bidir_spawn(const char *const *argv, int stderr_mode);
int cbm_bidir_read(cbm_bidir_t *b, void *buf, size_t buf_size);
int cbm_bidir_read_timeout(cbm_bidir_t *b, void *buf, size_t buf_size, int timeout_ms);
int cbm_bidir_write(cbm_bidir_t *b, const void *buf, size_t len);
int cbm_bidir_shutdown(cbm_bidir_t *b);
void cbm_bidir_free(cbm_bidir_t *b);
long cbm_bidir_pid(const cbm_bidir_t *b);

char *cbm_which(const char *name);

#endif /* CBM_EXT_BIDIR_H */
