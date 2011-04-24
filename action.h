#ifndef action_h
#define action_h

#define EXEC_ARG_MAX 6

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


struct ActionDef *action_def(enum ActionType type);
char *action_verb(enum ActionType type);

#endif
