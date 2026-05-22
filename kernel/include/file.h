struct file {
  enum { FD_NONE, FD_PIPE, FD_ENTRY, FD_DEVICE } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe; // FD_PIPE
  struct dirent *ep; // FD_ENTRY
  uint off;          // FD_ENTRY
  short major;       // FD_DEVICE
  short minor;       // FD_DEVICE
};

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
  int (*ioctl)(int, uint64, uint64); // minor, cmd, arg
};

extern struct devsw devsw[];

int             dirnext(struct file *f, uint64 addr);

#define CONSOLE 1
#define STATS   2
#define SPI_DEV 3
#define I2C_DEV 4
#define SDCARD_DEV 5
#define UART_DEV 6