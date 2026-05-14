#include "types.h"
#include "stat.h"
#include "user/user.h"
#include "fs.h"

#define MAX_LEN  50
#define MAX_ARGS 10

int
read_line(char *buf, int max_len) {
    int i = 0;
    char c;

    while(i < max_len - 1) {
        if (read(0, &c, 1) != 1) {
            if (i == 0) {  // EOF
                return -1;
            }
            break;  // 输入结束
        }
        
        if (c == '\n') {  // 一行结束
            break;
        }
        buf[i++] = c;
    }

    buf[i] = '\0';
    return i;
}

int 
split_line(char *buf, char **cmd, int max_argc) {
    char *p = buf;
    int i = 0;

    while(i < max_argc - 1 && *p != '\0') {
        // 去掉空格
        while(*p == ' ' && *p != '\0') {
            p ++;
        }

        // 结束查找
        if (*p == '\0') {
            break;
        }
        
        // 记录位置
        cmd[i ++] = p;

        // 找到这个字符的结束位置
        while(*p != ' ' && *p != '\0') {
            p ++;
        }

        *p++ = '\0';  // 这里要用p++，否则下一个单词的查找会存在问题
    }

    // 保证最后一个是0
    cmd[i] = 0;

    return i;
}

int main(char argc, char **argv)
{
    int cmd_argc = 0;
    char* cmd_argv[MAX_ARGS];
    char* line_argv[MAX_ARGS];
    char line_buf[MAX_LEN];

    // 得到xargc的参数
    if (argc < 3) {
        cmd_argv[0] = "echo";
    } else {
        for (int i = 1; i < argc; i ++) {
            cmd_argv[cmd_argc++] = argv[i];
        }
    }

    while(1) {
        // 得到标准输入
        int line_len = read_line(line_buf, MAX_LEN);
        if (line_len == -1) {
            exit(0);
        } else if (line_len == 0) {
            continue;
        }

        int target_argc = cmd_argc;
        int line_argc = split_line(line_buf, line_argv, MAX_ARGS - cmd_argc);
        for (int i = 0; i < line_argc; i ++) {
            cmd_argv[target_argc++] = line_argv[i];
        }

        int pid = fork();
        if (pid < 0) {
            printf("xargs fork() err!\n");
            exit(1);
        } else if (pid == 0) {
            exec(cmd_argv[0], cmd_argv);
            printf("xargs exec() err!");
            exit(1);
        } else {
            wait(0);
        }
    }

    exit(0);
}