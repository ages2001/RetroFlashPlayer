/* fetch.h - URLs of loaded content: resolution against the loading movie
 * and the mapping of file: URLs to local paths. */
#ifndef FLASHNT_FETCH_H
#define FLASHNT_FETCH_H

#include "player.h"

const char *fetch_movie_url(SWFMovie *m);
void        fetch_url_resolve(const char *base, const char *rel, char *out, size_t n);
int         fetch_url_to_path(Player *p, const char *url, char *out, size_t n);
u8         *fetch_read_file(const char *path, size_t *len);


/* local storage (SharedObject): keys like "localhost/game.swf/save";
 * files under g_store_dir (NULL: kept in memory for the session) */
extern const char *g_store_dir;
int  store_get(const char *key, u8 **out, size_t *n);
int  store_put(const char *key, const u8 *d, size_t n);
void store_remove(const char *key);
int  plat_mkdir(const char *path);      /* platform layer: 1 when it exists afterwards */

#endif
