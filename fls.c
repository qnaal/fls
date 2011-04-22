#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <libgen.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>

#define PROGRAM_NAME "fls"
#define FILEPATH_MAX 2000
#define STACK_MAX 100
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
#define EXEC_ARG_MAX 6
#define PLURALS(int) (int == 1 ? "" : "s")

typedef struct StackNode {
  char *dat;
  struct StackNode *next;
} Node;

struct Action {
  enum ActionType {
    NOTHING,
    PUSH,
    DROP,
    PRINT,
    COPY,
    MOVE,
    SYMLINK,
    INTERACTIVE,
    STOP,
  } type;
  int num;
  void *ptr;
};

struct ActionDef {
  enum ActionType type;
  char *verb;
  char *exargv[EXEC_ARG_MAX];
  int source_slot, dest_slot;
};

struct ActionDef actions[] = {
  {PUSH,        "push",  {NULL}, 0, 0},
  {DROP,        "drop",  {NULL}, 0, 0},
  {PRINT,       "print", {NULL}, 0, 0},
  {COPY,        "copy",    {"/bin/cp", "-r", "--", NULL, NULL, NULL}, 3, 4},
  {MOVE,        "move",    {"/bin/mv", "--", NULL, NULL, NULL},       2, 3},
  {SYMLINK,     "symlink", {"/bin/ln", "-s", "--", NULL, NULL, NULL}, 3, 4},
  {INTERACTIVE, "interactive mode", {NULL}, 0, 0},
  {STOP,        "terminate daemon", {NULL}, 0, 0},
  {NOTHING}
};
const char *program_name;
const char *soc_path;
int verbose=0;
bool am_daemon=false;

/*
 * General functions
 */

void usage(int status) {
  /* Tell the user how to do better, and exit with <status>. */
  if( status != EXIT_SUCCESS ) {
    fprintf(stderr, "Try `%s -h' for more information.\n", program_name);
  } else {
    printf("\
Usage: %s <ACTION> [OPTION...] [DEST]\n\
  or:  %s [FILE]...\n\
", program_name, program_name);
    printf("\
Push FILEs onto the stack, or perform action ACTION.\n\
");
    printf("\
\n\
Actions:\n\
  -c    COPY\n\
          pop a file from the stack, copy it to DEST or current dir\n\
  -m    MOVE\n\
          pop a file from the stack, move it to DEST or current dir\n\
  -s    SYMLINK\n\
          pop a file from the stack, symlink it to DEST or current dir\n\
  -d    DROP\n\
          pop a file from the stack, print its name\n\
");
    printf("\
  -p    PRINT\n\
          print the contents of the stack\n\
  -q    QUIT\n\
          terminate the stack daemon, losing the contents of the stack\n\
  -h    HELP\n\
          display usage information, and then exit\n\
");
    printf("\
\n\
Options:\n\
  -n N  (available for COPY, MOVE, SYMLINK, and DROP)\n\
          perform action to the top N files on the stack\n\
");
    printf("\
\n\
If no args are provided, the default action is PRINT.\n\
\n\
If FILEs are provided, push them onto the stack.\n\
");
  }
  exit(status);
}

