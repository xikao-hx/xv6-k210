#include "types.h"
#include "stat.h"
#include "fcntl.h"
#include "user/user.h"

int main(int argc, char **argv) 
{
    if (argc != 2) {
        printf("Usage: sleep <time>\n");
        exit(1);
    }
    
    sleep(atoi(argv[1]));
    exit(0);
}