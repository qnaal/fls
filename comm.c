/* Provide a simple socket communication system. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "comm.h"
#include "fls.h"


int soc_r(int s, char *buf, int blen) {
  /* Read a string to <buf> from <s>.
     Return the number of bytes read (including null) on success,
     0 on broken socket, or -1 on general failure. */
  char *last_read, *prefix;
  bool garbage=false;
  int n;

  if( am_daemon )
    prefix = "daemon: revc";
  else
    prefix = "recv";

  if( blen < 1 ) {
    fprintf(stderr, "%s: unacceptable buffer size of %d\n", prefix, blen);
    return -1;
  }
  last_read = buf;
  n = recv(s, last_read, 1, 0);

  while(1) {

    /* check error */
    if( n <= 0 ) {
      if( n == -1 ) {
	if( errno == EAGAIN || errno == EWOULDBLOCK ) {
	  /* end of sent */
	  fprintf(stderr,"%s didn't get full string (%d bytes, no null)\n", prefix, (int)(last_read - buf));
	  *last_read = 0;
	  fprintf(stderr,"%s partial message: `%s'\n", prefix, buf);
	} else
	  perror(prefix);
      }
      return n;
    }

    /* check null */
    if( *last_read == 0 ) {
      if( verbose > 1 )
	printf("%s read null\n", prefix);
      break;
    } else {
      if( verbose > 1 )
	printf("%s read char %#04x `%c'; %d in string\n", prefix, *last_read, *last_read, (int)(last_read - buf) +1);
    }

    /* check buffer length before reading char */
    if( ++last_read - buf >= blen ) {
      /* clear the stream buffer til the next null, what's left can only possibly be garbage */
      if( !garbage ) {
	garbage = true;
	fprintf(stderr,"%s filled buffer before getting full string (%d bytes, no null)\n", prefix, (int)(last_read - buf));
	*--last_read = 0;
	fprintf(stderr,"%s first %d bytes of lost message: `%s'\n", prefix, (int)(last_read - buf), buf);
      } else
	fprintf(stderr,"%s flopped around a bit while cleaning read buffer\n", prefix);
      last_read = buf;
    }
    n = recv(s, last_read, 1, MSG_DONTWAIT);
  }

  if( garbage )
    return -1;

  if( verbose )
    printf("%s `%s'\n", prefix, buf);
  /* +1 to count last_read */
  return last_read +1 - buf;
}

void soc_w(int s, char *buf) {
  /* Quick and dirty write string to socket -
     expect most error handling to happen on the other side. */
  char *prefix;
  int len, n;

  if( am_daemon )
    prefix = "daemon: send";
  else
    prefix = "send";
  if( verbose )
    printf("%sing `%s'\n", prefix, buf);
  /* add one for the null */
  len = strlen(buf) +1;
  n = write(s, buf, len);
  if( n != len ) {
    if( n == -1 ) {
      perror(prefix);
    } else
      /* FIXME: should resend the rest */
      fprintf(stderr, "%s short by %d bytes\n", prefix, len - n);
  }
}

bool readwait(int s, float timeout) {
  /* Return whether socket <s> is ready for reading, after waiting
     up to <timeout> seconds for that to become true. */
  struct timeval tv;
  fd_set readfds;

  tv.tv_sec = (int)timeout;
  tv.tv_usec = (timeout - tv.tv_sec) * 1000000;

  FD_ZERO(&readfds);
  FD_SET(s, &readfds);

  if( select(s+1, &readfds, NULL, NULL, &tv) )
    return true;
  return false;
}

bool read_status_okay(int s) {
  /* Read status message from socket <s>.
     Return true if successfully read MSG_SUCCESS,
     false otherwise. */
  char buf[MSG_MAX];
  if( soc_r(s, buf, MSG_MAX) > 0 && strcmp(buf, MSG_SUCCESS) == 0 )
    return true;
  return false;
}

int client_connect() {
  /* Return a socket that is connected to the daemon. */
  struct sockaddr_un sockaddr;
  int s, len;

  s = socket(AF_UNIX,SOCK_STREAM, 0);
  if( s == -1 ) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  if( verbose )
    printf("Trying to connect...\n");
  sockaddr.sun_family = AF_UNIX;
  strcpy(sockaddr.sun_path, soc_path);
  len = strlen(sockaddr.sun_path) + sizeof(sockaddr.sun_family);
  if( connect(s, (struct sockaddr*)&sockaddr, len) == -1 ) {
    if( errno == ECONNREFUSED ){
      fprintf(stderr, "No-one listening at `%s'.\n", soc_path);
      exit(EXIT_FAILURE);
    }
    perror("connect");
    exit(EXIT_FAILURE);
  }
  if( verbose )
    printf("Connected.\n");
  return s;
}
