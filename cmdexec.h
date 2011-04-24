char **cmd_gen(struct Action action, char *source, char *dest);
int action_exec(char **exargv);
bool cmd_report(struct Action action, char *source, char *dest, bool interactive);
