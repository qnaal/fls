/* Maintain a dynamically allocated stack of strings. */

#include <stdlib.h>
#include <string.h>
#include "fls.h"
#include "stack.h"

Node **stack_new() {
  /* Return a blank stack. */
  Node **stack=xmalloc(sizeof(*stack));

  *stack = NULL;
  return stack;
}

void stack_push(char *dat, Node **stack) {
  /* Push <dat> onto <stack>. */
  Node *new=xmalloc(sizeof(*new));

  new->dat = xstrdup(dat);
  new->next = *stack;
  *stack = new;
}

bool stack_drop(Node **stack) {
  /* Drop the top item from <stack>. */
  Node *dropnode=*stack;

  if( dropnode == NULL )
    return false;
  *stack = (*stack)->next;
  free(dropnode->dat);
  free(dropnode);
  return true;
}

char *stack_peek(Node **stack) {
  /* Return the top item of <stack>. */

  if( *stack == NULL )
    return NULL;
  return (*stack)->dat;
}

char *stack_nth(int n, Node **stack) {
  /* Return the <n>th item of <stack>. */

  if( *stack == NULL )
    return NULL;
  if( n > 0 )
    return stack_nth( n-1, &((*stack)->next) );
  else
    return stack_peek(stack);
}

int stack_len(Node **stack) {
  /* Return the number of items in <stack>. */
  Node *elt=*stack;
  int i=0;

  while( elt != NULL ) {
    elt = elt->next;
    i++;
  }
  return i;
}

void stack_free(Node **stack) {
  /* Drop all items from <stack>. */
  int i;

  while( stack_drop(stack) )
    ;
  free(stack);
}
