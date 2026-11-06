/* lc_hub.h - LocalConnection registry shared by AVM1 (vmk 0) and AVM2
 * (vmk 1).  Message arguments travel as AMF0 values (one buffer each). */
#ifndef FLASHNT_LC_HUB_H
#define FLASHNT_LC_HUB_H

#include "flashnt.h"
#include <stddef.h>

typedef struct {
    void *ctx;
    void (*status)(void *ctx, void *src, const char *level);
    void (*call)(void *ctx, void *obj, const char *method, int nargs, u8 **args, size_t *argn);
} LCHandler;

void  lch_set_handler(int vmk, const LCHandler *h);
void  lch_superdomain(const char *url, char *out, size_t n);
char *lch_key(const char *superdomain, const char *name, int for_send);   /* malloc'd */
int   lch_connect(const char *key, int vmk, void *obj);                    /* 0: name taken */
void  lch_close(const char *key);
/* takes ownership of args / argn (malloc'd arrays of malloc'd buffers) */
void  lch_send(int vmk, void *src, const char *conn_key, const char *method, int nargs, u8 **args, size_t *argn);
void  lch_deliver(void);
void  lch_mark(int vmk, void (*mark)(void *ctx, void *obj), void *ctx);
void  lch_forget_vm(int vmk);

#endif
