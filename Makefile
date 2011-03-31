SRC = fls.c

CC = cc


all: fls

fls:
	@echo "compiling..."
	@${CC} ${SRC} -o $@

clean:
	@echo "cleaning..."
	rm -f fls

again: clean fls
