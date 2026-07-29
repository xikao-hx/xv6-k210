#include "types.h"
#include "dev.h"
#include "param.h"

struct device devices[NDEV];

int
device_register(int major, const char *name,
                const struct file_operations *ops) {
    
    if (major <= 0 || major >= NDEV || name == 0 || ops == 0)  
        return -1;
    
    if (devices[major].ops != 0)
        return -1;
    
    devices[major].name = name;
    devices[major].ops = ops;
    return 0;
}

struct device *
device_get(int major) {
    if (major <= 0 || major >= NDEV || devices[major].ops == 0)
        return 0;
    return &devices[major];
}