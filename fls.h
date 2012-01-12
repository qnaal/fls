#include <stdbool.h>

#define PROGRAM_NAME "fls"
#define COLR_CLR "\033[0m"
#define COLR_PATH "\033[1;34m" /* light blue */
#define COLR_WARN "\033[31m"   /* red */

const char *program_name;
int verbose;
bool am_daemon;


void usage(int status);
char* color_string(char *color,char *string);
void *xmalloc(size_t size);
char *xstrdup(char *str);
