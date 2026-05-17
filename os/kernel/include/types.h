typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;
typedef unsigned short wchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
typedef unsigned long uint64;
typedef unsigned long uintptr_t;

typedef uint64 pde_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef readb
#define readb(addr) (*(volatile uint8 *)(addr))
#endif
#ifndef readw
#define readw(addr) (*(volatile uint16 *)(addr))
#endif
#ifndef readd
#define readd(addr) (*(volatile uint32 *)(addr))
#endif
#ifndef readq
#define readq(addr) (*(volatile uint64 *)(addr))
#endif

#ifndef writeb
#define writeb(v, addr) { (*(volatile uint8 *)(addr)) = (v); }
#endif
#ifndef writew
#define writew(v, addr) { (*(volatile uint16 *)(addr)) = (v); }
#endif
#ifndef writed
#define writed(v, addr) { (*(volatile uint32 *)(addr)) = (v); }
#endif
#ifndef writeq
#define writeq(v, addr) { (*(volatile uint64 *)(addr)) = (v); }
#endif

#define NELEM(x) (sizeof(x)/sizeof((x)[0]))
