#ifndef __CONSOLE_H
#define __CONSOLE_H

#define CONSOLE_IOCTL_FLUSH_INPUT 1

void            consoleinit(void);
void            consoleintr(int);
void            consputc(int);

#endif
