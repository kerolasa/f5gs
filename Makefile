all:
	gcc -pthread -Wall -pedantic f5gs.c -o f5gs

clean:
	rm -f f5gs
