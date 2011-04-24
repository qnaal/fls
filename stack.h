#include <stdbool.h>

#define STACK_MAX 100

typedef struct StackNode {
  char *dat;
  struct StackNode *next;
} Node;


Node **stack_new();
void stack_push(char *dat, Node **stack);
bool stack_drop(Node **stack);
char *stack_peek(Node **stack);
char *stack_nth(int n, Node **stack);
int stack_len(Node **stack);
void stack_free(Node **stack);
