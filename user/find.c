#include "types.h"
#include "stat.h"
#include "user/user.h"
#include "fs.h"

extern int match(char*, char*);

void
find(char *path, char *filename)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if((fd = open(path, 0)) < 0){
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_FILE:
    printf("Usage: find <path> <filename>\n");
    break;

  case T_DIR:
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("ls: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    *p++ = '/';
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      
      if (st.type == T_DIR && strcmp(p, ".") != 0 && strcmp(p, "..") != 0) {
        find(path, filename);
      } else if (strcmp(filename, p) == 0){
        printf("%s\n", buf);
        // if (match(filename, p)) {
        //   printf("%s\n", buf);
        // }
      }
    }
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc != 3){
    printf("Usage: find <path> <filename>\n");
    exit(1);
  }

  find(argv[1], argv[2]);
  exit(0);
}
