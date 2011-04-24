/*
 * fls -- Manage a file stack, allowing the user to push and pop files
 * around a filesystem.
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/un.h>
#include <sys/socket.h>
#include "fls.h"
#include "client.h"
#include "daemon.h"
#include "comm.h"
#include "sig.h"

int verbose=0;
bool am_daemon=false;


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

void set_program_name(const char *argv0) {
  /* Set program_name to argv0. */

  program_name = argv0;
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

struct Action handle_options(int argc, char **argv) {
  /* Return the proper action to take. */
  struct Action action = {NOTHING, 1, NULL};
  int c;

  while( (c = getopt(argc, argv, "cmsdpiqvn:h")) != -1 ) {
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
	fprintf(stderr, "invalid argument `%s' for option `%c'\n", optarg, c);
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
      action.ptr = &argv[optind];
      break;
    case COPY:
    case MOVE:
    case SYMLINK:
      if( (optind +1) < argc ) {
	fprintf(stderr, "Too many supplied arguments for requested action: `%s'\n",
		action_verb(action.type));
	usage(EXIT_FAILURE);
      }
      action.ptr = argv[optind];
      break;
    default:
      fprintf(stderr, "Requested action `%s' does not take arguments\n",
	      action_verb(action.type));
      usage(EXIT_FAILURE);
    }
  }
  return action;
}


int main(int argc, char **argv) {
  int soc_listen;
  struct sockaddr_un local;
  struct Action action;

  verbose = 0;
  am_daemon = false;
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
