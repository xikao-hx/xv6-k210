#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

int main(char argc, char **argv) 
{
    uint64 time = 0;
    time = uptime();

    printf("system run time: %ds\n", time);

    exit(0);
}