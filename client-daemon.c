/* Provide client with access to the daemon's stack through a socket. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "fls.h"
#include "client.h"
#include "comm.h"
#include "file-info.h"


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
    char *fullpathcolr = color_string(COLR_PATH, fullpath);
    printf("Pushed `%s'\n", fullpathcolr);
    free(fullpathcolr);

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
    char *filecolr = color_string(COLR_PATH, buf);
    printf("%d: %s\n", i, filecolr);
    free(filecolr);
  }
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
