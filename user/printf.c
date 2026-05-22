#include "types.h"
#include "stat.h"
#include "user/user.h"

#include <stdarg.h>

static char digits[] = "0123456789ABCDEF";

/* Buffer context for snprintf */
typedef struct {
  char *buf;
  int   pos;
  int   size;  /* including space for '\0' */
} BufCtx;

static void
putc(int fd, char c)
{
  write(fd, &c, 1);
}

static void
buf_putc(BufCtx *ctx, char c)
{
  if (ctx->pos < ctx->size - 1)
    ctx->buf[ctx->pos] = c;
  ctx->pos++;  /* always advance so we can return the true length */
}

/* Output target: either fd (>=0) or buffer (fd == -1) */
typedef struct {
  int    fd;
  BufCtx *buf;
} OutCtx;

static void
out_putc(OutCtx *o, char c)
{
  if (o->fd >= 0)
    putc(o->fd, c);
  else
    buf_putc(o->buf, c);
}

static void
out_printint(OutCtx *o, int xx, int base, int sgn)
{
  char buf[16];
  int i, neg;
  uint x;

  neg = 0;
  if (sgn && xx < 0) {
    neg = 1;
    x = -xx;
  } else {
    x = xx;
  }

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);
  if (neg)
    buf[i++] = '-';

  while (--i >= 0)
    out_putc(o, buf[i]);
}

static void
out_printptr(OutCtx *o, uint64 x)
{
  int i;
  out_putc(o, '0');
  out_putc(o, 'x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    out_putc(o, digits[x >> (sizeof(uint64) * 8 - 4)]);
}

static void
out_vprintf(OutCtx *o, const char *fmt, va_list ap)
{
  char *s;
  int c, i, state;

  state = 0;
  for (i = 0; fmt[i]; i++) {
    c = fmt[i] & 0xff;
    if (state == 0) {
      if (c == '%')
        state = '%';
      else
        out_putc(o, c);
    } else if (state == '%') {
      if (c == 'd') {
        out_printint(o, va_arg(ap, int), 10, 1);
      } else if (c == 'l') {
        out_printint(o, va_arg(ap, uint64), 10, 0);
      } else if (c == 'x') {
        out_printint(o, va_arg(ap, int), 16, 0);
      } else if (c == 'p') {
        out_printptr(o, va_arg(ap, uint64));
      } else if (c == 's') {
        s = va_arg(ap, char*);
        if (s == 0)
          s = "(null)";
        while (*s != 0) {
          out_putc(o, *s);
          s++;
        }
      } else if (c == 'c') {
        out_putc(o, va_arg(ap, uint));
      } else if (c == '%') {
        out_putc(o, c);
      } else {
        out_putc(o, '%');
        out_putc(o, c);
      }
      state = 0;
    }
  }
}

void
vprintf(int fd, const char *fmt, va_list ap)
{
  OutCtx o;
  o.fd  = fd;
  o.buf = 0;
  out_vprintf(&o, fmt, ap);
}

void
fprintf(int fd, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(fd, fmt, ap);
}

void
printf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vprintf(1, fmt, ap);
}

/*
 * snprintf — write at most n bytes (including '\0') into buf.
 * Returns the number of characters that *would* have been written
 * if buf were large enough (C99 semantics), not counting '\0'.
 * If n == 0, buf may be NULL and nothing is written.
 */
int
vsnprintf(char *buf, int n, const char *fmt, va_list ap)
{
  BufCtx  bctx;
  OutCtx  o;

  bctx.buf  = buf;
  bctx.pos  = 0;
  bctx.size = n;

  o.fd  = -1;   /* signal: write to buffer, not fd */
  o.buf = &bctx;

  out_vprintf(&o, fmt, ap);

  /* NUL-terminate if there is any room at all */
  if (n > 0)
    buf[bctx.pos < n ? bctx.pos : n - 1] = '\0';

  return bctx.pos;   /* true length, excluding '\0' */
}

int
snprintf(char *buf, int n, const char *fmt, ...)
{
  va_list ap;
  int ret;

  va_start(ap, fmt);
  ret = vsnprintf(buf, n, fmt, ap);
  va_end(ap);

  return ret;
}
