all: httpserver.o
	gcc -Wall -Wextra -Wpedantic -Wshadow -o httpserver httpserver.o -lpthread
httpserver.o: httpserver.c
	gcc -Wall -Wextra -Wpedantic -Wshadow -c httpserver.c -lpthread
clean:
	rm -f httpserver.o
spotless:
	rm -f *.o httpserver

