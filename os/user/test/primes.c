#include "types.h"
#include "stat.h"
#include "user/user.h"
#include "fs.h"

void seive(int left_read_fd) {
    int prime;

    if (read(left_read_fd, &prime, sizeof(prime)) != sizeof(prime)) {
        close(left_read_fd);
        exit(1);
    }

    printf("primes %d\n", prime);
    int rightfd[2];
    pipe(rightfd);

    int pid = fork();
    if (pid < 0) {
        close(left_read_fd);
        close(rightfd[0]);
        close(rightfd[1]);
        exit(1);
    } else if (pid == 0) {
        close(left_read_fd);
        close(rightfd[1]);
        seive(rightfd[0]);
    } else {
        int num;
        close(rightfd[0]);
        while(read(left_read_fd, &num, sizeof(num)) == sizeof(num)) {
            if (write(rightfd[1], &num, sizeof(num)) != sizeof(num)) {
                close(left_read_fd);
                close(rightfd[1]);
                exit(1);
            }
        }

        close(left_read_fd);
        close(rightfd[1]);
        wait(0);
        exit(0);
    }
}

int main(char argc, char **argv)
{
    int pipefd[2];
    pipe(pipefd);

    int pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        exit(1);
    } else if (pid == 0) {
        close(pipefd[1]);
        seive(pipefd[0]);
    } else {
        close(pipefd[0]);
        for (int i = 2; i <= 35; i ++) {
            if (write(pipefd[1], &i, sizeof(int)) != sizeof(int)) {
                close(pipefd[1]);
                exit(1);
            }
        }
        
        close(pipefd[1]);
        wait(0);
    }

    exit(0);
}