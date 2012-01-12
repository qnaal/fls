/* Miscellaneous functions that hold the client system together. */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <libgen.h>
#include "fls.h"
#include "client.h"
#include "comm.h"
#include "stack.h"
#include "cmdexec.h"
#include "file-info.h"


int collision_check(int s, int n, char *dest) {
  /* Check if any of the top <n> files in the stack would collide with anything
     if they were all moved to <dest>.
     Return the number of collisions with files in <dest>.
     Terminate if any of said stack-files would collide with each other. */
  char buf[FILEPATH_MAX], *collisions[n];
  Node **stack=stack_new();
  int i, ncol=0;
  bool dest_is_dir=isdir(dest);

  if( n > 1 && !dest_is_dir ) {
    fprintf(stderr, "%s: multi-file target `%s' is not a directory\n",
	    program_name, dest);
    exit(EXIT_FAILURE);
  }

  for( i = 0; i < n; i++ ) {
    int j;
    char *to_push;
    soc_w(s, CMD_PICK);
    sprintf(buf, "%d", i);
    soc_w(s, buf);
    if( !read_status_okay(s) ) {
      soc_r(s, buf, FILEPATH_MAX);
      printf("received error `%s'\n", buf);
      exit(EXIT_FAILURE);
    }
    soc_r(s, buf, FILEPATH_MAX);
    to_push = basename(buf);

    for( j = 0; j < i; j++ ) {
      int ndx=i-j-1;	   /* where stack(j) is, in the daemon's stack */
      if( strcmp(to_push, stack_nth(j, stack)) == 0 ) {
	char *collisioncolr = color_string(COLR_PATH, to_push);
	fprintf(stderr, "%s: Stack items %d and %d are both named `%s', \
so I'm not going to let you do that.\n", program_name, i, ndx, collisioncolr);
	free(collisioncolr);
	usage(EXIT_FAILURE);
      }
    }
    stack_push(to_push, stack);
  }

  if( dest_is_dir ) {
    for( i = 0; i < n; i++ ) {
      DIR *dir=opendir(dest);
      if( dir != NULL ) {
	struct dirent *dent;
	while( (dent = readdir(dir)) ) {
	  char *dir_basename=dent->d_name;
	  char *stack_basename=stack_nth(i, stack);
	  if( strcmp(dir_basename, stack_basename) == 0 ) {
	    collisions[ncol++] = stack_basename;
	    break;
	  }
	}
      } else {
	perror("opendir");
	exit(EXIT_FAILURE);
      }
      closedir(dir);
    }
  } else {
    if( exists(dest) )
      ncol++;
  }

  if( ncol ) {
    char *ow = color_string(COLR_WARN, "overwrite");
    if( !dest_is_dir ) {
      char *destcolr = color_string(COLR_PATH, dest);
      printf("operation will %s `%s'\n", ow, destcolr);
      free(destcolr);
    } else if( ncol == 1 ) {
      char *destcolr = color_string(COLR_PATH, dest);
      char *filecolr = color_string(COLR_PATH, collisions[0]);
      printf("operation will %s `%s%s'\n", ow, destcolr, filecolr);
      free(filecolr);
      free(destcolr);
    } else {
      printf("operation will %s %d file%s:\n", ow, ncol, PLURALS(ncol));
      for( i = 0; i < ncol; i++ )
	puts(collisions[i]);
    }
    free(ow);
  }
  stack_free(stack);
  return ncol;
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
  if( interactive )
    collision_check(s, action.num, dest);
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
