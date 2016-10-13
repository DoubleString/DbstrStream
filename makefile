CC=gcc
C_SRC=$(wildcard *.c)
C_OBJ=$(patsubst %.c,%.o,$(C_SRC))

CFLAGS= -Wall -g -c -I ../headers
LDFLAGS= -lpthread
TARGET=stream

.PHONY: all clean

all:$(TARGET)

$(TARGET):$(C_OBJ)
	gcc -o $(TARGET) $(C_OBJ) -pthread -lm 
%.o:%.c
	$(CC) $(CFLAGS) $<

clean:
	rm *.o $(TARGET)