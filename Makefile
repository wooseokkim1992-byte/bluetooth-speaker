
CC = cc
AR = ar
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -Iheader -D_POSIX_C_SOURCE=200809L
DB_CPPFLAGS += $(shell mariadb_config --cflags)
THREAD_FLAGS = -pthread

BUILD_DIR := build
SERVER := $(BUILD_DIR)/server
CLIENT := $(BUILD_DIR)/client

LDLIBS += $(shell mariadb_config --libs) -lm

# Static libraries must follow their consumers in the link command.
LIBRARIES := $(BUILD_DIR)/libtcp_server.a \
             $(BUILD_DIR)/libtcp_interface.a \
             $(BUILD_DIR)/libfile_util.a \
             $(BUILD_DIR)/libsignal_util.a \
			 $(BUILD_DIR)/libdb_admin.a
OBJECTS := $(BUILD_DIR)/server.o $(BUILD_DIR)/client.o \
           $(BUILD_DIR)/tcp_server.o $(BUILD_DIR)/tcp_interface.o \
           $(BUILD_DIR)/file_util.o \
           $(BUILD_DIR)/signal_util.o \
			$(BUILD_DIR)/db_admin.o

DEPS := $(OBJECTS:.o=.d)

.PHONY: all server client libs clean
all: server client 
server: $(SERVER)
client: $(CLIENT)
libs: $(LIBRARIES)


$(SERVER): $(BUILD_DIR)/server.o $(LIBRARIES)
	$(CC) $(CFLAGS) $(LDFLAGS) $(THREAD_FLAGS) -o $@ $^ $(LDLIBS)

$(CLIENT): $(BUILD_DIR)/client.o $(BUILD_DIR)/libtcp_interface.a
	$(CC) $(CFLAGS) $(LDFLAGS) $(THREAD_FLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/libdb_admin.a: $(BUILD_DIR)/db_core.o $(BUILD_DIR)/db_member.o $(BUILD_DIR)/db_device.o $(BUILD_DIR)/db_song.o 
	$(AR) rcs $@ $^

$(BUILD_DIR)/libtcp_server.a: $(BUILD_DIR)/tcp_server.o
	$(AR) rcs $@ $^

$(BUILD_DIR)/libtcp_interface.a: $(BUILD_DIR)/tcp_interface.o
	$(AR) rcs $@ $^

$(BUILD_DIR)/libfile_util.a: $(BUILD_DIR)/file_util.o
	$(AR) rcs $@ $^

$(BUILD_DIR)/libsignal_util.a: $(BUILD_DIR)/signal_util.o
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: DB/%.c 
	$(CC) $(DB_CPPFLAGS) -Iheader -std=c11 -Wall -Wextra -Wpedantic -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

# Remove only known generated files, not the whole directory.
clean:
	$(RM) $(SERVER) $(CLIENT) $(OBJECTS) $(DEPS) $(LIBRARIES)

-include $(DEPS)
