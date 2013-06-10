OUT_FILES=sem-waitzero sem-pingpong sem-lockunlock

CFLAGS = -Wall -g -O2 -pthread
LFLAGS = -static
CC= gcc
CPP= g++

%:	%.cpp
	$(CPP) $(CFLAGS) -o $@ $< $(LFLAGS)

%:	%.c
	$(CC) $(CFLAGS) -o $@ $< $(LFLAGS)


all: $(OUT_FILES)

clean:
	rm -f $(OUT_FILES)
	rm -f *~
	rm -f core

