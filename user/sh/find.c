#include "types.h"
#include "stat.h"
#include "user.h"

void
find(char *path, char *filename)
{
  char buf[512], *p;
  int fd;
  struct stat st;

  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  if(st.type == T_FILE){
    fprintf(2, "find: %s is not a directory\n", path);
    close(fd);
    return;
  }

  while(readdir(fd, &st) == 1){
    if(strcmp(st.name, ".") == 0 || strcmp(st.name, "..") == 0)
      continue;

    if(strlen(path) + 1 + strlen(st.name) + 1 > sizeof(buf)){
      fprintf(2, "find: path too long\n");
      continue;
    }
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';
    strcpy(p, st.name);

    if(st.type == T_DIR)
      find(buf, filename);
    else if(strcmp(st.name, filename) == 0)
      printf("%s\n", buf);
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc != 3){
    fprintf(2, "Usage: find <path> <filename>\n");
    exit(1);
  }

  find(argv[1], argv[2]);
  exit(0);
}
