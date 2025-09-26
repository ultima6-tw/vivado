# Makefile for the AWG Server

# --------------------
#   Compiler & Tools
# --------------------
CC = gcc

# --------------------
#   Project Files
# --------------------
TARGET = awg_server
SOURCES = awg_server_raw_top.c \
          awg_server_raw_direct.c \
          awg_server_raw_queue.c \
          awg_server_raw_notify.c \
          awg_core_mmap.c

# Automatically generate object file names from source file names (e.g., source.c -> source.o)
OBJECTS = $(SOURCES:.c=.o)

# --------------------
#   Compilation Flags
# --------------------
# Flags that are always used
BASE_CFLAGS = -pthread -Wall
LDFLAGS = -pthread

# --- The DEBUG Switch Magic ---
# By default, DEBUG is not set to 1, so it will build a Release version.
# To build a Debug version, you run: make DEBUG=1
CFLAGS = $(BASE_CFLAGS)
ifeq ($(DEBUG), 1)
    # Debug Mode: Add debugging symbols (-g) and the -DDEBUG macro
    CFLAGS += -g -DDEBUG
    INFO = (Debug Build)
else
    # Release Mode: Add optimizations (-O2) and strip symbols (-s) for a smaller executable
    CFLAGS += -O2
    LDFLAGS += -s
    INFO = (Release Build)
endif

# --------------------
#   Build Rules
# --------------------
.PHONY: all clean

# Default target: build the executable
all: $(TARGET)
	@echo "--- Build successful for $(TARGET) $(INFO) ---"

# Rule to link the object files into the final executable
# $@ is the target name (awg_server)
# $^ is the list of all prerequisites (all .o files)
$(TARGET): $(OBJECTS)
	@echo "Linking..."
	$(CC) $^ -o $@ $(LDFLAGS)

# Pattern rule to compile a .c source file into a .o object file
# $< is the first prerequisite (the .c file)
%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Target to clean up compiled files
clean:
	@echo "Cleaning up..."
	rm -f $(TARGET) $(OBJECTS)