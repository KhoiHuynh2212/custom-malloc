CC      = gcc
CFLAGS  = -Wall -Wextra
DEBUG   = -g
RELEASE = -O2

ASAN_FLAGS = -fsanitize=address,undefined -g

ASAN_RUN   = ASAN_OPTIONS=detect_leaks=0 

ma: 
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -o my-malloc my-malloc.c
	@echo "== RUNNING MALLOC =="
	./my-malloc