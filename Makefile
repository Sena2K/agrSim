# Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 `pkg-config fuse3 --cflags`
LIBS = `pkg-config fuse3 --libs`

OBJ = main.o bmpfs.o bmp.o

all: bmpfs

bmpfs: $(OBJ)
	$(CC) $(CFLAGS) -o bmpfs $(OBJ) $(LIBS)

main.o: main.c bmpfs.h
	$(CC) $(CFLAGS) -c main.c

bmpfs.o: bmpfs.c bmpfs.h bmp.h
	$(CC) $(CFLAGS) -c bmpfs.c

bmp.o: bmp.c bmp.h
	$(CC) $(CFLAGS) -c bmp.c

clean:
	rm -f *.o bmpfs