void *xmalloc(size_t size) {
  /* Loudly fail on memory allocation error. */
  void *ptr;

  ptr = malloc(size);
  if( ptr == NULL ) {
    fprintf(stderr, "malloc failed\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

char *xstrdup(char *str) {
  /* Loudly fail on memory allocation error. */
  char *ptr;

  ptr = strdup(str);
  if( ptr == NULL ) {
    fprintf(stderr, "strdup failed\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

void log_output() {
  /* Redirect stdout and stderr to `<soc_path>.log'. */
  char *prefix, logfile[FILEPATH_MAX];
  sprintf(logfile, "%s.log", soc_path);

  if( am_daemon )
    prefix = "daemon: log_output";
  else
    prefix = "log_output";

  if( freopen(logfile, "a", stdout) == NULL ) {
    fprintf(stderr, "%s: could not redirect stdout\n", prefix);
    exit(EXIT_FAILURE);
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  stderr = stdout;
}

bool isdir(char *path) {
  /* Return whether <path> is a directory. */
  struct stat st;
  if( stat(path, &st) == -1 ) {
    if( errno != EFAULT ) {
      /* terminate for any error other than "file doesn't exist" */
      perror("stat");
      exit(EXIT_FAILURE);
    }
  } else if( S_ISDIR(st.st_mode) )
    return true;
  return false;
}

/*
 * Socket functions
 */

void set_program_name(const char *argv0) {
  /* Set program_name to argv0. */

  program_name = argv0;
}

void genset_soc_path() {
  /* Generate soc_path from user name, and set it. */
  char *generated, *username, *tmpdir="/tmp/";
  int size;

  username = getenv("USER");

  size = strlen(tmpdir) + strlen(username) + strlen(PROGRAM_NAME) +1;
  generated = xmalloc(size);
  sprintf(generated, "%s%s%s", tmpdir, username, PROGRAM_NAME);
  soc_path = generated;
}

int soc_r(int s, char *buf, int blen) {
  /* Read a string to <buf> from <s>.
     Return the number of bytes read (including null) on success,
     0 on broken socket, or -1 on general failure. */
  char *last_read, *prefix;
  bool garbage=false;
  int n;

  if( am_daemon )
    prefix = "daemon: revc";
  else
    prefix = "recv";

  if( blen < 1 ) {
    fprintf(stderr, "%s: unacceptable buffer size of %d\n", prefix, blen);
    return -1;
  }
  last_read = buf;
  n = recv(s, last_read, 1, 0);

  while(1) {

    /* check error */
    if( n <= 0 ) {
      if( n == -1 ) {
	if( errno == EAGAIN || errno == EWOULDBLOCK ) {
	  /* end of sent */
	  fprintf(stderr,"%s didn't get full string (%d bytes, no null)\n", prefix, (int)(last_read - buf));
	  *last_read = 0;
	  fprintf(stderr,"%s partial message: `%s'\n", prefix, buf);
	} else
	  perror(prefix);
      }
      return n;
    }

    /* check null */
    if( *last_read == 0 ) {
      if( verbose > 1 )
	printf("%s read null\n", prefix);
      break;
    } else {
      if( verbose > 1 )
	printf("%s read char %#04x `%c'; %d in string\n", prefix, *last_read, *last_read, (int)(last_read - buf) +1);
    }

    /* check buffer length before reading char */
    if( ++last_read - buf >= blen ) {
      /* clear the stream buffer til the next null, what's left can only possibly be garbage */
      if( !garbage ) {
	garbage = true;
	fprintf(stderr,"%s filled buffer before getting full string (%d bytes, no null)\n", prefix, (int)(last_read - buf));
	*--last_read = 0;
	fprintf(stderr,"%s first %d bytes of lost message: `%s'\n", prefix, (int)(last_read - buf), buf);
      } else
	fprintf(stderr,"%s flopped around a bit while cleaning read buffer\n", prefix);
      last_read = buf;
    }
    n = recv(s, last_read, 1, MSG_DONTWAIT);
  }

  if( garbage )
    return -1;

  if( verbose )
    printf("%s `%s'\n", prefix, buf);
  /* +1 to count last_read */
  return last_read +1 - buf;
}

void soc_w(int s, char *buf) {
  /* Quick and dirty write string to socket -
     expect most error handling to happen on the other side. */
  char *prefix;
  int len, n;

  if( am_daemon )
    prefix = "daemon: send";
  else
    prefix = "send";
  if( verbose )
    printf("%sing `%s'\n", prefix, buf);
  /* add one for the null */
  len = strlen(buf) +1;
  n = write(s, buf, len);
  if( n != len ) {
    if( n == -1 ) {
      perror(prefix);
    } else
      /* FIXME: should resend the rest */
      fprintf(stderr, "%s short by %d bytes\n", prefix, len - n);
  }
}

bool readwait(int s, float timeout) {
  /* Return whether socket <s> is ready for reading, after waiting
     up to <timeout> seconds for that to become true. */
  struct timeval tv;
  fd_set readfds;

  tv.tv_sec = (int)timeout;
  tv.tv_usec = (timeout - tv.tv_sec) * 1000000;

  FD_ZERO(&readfds);
  FD_SET(s, &readfds);

  if( select(s+1, &readfds, NULL, NULL, &tv) )
    return true;
  return false;
}

bool read_status_okay(int s) {
  /* Read status message from socket <s>.
     Return true if successfully read MSG_SUCCESS,
     false otherwise. */
  char buf[MSG_MAX];
  if( soc_r(s, buf, MSG_MAX) > 0 && strcmp(buf, MSG_SUCCESS) == 0 )
    return true;
  return false;
}

/*
 * Signal functions
 */

void sig_block(int signum) {
  /* Block signal <signum>. */
  sigset_t block_mask;

  sigemptyset(&block_mask);
  sigaddset(&block_mask, signum);
  sigprocmask(SIG_BLOCK, &block_mask, NULL);
}

bool sig_catch(float timeout) {
  /* Wait up to <timeout> seconds to receive a signal (including blocked/pending).
     Return whether we did. */
  struct timespec ts;
  sigset_t all_sigs;

  ts.tv_sec = (int)timeout;
  ts.tv_nsec = (timeout - ts.tv_sec) * 1000000000;
  sigfillset(&all_sigs);
  if( sigtimedwait(&all_sigs, NULL, &ts) > 0 )
    return true;
  return false;
}

void sig_ignore(int signum) {
  /* Ignore any received <signum>. */
  struct sigaction sa_ign;

  sa_ign.sa_handler = SIG_IGN;
  sa_ign.sa_flags = 0;
  sigemptyset(&sa_ign.sa_mask);

  if( sigaction(signum, &sa_ign, NULL) ) {
    perror("sig_ignore: sigaction");
    exit(EXIT_FAILURE);
  }
}

/*
 * Client functions
 */

char* abs_path(char *relpath) {
  /* Return the absolute path to file <relpath>, with a slash at the end
     if it is a dir.
     Return NULL if the file at <relpath> does not exist.
     Terminate if any other component of the path does not exist. */
  char* path;

  path = realpath(relpath, NULL);
  if( path == NULL ) {
    if( errno != ENOENT ) {
      perror("realpath");
      exit(EXIT_FAILURE);
    }
    return NULL;
  }
  if( isdir(path) && strcmp(path, "/") != 0 ) {
    path = realloc(path, strlen(path) +2);
    if( path == NULL ) {
      fprintf(stderr, "realloc failed\n");
      exit(EXIT_FAILURE);
    }
    memcpy(&(path[strlen(path)]), "/", 2);
  }
  return path;
}

struct ActionDef *action_def(enum ActionType type) {
  /* Return a pointer to the ActionDef matching <type>, or NULL if there
     was no match. */
  struct ActionDef *def=actions;

  while( def->verb != NULL && def->type != type ) {
    def++;
  }
  if( def->verb == NULL )
    return NULL;
  return def;
}

char *action_verb(enum ActionType type) {
  /* Return the string associated with <type>. */
  struct ActionDef *def=action_def(type);

  if( def == NULL ) {
    fprintf(stderr, "action_verb: failure to find verb for ActionType %d\n", type);
    exit(EXIT_FAILURE);
  }
  return def->verb;
}

void action_set(struct Action *action, enum ActionType type) {
  /* Set <action> to <type>, complaining if it has been set before. */

  if( action->type == NOTHING )
    action->type = type;
  else {
    fprintf(stderr, "%s: cannot perform two actions (%s, %s)\n",
	    program_name, action_verb(action->type), action_verb(type));
    usage(EXIT_FAILURE);
  }
}

struct Action handle_options(int argc, char **exargv) {
  /* Return the proper action to take. */
  struct Action action = {NOTHING, 1, NULL};
  int c;

  while( (c = getopt(argc, exargv, "cmsdpiqvn:h")) != -1 ) {
    switch (c) {
    case 'c':
      action_set(&action, COPY);
      break;
    case 'm':
      action_set(&action, MOVE);
      break;
    case 's':
      action_set(&action, SYMLINK);
      break;
    case 'd':
      action_set(&action, DROP);
      break;
    case 'p':
      action_set(&action, PRINT);
      break;
    case 'i':
      action_set(&action, INTERACTIVE);
      break;
    case 'q':
      action_set(&action, STOP);
      break;
    case 'v':
      verbose++;
      break;
    case 'n':
      action.num = atoi(optarg);
      if( action.num <= 0 ) {
	fprintf(stderr, "invalid argument `%s' for option `%s'\n", optarg, exargv[optind]);
	usage(EXIT_FAILURE);
      }
      break;
    case 'h':
      usage(EXIT_SUCCESS);
    case '?':
      usage(EXIT_FAILURE);
    }
  }
  if( optind < argc ) {
    if( verbose )
      printf("arg provided\n");
    switch (action.type) {
    case NOTHING:
      action_set(&action, PUSH);
      action.num = argc - optind;
      action.ptr = &exargv[optind];
      break;
    case COPY:
    case MOVE:
    case SYMLINK:
      if( (optind +1) < argc ) {
	fprintf(stderr, "Too many supplied arguments for requested action: `%s'\n",
		action_verb(action.type));
	usage(EXIT_FAILURE);
      }
      action.ptr = exargv[optind];
      break;
    default:
      fprintf(stderr, "Requested action `%s' does not take arguments\n",
	      action_verb(action.type));
      usage(EXIT_FAILURE);
    }
  }
  return action;
}

int client_connect() {
  /* Return a socket that is connected to the daemon. */
  struct sockaddr_un sockaddr;
  int s, len;

  s = socket(AF_UNIX,SOCK_STREAM, 0);
  if( s == -1 ) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  if( verbose )
    printf("Trying to connect...\n");
  sockaddr.sun_family = AF_UNIX;
  strcpy(sockaddr.sun_path, soc_path);
  len = strlen(sockaddr.sun_path) + sizeof(sockaddr.sun_family);
  if( connect(s, (struct sockaddr*)&sockaddr, len) == -1 ) {
    perror("connect");
    exit(EXIT_FAILURE);
  }
  if( verbose )
    printf("Connected.\n");
  return s;
}

void push(int s, char *file) {
  /* Instruct daemon to push <file> onto the stack.
     Terminate on error. */
  char buf[FILEPATH_MAX], *fullpath, *prefix="push:";
  int okay;

  soc_w(s, CMD_PUSH);
  okay = read_status_okay(s);
  if( !okay ) {
    if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
      fprintf(stderr, "%s quitting for read error\n", prefix);
      exit(EXIT_FAILURE);
    }
    printf("Could not push; received error: `%s'\n", buf);
    exit(EXIT_FAILURE);
  }

  fullpath = abs_path(file);
  if( fullpath == NULL ) {
    fprintf(stderr, "%s: file `%s' does not exist\n", program_name, file);
    exit(EXIT_FAILURE);
  }
  soc_w(s, fullpath);

  okay = read_status_okay(s);
  if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
    fprintf(stderr, "%s quitting for read error\n", prefix);
    exit(EXIT_FAILURE);
  }
  if( !okay ) {
    printf("received error `%s' (stack state debatable)\n", buf);
  } else {
    if( strcmp(buf, fullpath) != 0 ) {
      fprintf(stderr, "%s error: path sent not the same as path pushed\n", prefix);
      exit(EXIT_FAILURE);
    }
    printf("Pushed `%s'\n", fullpath);
  }
  free(fullpath);
}

bool drop(int s) {
  /* Instruct daemon to pop a file from the stack.
     Return whether it could. */
  char buf[FILEPATH_MAX], *prefix="drop:";
  int okay;

  soc_w(s, CMD_POP);
  okay = read_status_okay(s);
  if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
    fprintf(stderr, "%s quitting for read error\n", prefix);
    exit(EXIT_FAILURE);
  }
  if( okay ) {
    printf("%s\n", buf);
    return true;
  } else {
    fprintf(stderr, "error: `%s'\n", buf);
    return false;
  }
}

void multidrop(int s, int num) {
  /* Drop <num> files from stack. */
  char buf[MSG_MAX];
  int i, instack;

  soc_w(s, CMD_SIZE);
  soc_r(s, buf, MSG_MAX);
  instack = atoi(buf);
  if( num > instack ) {
    if( instack == 0 )
      fprintf(stderr, "%s: cannot pop, file stack empty\n", program_name);
    else
      fprintf(stderr, "%s: asked to drop %d file%s, only %d in stack\n",
	      program_name, num, PLURALS(num), instack);
    exit(EXIT_FAILURE);
  }
  for( i = 0; i < num; i++ ) {
    if( !drop(s) ) {
      printf("popped %d file%s\n", i, PLURALS(i));
      exit(EXIT_FAILURE);
    }
  }
}

void print(int s) {
  /* Print the contents of the stack for the user. */
  char buf[FILEPATH_MAX];
  int i, stack_size;

  soc_w(s, CMD_SIZE);
  soc_r(s, buf, MSG_MAX);
  stack_size = atoi(buf);
  printf("%d file%s in stack\n", stack_size, PLURALS(stack_size));
  for( i = 0; i < stack_size; i++ ) {
    soc_w(s, CMD_PICK);
    sprintf(buf, "%d", i);
    soc_w(s, buf);
    if( !read_status_okay(s) ) {
      soc_r(s, buf, FILEPATH_MAX);
      printf("error: `%s'\n", buf);
      exit(EXIT_FAILURE);
    }
    soc_r(s, buf, FILEPATH_MAX);
    printf("%d: %s\n", i, buf);
  }
}

void stop_daemon(int s) {
  /* Stop the daemon process.
     Ask the user first, if the stack isn't empty. */
  char buf[MSG_MAX];

  soc_w(s, CMD_SIZE);
  soc_r(s, buf, MSG_MAX);
  if( atoi(buf) > 0 ) {
    printf("Stack not empty, still stop daemon [Yn]?");
    if( fgets(buf, MSG_MAX, stdin) == NULL ) {
      printf("error reading from stdin\n");
      exit(EXIT_FAILURE);
    }
    switch (buf[0]) {
    case '\n':
    case 'Y': case 'y':
      break;
    default:
      printf("Canceled by user\n");
      exit(EXIT_FAILURE);
      break;
    }
  }
  soc_w(s, CMD_STOP);
  if( read_status_okay(s) )
    printf("Server shutting down.\n");
  else
    printf("It doesn't want to.\n");
}

char **cmd_gen(struct Action action, char *source, char *dest) {
  /* Return the shell command (in the form of a null-terminated argv)
     that would perform <action> between <source> and <dest>. */
  char **exargv, *prefix="cmd_gen:";
  struct ActionDef *def;
  int i;

  def = action_def(action.type);
  if( def == NULL ) {
    fprintf(stderr, "%s error: unsupported action\n", prefix);
    exit(EXIT_FAILURE);
  }

  exargv = xmalloc(sizeof(char*) * EXEC_ARG_MAX);
  for( i = 0; i < EXEC_ARG_MAX; i++ ) {
    exargv[i] = def->exargv[i];
  }
  exargv[def->source_slot] = source;
  exargv[def->dest_slot] = dest;
  return exargv;
}

int action_exec(char **exargv) {
  /* Fork an exec of the null-terminated argv <exargv>.
     Return the exit status, or -1 on other error. */
  char *prefix="action_exec:";
  int exit_status;
  pid_t pid;

  pid = fork();
  if( pid == 0 ) {
    if( execv(exargv[0], exargv) == -1 ) {
      perror("execv");
      return -1;
    }
  } else if( pid < 0 ) {
    fprintf(stderr,"%s error: couldn't fork\n", prefix);
    return -1;
  } else {
    pid_t ws = waitpid(pid, &exit_status, 0);
    if( ws == -1 ) {
      perror("wait");
      return -1;
    }

    if( WIFEXITED(exit_status) ) {
      int status = WEXITSTATUS(exit_status);
      if( status != EXIT_SUCCESS ) {
	fprintf(stderr,"%s %s exited with status=%d\n", prefix, exargv[0], status);
      }
      return status;
    } else if( WIFSIGNALED(exit_status) ) {
      fprintf(stderr,"%s error: process killed\n", prefix);
      return -1;
    }
  }
  /* previous if()s should be collectively exhaustive */
  fprintf(stderr,"%s this should not happen\n", prefix);
  return -1;
}

char *real_target(char *reltarget) {
  /* Return the canonicalized absolute path to <reltarget>
     (or current working dir, if <reltarget> is NULL).
     Terminate if any component of the path but the last doesn't exist. */
  char *target;

  target = abs_path((reltarget == NULL ? "." : reltarget));
  if( target == NULL ) {
    char *dir, *base, *givendir, *givenbase;

    /* make copies of everything because I can't trust
       dirname and basename to do anything right */
    givendir = xstrdup(reltarget);
    dir = abs_path(dirname(givendir));
    if( dir == NULL ) {
      fprintf(stderr, "%s: target directory `%s' does not exist\n",
	      program_name, dirname(reltarget));
      exit(EXIT_FAILURE);
    }

    givenbase = xstrdup(reltarget);
    base = basename(givenbase);

    if( strcmp(dir, "/") == 0 ) {
      target = xmalloc(1+ strlen(base) +1);
      sprintf(target, "/%s", base);
    } else {
      target = xmalloc(strlen(dir) + strlen(base) +1);
      sprintf(target, "%s%s", dir, base);
    }
    free(givendir);
    free(dir);
    free(givenbase);
  }
  return target;
}

bool cmd_report(struct Action action, char *source, char *dest, bool interactive) {
  /* Report to the user what the command is about to do,
     and, if <interactive>, ask the user whether to continue.
     Return true if everything's normal,
     false if user wants to drop without performing the action,
     or terminate if user didn't want to continue. */
  char buf[MSG_MAX], *verb=action_verb(action.type);
  bool cancel=false, do_action=true;

  if( interactive ) {
    if( action.num > 1 ) {
      int read_again=true;
      while( read_again == true ) {
	printf("%s %d files to `%s' [Yn]?", verb, action.num, dest);
	if( fgets(buf, MSG_MAX, stdin) == NULL ) {
	  printf("error reading from stdin\n");
	  exit(EXIT_FAILURE);
	}
	switch (buf[0]) {
	case '\n':
	case 'Y': case 'y':
	  read_again = false;
	  break;
	case 'N': case 'n':
	  read_again = false;
	  cancel = true;
	  break;
	default:
	  printf("What?\n");
	  break;
	}
	printf("%s `%s' to `%s'\n", verb, source, dest);
      }
    } else {
      int read_again=true;
      while( read_again == true ) {
	printf("%s `%s' to `%s' [Ynd]?", verb, source, dest);
	if( fgets(buf, MSG_MAX, stdin) == NULL ) {
	  printf("error reading from stdin\n");
	  exit(EXIT_FAILURE);
	}
	switch (buf[0]) {
	case '\n':
	case 'Y': case 'y':
	  read_again = false;
	  break;
	case 'N': case 'n':
	  cancel = true;
	  read_again = false;
	  break;
	case 'D': case 'd':
	  do_action = false;
	  printf("drop `%s'\n", source);
	  read_again = false;
	  break;
	default:
	  printf("What?\n");
	  break;
	}
      }
    }
  } else { 			/* !interactive */
    printf("%s `%s' to `%s'\n", verb, source, dest);
  }
  if( cancel ) {
    printf("%s canceled by user\n", verb);
    exit(EXIT_FAILURE);
  }
  return do_action;
}

void action_pop(int s, struct Action action, bool interactive) {
  /* <action> the top file from the stack, and pop it. */
  char *prefix="action_pop:", *stack_state="stack not altered";
  char buf[FILEPATH_MAX], *source, *dest, **exargv, *verb=action_verb(action.type);
  bool remote_error=false;
  int instack;

  soc_w(s, CMD_SIZE);
  soc_r(s, buf, MSG_MAX);
  instack = atoi(buf);
  if( action.num > instack ) {
    if( instack == 0 )
      fprintf(stderr, "%s: cannot pop, file stack empty\n", program_name);
    else
      fprintf(stderr, "%s: asked to %s %d file%s, only %d in stack\n",
	      program_name, verb, action.num, PLURALS(action.num), instack);
    exit(EXIT_FAILURE);
  }

  soc_w(s, CMD_PEEK);
  if( !read_status_okay(s) )
    remote_error = true;
  if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
    fprintf(stderr, "%s quitting for read error (%s)\n", prefix, stack_state);
    exit(EXIT_FAILURE);
  }
  if( remote_error ) {
    if( strcmp(buf, MSG_ERR_STACK_EMPTY) == 0 )
      printf("Could not %s; file stack empty\n", verb);
    else
      fprintf(stderr, "%s received error `%s' (%s)\n", prefix, buf, stack_state);
    exit(EXIT_FAILURE);
  }
  source = buf;

  dest = real_target(action.ptr);
  if( action.num > 1 && !isdir(dest) ) {
    fprintf(stderr, "%s: multi-file target `%s' is not a directory\n", program_name, dest);
    exit(EXIT_FAILURE);
  }
  if( verbose ) {
    printf("src: %s\n", source);
    printf("dst: %s\n", dest);
  }

  exargv = cmd_gen(action, source, dest); /* copies references, not data */

  if( cmd_report(action, source, dest, interactive)
      && action_exec(exargv) != 0 ) {
    fprintf(stderr, "%s copy unsuccessful, aborting... (%s)\n", prefix, stack_state);
    exit(EXIT_FAILURE);
  }
  free(dest);
  free(exargv);
  soc_w(s, CMD_POP);
  stack_state = "stack state debatable";
  if( !read_status_okay(s) ) {
    fprintf(stderr, "%s could not confirm pop from stack (%s)\n", prefix, stack_state);
    exit(EXIT_FAILURE);
  }
  soc_r(s, buf, FILEPATH_MAX);
  if( --(action.num) > 0 )
    action_pop(s, action, false);
}

void interactive(int s) {
  /* Open an interactive terminal session with the daemon.
     Useful for debugging, not much else. */
  char buf[FILEPATH_MAX+1], *nl;
  int c, read;

  while( printf("> "), fgets(buf, FILEPATH_MAX+1, stdin) != NULL ) {
    nl = strchr(buf, '\n');
    if( nl == NULL ) {
      printf("Didn't send, input too long\n");
      while( (c = getchar()) != '\n' && c != EOF )
	/* discard */;
      continue;
    }
    *nl = '\0';
    if( strcmp(buf, "q") == 0 )
      break;
    soc_w(s, buf);

    do {
      if( (read = soc_r(s, buf, FILEPATH_MAX)) > 0 ) {
	printf("recv> `%s'\n", buf);
      } else {
	if( read < 0 )
	  fprintf(stderr, "Quitting for read error\n");
	else
	  fprintf(stderr, "Server closed connection\n");
	exit(EXIT_FAILURE);
      }
      /* check if there's more */
    } while( readwait(s, 0.2) );
  }
}

void action_do(struct Action action, int s) {
  /* Invoke the proper handler for <action>. */
  int i;

  switch (action.type) {
  case PUSH:
    if( verbose )
      printf("push\n");
    for( i = 0; i < action.num; i++ ) {
      push(s, ((char**)action.ptr)[i]);
    }
    break;
  case DROP:
    if( verbose )
      printf("drop\n");
    multidrop(s, action.num);
    break;
  case NOTHING:
  case PRINT:
    if( verbose )
      printf("print\n");
    print(s);
    break;
  case COPY:
  case MOVE:
  case SYMLINK:
    if( verbose )
      printf("action_pop\n");
    action_pop(s, action, true);
    break;
  case INTERACTIVE:
    if( verbose )
      printf("interactive mode\n");
    interactive(s);
    break;
  case STOP:
    stop_daemon(s);
    break;
  }
}

/*
 * Stack functions
 */

Node **new_stack() {
  /* Return a blank stack. */
  Node **stack=malloc(sizeof(*stack));

  *stack = NULL;
  return stack;
}

void stack_push(char *dat, Node **stack) {
  /* Push <dat> onto the stack. */
  Node *new=xmalloc(sizeof(*new));

  new->dat = xstrdup(dat);
  new->next = *stack;
  *stack = new;
}

bool stack_drop(Node **stack) {
  /* Drop the top item from the stack. */
  Node *dropnode=*stack;

  if( dropnode == NULL )
    return false;
  *stack = (*stack)->next;
  free(dropnode->dat);
  free(dropnode);
  return true;
}

char *stack_peek(Node **stack) {
  /* Return the top item of the stack. */

  return (*stack)->dat;
}

char *stack_nth(int n, Node **stack) {
  /* Return the <n>th item of the stack. */

  if( n > 0 )
    return stack_nth( n-1, &((*stack)->next) );
  else
    return stack_peek(stack);
}

int stack_len(Node **stack) {
  /* Return the number of items in the stack. */
  Node *elt=*stack;
  int i=0;

  while( elt != NULL ) {
    elt = elt->next;
    i++;
  }
  return i;
}

/*
 * Daemon functions
 */

bool daemon_serve(int s, char *cmd) {
  /* Do <cmd> for client connected on <s>. */
  static Node *null=NULL, **stack=&null;
  char buf[FILEPATH_MAX];
  bool keep_running=true;

  if( strcmp(cmd, CMD_PUSH) == 0 ) {
    char *status;
    if( stack_len(stack) >= STACK_MAX ) {
      printf("daemon: push request failed (stack full)\n");
      status = MSG_ERROR;
      strcpy(buf, MSG_ERR_STACK_FULL);
    } else {
      soc_w(s, MSG_SUCCESS);
      if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
	printf("daemon: push request failed (read error)\n");
	status = MSG_ERROR;
	strcpy(buf, MSG_ERR_LENGTH);
      } else {
	status = MSG_SUCCESS;
	stack_push(buf, stack);
	printf("daemon: PUSH `%s'\n", buf);
      }
    }
    soc_w(s, status);
    soc_w(s, buf);

  } else if( strcmp(cmd, CMD_POP) == 0 ) {
    char *status;
    if( stack_len(stack) > 0 ) {
      status = MSG_SUCCESS;
      sprintf(buf, "%s", stack_peek(stack));
      stack_drop(stack);
      printf("daemon: POP `%s'\n", buf);
    } else {
      printf("daemon: tried to pop from empty stack\n");
      status = MSG_ERROR;
      sprintf(buf, MSG_ERR_STACK_EMPTY);
    }
    soc_w(s, status);
    soc_w(s, buf);

  } else if( strcmp(cmd, CMD_PEEK) == 0 ) {
    char *status;
    if( stack_len(stack) > 0 ) {
      status = MSG_SUCCESS;
      sprintf(buf, "%s", stack_peek(stack));
    } else {
      status = MSG_ERROR;
      sprintf(buf, MSG_ERR_STACK_EMPTY);
    }
    soc_w(s, status);
    soc_w(s, buf);

  } else if( strcmp(cmd, CMD_PICK) == 0 ) {
    char *picked;
    soc_r(s, buf, MSG_MAX);
    picked = stack_nth(atoi(buf), stack);
    if( picked == NULL ) {
      soc_w(s, MSG_ERROR);
      soc_w(s, "stack is not quite that deep");
    } else {
      soc_w(s, MSG_SUCCESS);
      soc_w(s, picked);
    }

  } else if( strcmp(cmd, CMD_SIZE) == 0 ) {
    sprintf(buf, "%d", stack_len(stack));
    soc_w(s, buf);

  } else if( strcmp(cmd, CMD_STOP) == 0 ) {
    printf("daemon: Shutting down...\n");
    soc_w(s, MSG_SUCCESS);
    keep_running = false;

  } else {
    char msg[MSG_MAX + FILEPATH_MAX];
    sprintf(msg, "unknown command `%s'", cmd);
    soc_w(s, msg);
  }

  return keep_running;
}

void daemon_run(int soc_listen) {
  /* Main daemon loop. */
  int soc_connect;
  bool done, connected;
  char cmd[MSG_MAX];

  /* we don't want to terminate just because a client broke the socket */
  sig_ignore(SIGPIPE);

  if( listen(soc_listen, 1) == -1 ) {
    perror("daemon: listen");
    exit(EXIT_FAILURE);
  }

  /* let parent know that we're ready */
  printf("daemon: signalling %d\n", getppid());
  kill(getppid(), SIGUSR1);

  done = false;
  while(!done) {
    printf("daemon: Waiting for a connection...\n");
    soc_connect = accept(soc_listen, NULL, NULL);
    if( soc_connect == -1 ) {
      perror("daemon: accept");
      exit(EXIT_FAILURE);
    }
    connected = true;
    printf("daemon: Connected.\n");
    while( connected && !done ) {
      int n;
      n = soc_r(soc_connect, cmd, MSG_MAX);
      if( n < 0 ) {
	connected = false;
	printf("daemon: disconnected for read error\n");
      }
      if( n == 0 ) {
	connected = false;
	printf("daemon: disconnected for closed socket\n");
      }
      if( connected ) {
	printf("daemon: received command `%s'\n", cmd);
	if( !daemon_serve(soc_connect, cmd) )
	  done = true;
      }
    }
    close(soc_connect);
  }
}

/*
 * and here's main()
 */

int main(int argc, char **argv) {
  int soc_listen;
  struct sockaddr_un local;
  struct Action action;

  set_program_name(argv[0]);
  genset_soc_path();

  action = handle_options(argc, argv);
  sig_block(SIGUSR1);

  if( (soc_listen = socket(AF_UNIX, SOCK_STREAM, 0)) == -1 ) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  /* try to bind a new socket; if we can, use it to start the server */
  local.sun_family = AF_UNIX;
  strcpy(local.sun_path, soc_path);
  if( bind(soc_listen, (struct sockaddr *)&local, sizeof(local)) == -1 ) {
    if( errno == EADDRINUSE ){
      if( verbose )
	printf("Daemon already running.\n");
    } else {
      perror("bind");
      exit(EXIT_FAILURE);
    }
  } else {
    if( verbose )
      printf("pid=`%d'\n", getpid());
    /* start the daemon */
    printf("Starting daemon...\n");

    if( !fork() ) {
      /* don't fill the log with junk just because client was started with -v */
      verbose = 0;
      am_daemon = true;
      log_output();
      printf("daemon: Daemon started with pid %d\n", getpid());
      daemon_run(soc_listen);
      unlink(soc_path);
      printf("daemon: All done.     -><-\n");
      exit(EXIT_SUCCESS);

    } else { /* if(!fork) */
      if( verbose )
	printf("Waiting for signal...\n");
      /* wait a whole second for the daemon to start running */
      /* expect to receive SIGUSR1 */
      if( !sig_catch(1.0) ) {
	fprintf(stderr, "Daemon failed to start\n");
	fprintf(stderr, "Continue anyway\n");
      }
    }
  }
  if( close(soc_listen) == -1 ) {
    perror("close");
  }

  int s = client_connect();

  action_do(action, s);
  close(s);
  if( verbose )
    printf("Client exit\n");
  return EXIT_SUCCESS;
}
