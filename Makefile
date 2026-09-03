CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -fPIC
PKG_PACKAGES = gkrellm gtk+-2.0 libcurl json-glib-1.0

PKG_CFLAGS = $(shell pkg-config --cflags $(PKG_PACKAGES))
PKG_LIBS = $(shell pkg-config --libs $(PKG_PACKAGES))

TARGET = gmail.so
AUTH_CLI = gkrellm-gmail-auth
TEST_BIN = tests/test_runner

SRCS = gmail.c gmail_api.c
OBJS = $(SRCS:.c=.o)

USER_PLUGIN_DIR = $(HOME)/.gkrellm2/plugins
SYS_PLUGIN_DIR = /usr/lib64/gkrellm2/plugins

all: $(TARGET) $(AUTH_CLI)

$(TARGET): $(OBJS)
	$(CC) -shared -o $@ $(OBJS) $(PKG_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

$(AUTH_CLI): auth_cli.o gmail_api.o
	$(CC) $(CFLAGS) -o $@ auth_cli.o gmail_api.o $(PKG_LIBS)

auth_cli.o: auth_cli.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

$(TEST_BIN): tests/test_suite.c gmail_api.o
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ tests/test_suite.c gmail_api.o $(PKG_LIBS)

test: $(TEST_BIN)
	@echo "Running tests..."
	@./$(TEST_BIN)

install-user: $(TARGET)
	mkdir -p $(USER_PLUGIN_DIR)
	install -m 755 $(TARGET) $(USER_PLUGIN_DIR)
	@echo "Installed $(TARGET) to $(USER_PLUGIN_DIR)"

install: $(TARGET)
	mkdir -p $(DESTDIR)$(SYS_PLUGIN_DIR)
	install -m 755 $(TARGET) $(DESTDIR)$(SYS_PLUGIN_DIR)
	@echo "Installed $(TARGET) to $(DESTDIR)$(SYS_PLUGIN_DIR)"

clean:
	rm -f $(OBJS) auth_cli.o $(TARGET) $(AUTH_CLI) $(TEST_BIN) tests/*.o

.PHONY: all clean test install install-user
