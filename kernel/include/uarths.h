#ifndef __UARTHS_H
#define __UARTHS_H

#include "types.h"

typedef int (*uarths_rx_observer_t)(int);

#define UARTHS_RX_KEEP     0
#define UARTHS_RX_CONSUME  1

void            uarthsinit(void);
void            uarthsputc(int);
void            uarthsputc_sync(int);
int             uarths_write(const char*, int);
void            uarths_flush_tx(void);
int             uarthsgetc(void);
int             uarths_read(char*, int);
int             uarths_try_read(char*, int);
void            uarths_flush_rx(void);
void            uarths_get_rx_stats(uint32*);
void            uarths_set_baud(int);
void            uarths_wait_tx_idle(void);
void            uarths_get_baud_info(uint32*);
void            uarths_set_rx_observer(uarths_rx_observer_t);

#ifdef __cplusplus
}
#endif

#endif /* __UARTHS_H */
