#include <dirent.h>
#include "action.h"

#define PLURALS(int) (int == 1 ? "" : "s")


void action_do(struct Action action, int s);
