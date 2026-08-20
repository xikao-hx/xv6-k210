#include "file.h"
#include "kalloc.h"
#include "printf.h"
#include "proc.h"
#include "vm.h"
#include "i2c_board.h"
#include "i2c.h"
#include "i2cdev.h"
#include "dev.h"

/* Validate message length against DMA page capacity */
#define MAX_MSG_LEN (PGSIZE / sizeof(uint32))

static int
i2cdev_open(struct file *f)
{
    struct i2cdev_data *i2cdev;

    if(f->minor < 0 || f->minor >= I2C_DEV_NR)
        return -1;

    i2cdev = kmalloc(sizeof(struct i2cdev_data));
    if(i2cdev == 0)
        return -1;

    initsleeplock(&i2cdev->lock, "i2c file");

    i2cdev->minor = f->minor;
    i2cdev->dev = i2c_device_get(f->minor);
    if(i2cdev->dev == 0) {
        kfree(i2cdev);
        return -1;
    }
    f->private_data = (void *)i2cdev;
    return 0;
}


static int
i2cdev_close(struct file *f)
{
   if(f->private_data) {
    kfree(f->private_data);
    f->private_data = 0;
  }

  return 0;
}

static int
i2cdev_ioctl(struct file *f, uint64 cmd, uint64 arg)
{
    struct proc *p = myproc();
    struct i2cdev_data *i2cdev = f->private_data;
    struct i2c_rdwr_ioctl_data rdwr;
    struct i2c_msg msgs[I2C_MAX_MSGS];
    uint8 *user_bufs[I2C_MAX_MSGS];
    int allocated = 0;
    int ret = -1;

    if(i2cdev == 0 || cmd != I2C_IOCTL_TRANSFER)
        return -1;

    struct i2c_device *dev = i2cdev->dev;
    // acquiresleep(&i2cdev->lock);
    if(copyin(p->pagetable, (char *)&rdwr, arg, sizeof(rdwr)) < 0 ||
        rdwr.nmsgs == 0 || rdwr.nmsgs > I2C_MAX_MSGS) {
        // releasesleep(&i2cdev->lock);
        return -1;
    }

    for(uint i = 0; i < rdwr.nmsgs; i++) {
        if(copyin(p->pagetable, (char *)&msgs[i],
                    (uint64)(rdwr.msgs + i), sizeof(msgs[i])) < 0)
            goto done;
        if(msgs[i].addr != dev->slave_address || (msgs[i].flags & ~I2C_M_RD) != 0 ||
            msgs[i].len == 0 || msgs[i].len > MAX_MSG_LEN)
            goto done;

        msgs[i].addr = dev->slave_address;
        user_bufs[i] = msgs[i].buf;
        msgs[i].buf = kmalloc(msgs[i].len);
        if(msgs[i].buf == 0)
            goto done;
        allocated++;
        if((msgs[i].flags & I2C_M_RD) == 0 &&
            copyin(p->pagetable, (char *)msgs[i].buf,
                    (uint64)user_bufs[i], msgs[i].len) < 0)
            goto done;
    }

    ret = i2c_transfer(dev, msgs, rdwr.nmsgs);
    if(ret < 0)
        goto done;
    for(uint i = 0; i < rdwr.nmsgs; i++) {
        if((msgs[i].flags & I2C_M_RD) &&
            copyout(p->pagetable, (uint64)user_bufs[i],
                    (char *)msgs[i].buf, msgs[i].len) < 0) {
            ret = -1;
            break;
        }
    }

done:
    for(int i = 0; i < allocated; i++)
        kfree(msgs[i].buf);

    // releasesleep(&i2cdev->lock);
    return ret;
}

static const struct file_operations i2cdev_ops = {
    .open = i2cdev_open,
    .close = i2cdev_close,
    .ioctl = i2cdev_ioctl,
};

void
i2cdev_init(void)
{
    i2c_init();
    if(device_register(DEV_I2C, "i2c", &i2cdev_ops) < 0)
        panic("i2c device register");
}
