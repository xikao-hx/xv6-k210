#ifdef LAB_FS
#define NPROC        10  // maximum number of processes
#else
#define NPROC        64  // maximum number of processes (speedsup bigfile)
#endif
#define NCPU          2  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE       200000  // size of file system in blocks
#define MAXPATH      128   // maximum file path name
// Timer interrupt interval in `time`-CSR cycles -> 5ms/tick (200Hz).
// The old value (390000000/200) assumed a 390MHz time base, but K210 real
// hardware measured ~125ms/tick (15.6MHz time base; 2026-08-14 burn log:
// 1631 ticks over 203.89s host wall clock), 25x slower than intended.
// Confirmed on hardware 2026-08-14: /timerfreq 100 -> 5.03ms/tick, and a
// 1.5M burn measured 69.87s / 13894 ticks = 5.03ms (mtime ~15.5MHz).
#ifdef QEMU
#define INTERVAL     (10000000 / 200) // QEMU virt: 10MHz time base -> 5ms/tick
#else
#define INTERVAL     (15600000 / 200) // K210: measured ~15.5MHz time base
#endif