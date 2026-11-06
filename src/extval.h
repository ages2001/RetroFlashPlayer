/* extval.h - ExternalInterface values (VM independent).
 *
 * The container (a browser page, or the test harness) exchanges these with
 * the movie.  Object keys are kept sorted, like Ruffle's BTreeMap. */
#ifndef FLASHNT_EXTVAL_H
#define FLASHNT_EXTVAL_H

#include "flashnt.h"

enum { XV_UNDEF = 0, XV_NULL, XV_BOOL, XV_NUM, XV_STR, XV_LIST, XV_OBJ };

typedef struct ExtVal {
    int    t;
    int    b;
    double n;
    char  *s;                   /* XV_STR: UTF-8 */
    int    count;               /* XV_LIST / XV_OBJ */
    struct ExtVal *items;
    char **keys;                /* XV_OBJ */
} ExtVal;

void   xv_free(ExtVal *v);                      /* frees contents, not v */
ExtVal xv_undef(void);
ExtVal xv_null(void);
ExtVal xv_bool(int b);
ExtVal xv_num(double n);
ExtVal xv_str(const char *s);
ExtVal xv_copy(const ExtVal *v);
void   xv_list_push(ExtVal *list, ExtVal v);    /* list takes ownership of v */
void   xv_obj_put(ExtVal *obj, const char *key, ExtVal v);   /* sorted insert, replaces */
/* Rust Debug formatting (String("x"), Number(1.0), Object({...}), ...); malloc'd */
char  *xv_debug(const ExtVal *v);

/* the container: NULL when the movie runs standalone (ExternalInterface.available is false) */
struct Player;
typedef int (*ExtProviderFn)(struct Player *p, const char *name, int argc, const ExtVal *args, ExtVal *out);
extern ExtProviderFn g_ext_provider;

#endif
