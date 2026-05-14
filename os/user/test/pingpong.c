#include "types.h"
#include "stat.h"
#include "fcntl.h"
#include "user/user.h"

int main(int argc, char **argv) 
{
    int pipe1[2], pipe2[2];
    int p = 'A';
    int exit_status = 0;

    pipe(pipe1);
    pipe(pipe2);

    int pid = fork();
    if (pid < 0) {
        close(pipe1[0]);
        close(pipe1[1]);
        close(pipe2[0]);
        close(pipe2[1]);
        exit(1);
    } else if (pid == 0) {
        close(pipe1[1]);
        close(pipe2[0]);
        if (read(pipe1[0], &p, sizeof(p)) != sizeof(p)) {
            close(pipe1[0]);
            close(pipe2[1]);
            exit_status = 1;
        } else {
            printf("%d: receving ping\n", getpid());
        }

        if (write(pipe2[1], &p, sizeof(p)) != sizeof(p)) {
            close(pipe1[0]);
            close(pipe2[1]);
            exit_status = 1;
        }
        
        close(pipe1[0]);
        close(pipe2[1]);
        exit(exit_status);
    } else {
        close(pipe1[0]);
        close(pipe2[1]);

        if (write(pipe1[1], &p, sizeof(p)) != sizeof(p)) {
            close(pipe1[1]);
            close(pipe2[0]);
            exit_status = 1;
        }

        if (read(pipe2[0], &p, sizeof(p)) != sizeof(p)) {
            close(pipe1[1]);
            close(pipe2[0]);
            exit_status = 1;
        } else {
            printf("%d: receiving pong\n", getpid());
        }

        close(pipe1[1]);
        close(pipe2[0]);
        exit(exit_status);
    }
}