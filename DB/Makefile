CC = gcc
CPPFLAGS += $(shell mariadb_config --cflags)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell mariadb_config --libs) -lm

.PHONY: all clean
all: db_admin

SOURCES = db_admin.c db_core.c db_member.c db_device.c db_song.c
OBJECTS = $(SOURCES:.c=.o)

db_admin: $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(LDLIBS)

$(OBJECTS): db_api.h
db_core.o db_member.o db_device.o db_song.o: db_internal.h

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f db_admin $(OBJECTS)
