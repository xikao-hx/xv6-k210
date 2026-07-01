#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"

int main(char argc, char **argv) 
{
    uint64 time = 0;
    time = uptime();

    printf("system run time: %ds\n", time);

    exit(0);
}