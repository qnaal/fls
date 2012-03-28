/* Manage the file stack, and arbitrate access through a socket. */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include "stack.h"
#include "comm.h"
#include "sig.h"

static bool daemon_serve(int s, char *cmd) {
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
    soc_w(s, MSG_SUCCESS);
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
