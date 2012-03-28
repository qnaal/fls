SRC = fls.c \
      client.c \
      daemon.c \
      stack.c \
      comm.c \
      sig.c \
      action.c \
      client-daemon.c \
      cmdexec.c \
      file-info.c \

CC = cc


all: fls

fls: ${SRC}
	@echo "compiling..."
	@${CC} ${SRC} -o $@

clean:
	@echo "cleaning..."
	rm -f fls

again: clean fls
