all: aspark

aspark: aspark.c
	$(CC) -std=c99 -pedantic -O2 -Wall -W -ansi aspark.c -o aspark -lm

clean:
	rm -f aspark
