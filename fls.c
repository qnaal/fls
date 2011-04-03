#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <pwd.h>
#include <sys/un.h>
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

enum ActionType{
  NOTHING,
  PUSH,
  DROP,
  PRINT,
  COPY,
  MOVE,
  INTERACTIVE,
  STOP,
};

char *soc_path;
int verbose=0;
bool am_daemon=false;
/* perhaps a global prefix for "deamon: " or "" ?*/

/*
 * general functions
 */

void usage(int status) {
  printf("\
Usage: %s [ACTION]\n\
  or:  %s [FILE]\n\
", PROGRAM_NAME, PROGRAM_NAME);
  printf("\
Push FILE onto the stack, or perform action ACTION.\n\
\n\
The files in the stack are not altered until being popped, \n\
and even then, only if they are to be moved.\n\
");
  printf("\
\n\
Actions:\n\
  -c    COPY\n\
          pop a file from the stack, copy it to current dir\n\
  -m    MOVE\n\
          pop a file from the stack, move it to current dir\n\
  -d    DROP\n\
          pop a file from the stack, print its name\n\
  -p    PRINT\n\
          print the contents of the stack\n\
  -q    QUIT\n\
          terminate the stack daemon, losing the contents of the stack\n\
  -h    HELP\n\
          display usage information, and then exit\n\
");
  printf("\
\n\
All but the last entered action are ignored.\n\
\n\
If no args are provided, the default action is PRINT.\n\
\n\
If FILE is provided, push it onto the stack.\n\
");
  exit(status);
}

