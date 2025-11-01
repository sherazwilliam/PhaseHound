CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -fPIC -pthread
LDFLAGS ?= -pthread -lasound

INCS = -I../../../include

NAME := audiosink
SO := ph-lib$(NAME).so

SRCS_SO := src/$(NAME).c ../../../src/common.c

all: $(SO)

$(SO): $(SRCS_SO)
	$(CC) $(CFLAGS) $(INCS) -shared -o $@ $^ $(LDFLAGS) -ldl

clean:
	rm -f $(SO) $(SRCS_SO:.c=.o)
