/* Run the filesystem commands that do the real work. */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include "fls.h"
#include "comm.h"
#include "action.h"


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
	char *destcolr = color_string(COLR_PATH, dest);
	printf("%s %d files to `%s' [Yn]?", verb, action.num, destcolr);
	free(destcolr);
	
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
	if( !cancel ) {
	  char *sourcecolr = color_string(COLR_PATH, source);
	  char *destcolr = color_string(COLR_PATH, dest);
	  printf("%s `%s' to `%s'\n", verb, sourcecolr, destcolr);
	  free(sourcecolr);
	  free(destcolr);
	  /* rest of operations are reported noninteractively */
	}
      }
    } else {
      int read_again=true;
      while( read_again == true ) {
	char *sourcecolr = color_string(COLR_PATH, source);
	char *destcolr = color_string(COLR_PATH, dest);
	printf("%s `%s' to `%s' [Ynd]?", verb, sourcecolr, destcolr);
	free(sourcecolr);
	free(destcolr);
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
    char *sourcecolr = color_string(COLR_PATH, source);
    char *destcolr = color_string(COLR_PATH, dest);
    printf("%s `%s' to `%s'\n", verb, sourcecolr, destcolr);
    free(sourcecolr);
    free(destcolr);
  }
  if( cancel ) {
    printf("%s canceled by user\n", verb);
    exit(EXIT_FAILURE);
  }
  return do_action;
}