void *xmalloc(size_t size) {
  /* loudly fail on memory allocation fail */
  void *ptr;

  ptr = malloc(size);
  if( ptr == NULL ) {
    fprintf(stderr, "malloc failed\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

void log_output() {
  /* redirect stdout and stderr to <soc_path>.log */
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

/*
 * socket functions
 */

void set_soc_path() {
  /* generate soc_path from user name */
  char *username, *tmpdir="/tmp/";
  int size;

  username = getenv("USER");

  size = strlen(tmpdir) + strlen(username) + strlen(PROGRAM_NAME) +1;
  soc_path = xmalloc(size);
  sprintf(soc_path, "%s%s%s", tmpdir, username, PROGRAM_NAME);
}

int soc_r(int s, char *buf, int blen) {
  /* recursive read, reads whole strings
     because, sometimes, two quick writes are sent as one packet (?)
     returns number of bytes read (with null) on success
     returns 0 on broken socket, and -1 on general failure */
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
      /* n==0 means connection dropped; should I let somebody know? */
      /* SPECULAH: throw USR2 or PIPE in case of error? (client would kill self, daemon would disconnect/ignore) */
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
  /* +1 to start index at 1 */
  return last_read - buf +1;
}

void soc_w(int s, char *buf) {
  /* quick and dirty write string to socket -
     expect most error handling to happen on the other side */
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
  /* waits up to <timeout> secs for socket <s> to not block; returns whether it did */
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
  /* reads status message from socket <s>;
     returns true if successfully read MSG_SUCCESS,
     false otherwise */
  char buf[MSG_MAX];
  if( soc_r(s, buf, MSG_MAX) > 0 && strcmp(buf, MSG_SUCCESS) == 0 )
    return true;
  return false;
}


/*
 * signal functions
 */

void sig_block(int signum) {
  /* block signal <signum> */
  sigset_t block_mask;

  sigemptyset(&block_mask);
  sigaddset(&block_mask, signum);
  sigprocmask(SIG_BLOCK, &block_mask, NULL);
}

bool sig_catch(float timeout) {
  /* wait up to <timeout> seconds to receive a signal (including blocked/pending)
     return whether we did */
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
  /* ignore any received <signum> */
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
 * client functions
 */

enum ActionType handle_options(int argc, char **argv) {
  /* returns the proper action to take */
  int c;
  enum ActionType action = NOTHING;

  while( (c = getopt(argc, argv, "cmdpiqvh")) != -1 ) {
    switch (c) {
    case 'c':
      action = COPY;
      break;
    case 'm':
      action = MOVE;
      break;
    case 'd':
      action = DROP;
      break;
    case 'p':
      action = PRINT;
      break;
    case 'i':
      action = INTERACTIVE;
      break;
    case 'q':
      action = STOP;
      break;
    case 'v':
      verbose++;
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
    action = PUSH;
  }
  return action;
}

int client_connect() {
  /* connects to daemon, returns connected socket */
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
  /* instructs daemon to push file onto stack, terminates on error */
  char buf[FILEPATH_MAX], *fullpath, *prefix="push:";
  int okay;

  soc_w(s, "push");
  okay = read_status_okay(s);
  if( !okay ) {
    if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
      fprintf(stderr, "%s quitting for read error\n", prefix);
      exit(EXIT_FAILURE);
    }
    printf("Could not push; received error: `%s'\n", buf);
    return;
  }

  fullpath = realpath(file, NULL); /* malloc()s fullpath */
  if( fullpath == NULL ) {
    perror("realpath");
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
  /* instructs daemon to pop a file from the stack, returns true if it could */
  char buf[FILEPATH_MAX], *prefix="drop:";
  int okay;

  soc_w(s, "pop");
  okay = read_status_okay(s);
  if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
    fprintf(stderr, "%s quitting for read error\n", prefix);
    exit(EXIT_FAILURE);
  }
  if( okay ) {
    printf("Popped `%s'\n", buf);
    return true;
  } else {
    printf("error: `%s'\n", buf);
    return false;
  }
}

void print(int s) {
  /* instructs daemon to send back the contents of the stack,
     and prints it for the user */
  char buf[FILEPATH_MAX];
  int i, stack_size;

  soc_w(s, "print");
  if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
    exit(EXIT_FAILURE);
  }
  stack_size = atoi(buf);
  printf("%d file%s in stack\n", stack_size, (stack_size == 1) ? "" : "s");
  for( i = 0; i < stack_size; i++ ) {
    if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
      exit(EXIT_FAILURE);
    }
    printf("%d: %s\n", i, buf);
  }
}

char **cmd_gen(enum ActionType action, char *source, char *dest, char **verb) {
  /* generates the command to run to perform "action" between "source" and "dest",
     returns the command in the form of null-terminiated argv, and sets "verb" to the proper verb */
  char *prefix = "cmd_gen:";
  char **argv;

  switch (action) {
  case COPY:
    {
      char *array_init[] = {
	"/bin/cp",
	"-r",
	"--",
	source,
	dest,
	NULL
      };
      *verb = "copy";
      argv = xmalloc(sizeof(array_init));
      memcpy(argv, array_init, sizeof(array_init));
      break;
    }
  case MOVE:
    {
      char *array_init[] = {
	"/bin/mv",
	"--",
	source,
	dest,
	NULL
      };
      *verb = "move";
      argv = xmalloc(sizeof(array_init));
      memcpy(argv, array_init, sizeof(array_init));
      break;
    }
  default:
    fprintf(stderr, "%s error: unsupported action\n", prefix);
    exit(EXIT_FAILURE);
  }
  return argv;
}

int action_exec(char **argv) {
  /* Fork an exec, and return the exit status, or -1 on other error */
  char *prefix="action_exec:";
  int exit_status;
  pid_t pid;

  pid = fork();

  if( pid == 0 ) {
    if( execv(argv[0], argv) == -1 ) {
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
	fprintf(stderr,"%s %s exited with status=%d\n", prefix, argv[0], status);
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

void action_pop(int s, enum ActionType action) {
  /* instructs daemon to pop a file from the stack,
     and "action"s that file to the current working dir */
  char *prefix="action_pop:", *stack_state="stack not altered";
  char buf[FILEPATH_MAX], *source, dest[FILEPATH_MAX], **execargv, *verb;
  bool remote_error=false;
  int c;

  soc_w(s, "peek");
  if( !read_status_okay(s) )
    remote_error = true;
  if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
    fprintf(stderr, "%s quitting for read error (%s)\n", prefix, stack_state);
    exit(EXIT_FAILURE);
  }
  if( remote_error ) {
    if( strcmp(buf, MSG_ERR_STACK_EMPTY) == 0 )
      printf("Could not pop; file stack empty\n");
    else
      fprintf(stderr, "%s received error `%s' (%s)\n", prefix, buf, stack_state);
    exit(EXIT_FAILURE);
  }
  source = buf;

  if( getcwd(dest, FILEPATH_MAX) == NULL ) {
    perror("getcwd");
    fprintf(stderr, "%s\n", stack_state);
    exit(EXIT_FAILURE);
  }
  if( verbose ) {
    printf("src: %s\n", source);
    printf("dst: %s\n", dest);
  }

  execargv = cmd_gen(action, source, dest, &verb);

  /* TODO: add 'd' option for "drop from stack but don't do anything with the value" */
  printf("%s `%s' to `%s' [Yn]?", verb, source, dest);
  c = getchar();
  if( c == 'n' ) {
    printf("%s canceled by user (%s)\n", verb, stack_state);
    exit(EXIT_FAILURE);
  }

  if( action_exec(execargv) != 0 ) {
    fprintf(stderr, "%s copy unsuccessful, aborting... (%s)\n", prefix, stack_state);
    exit(EXIT_FAILURE);
  }
  soc_w(s, "pop");
  stack_state = "stack state debatable";
  if( !read_status_okay(s) ) {
    fprintf(stderr, "%s could not confirm pop from stack (%s)\n", prefix, stack_state);
    exit(EXIT_FAILURE);
  }
  free(execargv);
}

void interactive(int s) {
  /* opens up an interactive terminal session with the daemon
     useful for debugging, not much else */
  char buf[FILEPATH_MAX+1], *nl;
  int c, read;
  bool more;

  while( printf("> "), fgets(buf, FILEPATH_MAX+1, stdin) != NULL ) {
    /* drop the newline */
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
      more = readwait(s, 0.2);
    } while( more );
  }
}

void do_action(enum ActionType action, int s, int argc, char **argv) {
  /* invokes the proper handler for action, passing args as necessary */

  switch(action) {
  case PUSH:
    if( verbose )
      printf("push\n");
    /* TODO: push all of these args, not just the first one */
    push(s, argv[0]);
    break;
  case DROP:
    if( verbose )
      printf("drop\n");
    drop(s);
    break;
  case NOTHING:
  case PRINT:
    if( verbose )
      printf("print\n");
    print(s);
    break;
  case COPY:
  case MOVE:
    if( verbose )
      printf("action_pop\n");
    action_pop(s, action);
    break;
  case INTERACTIVE:
    if( verbose )
      printf("interactive mode\n");
    interactive(s);
    break;
  case STOP:
    soc_w(s, "stop");
    printf("Server shutting down.\n");
    break;
  }
}

/*
 * daemon functions
 */

bool daemon_serve(int s, char *cmd) {
  /* do cmd for client connected on s */
  static char *stack[STACK_MAX];
  static int stackind=0;
  char buf[FILEPATH_MAX];
  bool keep_running=true;

  /* SPECULAH:
     replace with enums and switch?
     use macros for these things, instead of having the same strings here and there?
     send status before every message? */
  if( strcmp(cmd, "push") == 0 ) {
    char *status;
    if( stackind >= STACK_MAX ) {
      printf("deamon: push request failed (stack full)\n");
      status = MSG_ERROR;
      strcpy(buf, MSG_ERR_STACK_FULL);
    } else {
      soc_w(s, MSG_SUCCESS);
      if( soc_r(s, buf, FILEPATH_MAX) <= 0 ) {
	printf("deamon: push request failed (read error)\n");
	status = MSG_ERROR;
	strcpy(buf, MSG_ERR_LENGTH);
      } else {
	status = MSG_SUCCESS;
	stack[stackind] = xmalloc(strlen(buf) +1);
	strcpy(stack[stackind++], buf);
	printf("daemon: PUSH `%s'\n", buf);
      }
    }
    soc_w(s, status);
    soc_w(s, buf);

  } else if( strcmp(cmd, "pop") == 0 ) {
    char *status;
    if( stackind > 0 ) {
      sprintf(buf, "%s", stack[--stackind]);
      free(stack[stackind]);
      printf("daemon: POP `%s'\n", buf);
      status = MSG_SUCCESS;
    } else {
      printf("daemon: tried to pop from empty stack\n");
      sprintf(buf, MSG_ERR_STACK_EMPTY);
      status = MSG_ERROR;
    }
    soc_w(s, status);
    soc_w(s, buf);

  } else if( strcmp(cmd, "peek") == 0 ) {
    char *status;
    if( stackind > 0 ) {
      sprintf(buf, "%s", stack[stackind -1]);
      status = MSG_SUCCESS;
    } else {
      sprintf(buf, MSG_ERR_STACK_EMPTY);
      status = MSG_ERROR;
    }
    soc_w(s, status);
    soc_w(s, buf);

  } else if( strcmp(cmd, "print") == 0 ) {
    char buf[FILEPATH_MAX];
    int i;
    sprintf(buf, "%d", stackind);
    soc_w(s, buf);
    for( i = stackind -1; i >= 0; i-- ) {
      soc_w(s, stack[i]);
    }

  } else if( strcmp(cmd, "stop") == 0 ) {
    printf("daemon: Shutting down...\n");
    strcpy(buf, "Okay, I'm shutting down");
    soc_w(s, buf);
    keep_running = false;

  } else {
    char msg[MSG_MAX + FILEPATH_MAX];
    sprintf(msg, "unknown command `%s'", cmd);
    soc_w(s, msg);
  }

  return keep_running;
}

void daemon_run(int soc_listen) {
  /* main daemon loop */
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
    printf("deamon: Connected.\n");
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
  enum ActionType action = handle_options(argc, argv);

  set_soc_path();
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

    /* TODO: make sure daemon survives a SIGINT to its parent */
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
      /* expect to be receive SIGUSR1 */
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
  int action_argc = argc - optind;
  char **action_argv = argv + optind;

  do_action(action, s, action_argc, action_argv);
  close(s);
  if( verbose )
    printf("Client exit\n");
  return EXIT_SUCCESS;
}
