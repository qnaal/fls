/* Collect information from the filesystem. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <libgen.h>
#include <sys/stat.h>
#include "fls.h"


bool exists(char *path) {
  /* Return whether <path> exists on the filesystem. */
  struct stat st;

  if( stat(path, &st) == -1 ) {
    if( errno == ENOENT ) {
      return false;
    }
    perror("exists");
    exit(EXIT_FAILURE);
  }
  return true;
}

bool isdir(char *path) {
  /* Return whether <path> is a directory. */
  struct stat st;

  if( stat(path, &st) == -1 ) {
    if( errno != ENOENT ) {
      /* terminate for any error other than "file doesn't exist" */
      perror("isdir");
      exit(EXIT_FAILURE);
    }
  } else if( S_ISDIR(st.st_mode) )
    return true;
  return false;
}

char* abs_path(char *relpath) {
  /* Return the absolute path to file <relpath>, with a slash at the end
     if it is a dir.
     Return NULL if the file at <relpath> does not exist.
     Terminate if any other component of the path does not exist. */
  char* path;

  path = realpath(relpath, NULL);
  if( path == NULL ) {
    if( errno != ENOENT ) {
      perror("realpath");
      exit(EXIT_FAILURE);
    }
    return NULL;
  }
  if( isdir(path) && strcmp(path, "/") != 0 ) {
    path = realloc(path, strlen(path) +2);
    if( path == NULL ) {
      fprintf(stderr, "realloc failed\n");
      exit(EXIT_FAILURE);
    }
    memcpy(&(path[strlen(path)]), "/", 2);
  }
  return path;
}

char *real_target(char *reltarget) {
  /* Return the canonicalized absolute path to <reltarget>
     (or current working dir, if <reltarget> is NULL).
     Terminate if any component of the path but the last doesn't exist. */
  char *target;

  target = abs_path((reltarget == NULL ? "." : reltarget));
  if( target == NULL ) {
    char *dir, *base, *givendir, *givenbase;

    /* make copies of everything because I can't trust
       dirname and basename to do anything right */
    givendir = xstrdup(reltarget);
    dir = abs_path(dirname(givendir));
    if( dir == NULL ) {
      fprintf(stderr, "%s: target directory `%s' does not exist\n",
	      program_name, dirname(reltarget));
      exit(EXIT_FAILURE);
    }

    givenbase = xstrdup(reltarget);
    base = basename(givenbase);

    if( strcmp(dir, "/") == 0 ) {
      target = xmalloc(1+ strlen(base) +1);
      sprintf(target, "/%s", base);
    } else {
      target = xmalloc(strlen(dir) + strlen(base) +1);
      sprintf(target, "%s%s", dir, base);
    }
    free(givendir);
    free(dir);
    free(givenbase);
  }
  return target;
}


