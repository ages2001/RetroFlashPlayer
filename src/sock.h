/* sock.h - TCP sockets for XMLSocket (and later flash.net.Socket).
 *
 * Connections run behind a small event queue: the player pumps the
 * backends once per frame and the script engine takes the events
 * (connected / data / closed) when it updates its sockets. */
#ifndef FLASHNT_SOCK_H
#define FLASHNT_SOCK_H

#include "player.h"

enum { SOCK_EV_CONNECT = 1, SOCK_EV_DATA, SOCK_EV_CLOSE };
typedef struct { int type, handle, ok; u8 *data; size_t n; } SockEvent;

int  sock_connect(Player *p, const char *host, int port, int timeout_ms);   /* handle (> 0) */
void sock_send(Player *p, int h, const u8 *d, size_t n);
void sock_close(Player *p, int h);               /* by the script: no event follows */
void sock_pump(Player *p);                       /* progress connections (end of a frame) */
int  sock_next_event(Player *p, SockEvent *ev);  /* caller frees ev->data */
void sock_free_all(Player *p);

/* platform TCP (plat_*.c); a stub that always fails is fine */
int  plat_tcp_connect(const char *host, int port);           /* < 0: failed */
int  plat_tcp_poll(int fd, int *connected, int *readable);   /* < 0: error */
int  plat_tcp_send(int fd, const u8 *d, size_t n);
int  plat_tcp_recv(int fd, u8 *d, size_t n);                 /* 0: closed, < 0: would block */
void plat_tcp_close(int fd);

#endif
