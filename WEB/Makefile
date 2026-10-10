CC = gcc
CPPFLAGS += $(shell mariadb_config --cflags)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell mariadb_config --libs) -lcrypt

OBJECTS = web_server.o web_http.o web_session.o signup.o login.o db_core.o

.PHONY: all clean
all: web_server

web_server: $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

web_server.o: web_http.h web_session.h signup.h login.h db_api.h db_internal.h
web_http.o: web_http.h
web_session.o: web_session.h
signup.o: signup.h db_internal.h db_api.h
login.o: login.h db_internal.h db_api.h
db_core.o: db_internal.h db_api.h

clean:
	rm -f web_server $(OBJECTS)
