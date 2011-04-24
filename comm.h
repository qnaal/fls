#include <stdbool.h>

#define FILEPATH_MAX 2000
#define MSG_MAX 100
#define MSG_SUCCESS "okay"
#define MSG_ERROR "error"
#define MSG_ERR_STACK_EMPTY "file stack empty"
#define MSG_ERR_STACK_FULL "file stack full"
#define MSG_ERR_LENGTH "file path too long"
#define CMD_PUSH "push"
#define CMD_POP  "pop"
#define CMD_PEEK "peek"
#define CMD_PICK "pick"
#define CMD_SIZE "size"
#define CMD_STOP "stop"

const char *soc_path;
int soc_r(int s, char *buf, int blen);
void soc_w(int s, char *buf);
bool readwait(int s, float timeout);
bool read_status_okay(int s);
int client_connect();
