CC = cc
AR = ar
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -Iheader -D_POSIX_C_SOURCE=200809L
THREAD_FLAGS = -pthread

BUILD_DIR := build
SERVER := $(BUILD_DIR)/server
CLIENT := $(BUILD_DIR)/client

# Static libraries must follow their consumers in the link command.
LIBRARIES := $(BUILD_DIR)/libtcp_server.a \
             $(BUILD_DIR)/libfile_util.a \
             $(BUILD_DIR)/libsignal_util.a
OBJECTS := $(BUILD_DIR)/server.o $(BUILD_DIR)/client.o \
           $(BUILD_DIR)/tcp_server.o $(BUILD_DIR)/file_util.o \
           $(BUILD_DIR)/signal_util.o
DEPS := $(OBJECTS:.o=.d)

.PHONY: all server client libs clean
all: server client
server: $(SERVER)
client: $(CLIENT)
libs: $(LIBRARIES)

$(SERVER): $(BUILD_DIR)/server.o $(LIBRARIES)
	$(CC) $(CFLAGS) $(LDFLAGS) $(THREAD_FLAGS) -o $@ $^ $(LDLIBS)

$(CLIENT): $(BUILD_DIR)/client.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(THREAD_FLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/libtcp_server.a: $(BUILD_DIR)/tcp_server.o
	$(AR) rcs $@ $^

$(BUILD_DIR)/libfile_util.a: $(BUILD_DIR)/file_util.o
	$(AR) rcs $@ $^

$(BUILD_DIR)/libsignal_util.a: $(BUILD_DIR)/signal_util.o
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

# Remove only known generated files, not the whole directory.
clean:
	$(RM) $(SERVER) $(CLIENT) $(OBJECTS) $(DEPS) $(LIBRARIES)

-include $(DEPS)
