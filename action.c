/* Record information about what the client is trying to do into a format
   easily passed between functions. */

#include <stdlib.h>
#include <stdio.h>
#include "action.h"

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
