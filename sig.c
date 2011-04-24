/* Control response to signals. */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <signal.h>

void sig_block(int signum) {
  /* Block signal <signum>. */
  sigset_t block_mask;

  sigemptyset(&block_mask);
  sigaddset(&block_mask, signum);
  sigprocmask(SIG_BLOCK, &block_mask, NULL);
}

bool sig_catch(float timeout) {
  /* Wait up to <timeout> seconds to receive a signal (including blocked/pending).
     Return whether we did. */
  struct timespec ts;
  sigset_t all_sigs;

  ts.tv_sec = (int)timeout;
  ts.tv_nsec = (timeout - ts.tv_sec) * 1000000000;
  sigfillset(&all_sigs);
  if( sigtimedwait(&all_sigs, NULL, &ts) > 0 )
    return true;
  return false;
}

void sig_ignore(int signum) {
  /* Ignore any received <signum>. */
  struct sigaction sa_ign;

  sa_ign.sa_handler = SIG_IGN;
  sa_ign.sa_flags = 0;
  sigemptyset(&sa_ign.sa_mask);

  if( sigaction(signum, &sa_ign, NULL) ) {
    perror("sig_ignore: sigaction");
    exit(EXIT_FAILURE);
  }
}
