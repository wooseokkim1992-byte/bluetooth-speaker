CC = gcc
CPPFLAGS += $(shell mariadb_config --cflags)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell mariadb_config --libs) -lm

.PHONY: all clean
all: db_admin

db_admin: db_admin.o db_api.o

db_admin.o: db_admin.c db_api.h
db_api.o: db_api.c db_api.h

clean:
	rm -f db_admin db_admin.o db_api.o
