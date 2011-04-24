#include <stdbool.h>

#define PROGRAM_NAME "fls"

const char *program_name;
int verbose;
bool am_daemon;

void usage(int status);
void *xmalloc(size_t size);
char *xstrdup(char *str);
