#all: binify flexfs

all:	cmd2bin  fddump  fdedit  flexadd  flex-binify  flexdsk  flexdump  flexedit  flexfs  flexsort  flextract

CFLAGS += -Wall -pedantic

clean:
	rm -f *.o *~ binify flexfs md2bin fddump fdedit flexadd flex-binify flexdsk \
	          flexdump flexedit flexfs flexsort flextract fddump.o fdedit.o     \
	          flexdump.o flexedit.o


flexfs.c : flexfs.h

# Programs that use ncurses: fddump, fdedit, flexdump, flexedit
fddump: fddump.o
	$(CC) $(CFLAGS) -o $@ $^ -lcurses

fdedit: fdedit.o
	$(CC) $(CFLAGS) -o $@ $^ -lcurses

flexdump: flexdump.o
	$(CC) $(CFLAGS) -o $@ $^ -lcurses

flexedit: flexedit.o
	$(CC) $(CFLAGS) -o $@ $^ -lcurses
