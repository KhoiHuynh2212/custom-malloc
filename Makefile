CC = gcc
CFLAGS = -Wall -Wextra -std=c11
ASAN = -fsanitize=address,undefined -g

all:
	$(CC) $(CFLAGS) -o my-malloc my-malloc.c

asan:
	$(CC) $(CFLAGS) $(ASAN) -o my-malloc my-malloc.c

run: all
	./my-malloc

clean:
	rm -f my-malloc

valgrind: all
	valgrind ./my-malloc