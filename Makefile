# Makefile

# Compiler and flags
CC = gcc
NO_COVERAGE_FLAGS = -Wall -Wextra -Wno-unused-parameter -I$(SRC_DIR) -I$(TEST_DIR) -I$(SQLITE_DIR) -I$(CURL_DIR)
CFLAGS = $(NO_COVERAGE_FLAGS) -DCLOUDSYNC_OMIT_NETWORK=1 -DCLOUDSYNC_OMIT_PRINT_RESULT=1 -fprofile-arcs -ftest-coverage 
EXTENSION_FLAGS = $(NO_COVERAGE_FLAGS) -O3 -fPIC 

ifeq ($(shell uname -s),Darwin)
CONFIG_DARWIN=y
else ifeq ($(OS),Windows_NT)
CONFIG_WINDOWS=y
else
CONFIG_LINUX=y
endif

ifdef CONFIG_DARWIN
LOADABLE_EXTENSION=dylib
endif

ifdef CONFIG_LINUX
LOADABLE_EXTENSION=so
CFLAGS += -lm
endif

ifdef CONFIG_WINDOWS
LOADABLE_EXTENSION=dll
endif

LDFLAGS = -lcurl

# setting the -lgcov flag on macOS leads to a linker error
ifeq ($(shell uname), Linux)
    LDFLAGS += -lgcov
	DYLIB_LDFLAGS = 
else
    LDFLAGS += 
	DYLIB_LDFLAGS = -dynamiclib
endif

# Directories and files
SRC_DIR = src
TEST_DIR = test
SQLITE_DIR = sqlite
OBJ_DIR = obj
CURL_DIR = network/curl/macos
COV_DIR = coverage
CUSTOM_CSS = $(TEST_DIR)/sqlitecloud.css
TARGET_PREFIX=dist
	
TARGET_NAME=cloudsync
TARGET_LOADABLE=$(TARGET_PREFIX)/$(TARGET_NAME).$(LOADABLE_EXTENSION)
TARGET_STATIC=$(TARGET_PREFIX)/libsqlite_$(TARGET_NAME)0.a
TARGET_STATIC_H=$(TARGET_PREFIX)/sqlite-$(TARGET_NAME).h
TARGET_CLI=$(TARGET_PREFIX)/sqlite3

# Files and objects
LIB_HEADERS = $(wildcard $(SRC_DIR)/*.h) $(wildcard $(SQLITE_DIR)/*.h)
SRC_FILES = $(filter-out $(SQLITE_DIR)/sqlite3.c $(TEST_DIR)/unittest.c $(SRC_DIR)/lz4.c, $(wildcard $(SRC_DIR)/*.c) $(wildcard $(TEST_DIR)/*.c) $(wildcard $(SQLITE_DIR)/*.c))
LIB_OBJ_FILES = $(patsubst %.c, $(TARGET_PREFIX)/$(OBJ_DIR)/%.o, $(notdir $(SRC_FILES)))
LIB_SQLITE_OBJ = $(TARGET_PREFIX)/$(OBJ_DIR)/sqlite3.o

SRC_FILES = $(filter-out $(SQLITE_DIR)/sqlite3.c $(TEST_DIR)/unittest.c $(SRC_DIR)/lz4.c, $(wildcard $(SRC_DIR)/*.c) $(wildcard $(TEST_DIR)/*.c) $(wildcard $(SQLITE_DIR)/*.c))
OBJ_FILES = $(patsubst %.c, $(OBJ_DIR)/%.o, $(notdir $(SRC_FILES)))
LZ4_OBJ = $(OBJ_DIR)/lz4.o
SQLITE_OBJ = $(OBJ_DIR)/sqlite3.o
UNIT_TEST_OBJ = $(OBJ_DIR)/unittest.o
HEADERS = $(wildcard $(SRC_DIR)/*.h) $(wildcard $(TEST_DIR)/*.h) $(wildcard $(SQLITE_DIR)/*.h)

# Default target
extension: $(TARGET_LOADABLE)
all: $(TARGET_LOADABLE) 

# Loadable library
$(TARGET_LOADABLE): $(LIB_OBJ_FILES) $(LIB_SQLITE_OBJ) $(LZ4_OBJ) $(TARGET_PREFIX) 
	$(CC) $(LIB_OBJ_FILES) $(LZ4_OBJ) $(LIB_SQLITE_OBJ) -o $@ $(LDFLAGS) $(DYLIB_LDFLAGS)

# Object files for the lib (with coverage flags)
$(TARGET_PREFIX)/$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(LIB_HEADERS) | $(TARGET_PREFIX)/$(OBJ_DIR)
	$(CC) $(EXTENSION_FLAGS) -c $< -o $@
$(LIB_SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c $(LIB_HEADERS) | $(TARGET_PREFIX)/$(OBJ_DIR)
	$(CC) $(EXTENSION_FLAGS) -c $< -o $@

# Object files (with coverage flags)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(TEST_DIR)/%.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(SQLITE_DIR)/%.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile lz4.c without coverage flags
$(LZ4_OBJ): $(SRC_DIR)/lz4.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(NO_COVERAGE_FLAGS) -c $< -o $@
	
# Compile sqlite3.c without coverage flags
$(SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(NO_COVERAGE_FLAGS) -DSQLITE_CORE=1 -c $< -o $@

# Compile unittest.c without coverage flags
$(UNIT_TEST_OBJ): $(TEST_DIR)/unittest.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create object directory if not exists
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)
$(TARGET_PREFIX)/$(OBJ_DIR):
	mkdir -p $(TARGET_PREFIX)/$(OBJ_DIR)

$(TARGET_PREFIX):
	mkdir -p $(TARGET_PREFIX)

# Build unit test executable
UNIT_TEST = unittest
$(UNIT_TEST): CFLAGS += -DSQLITE_CORE=1 -DCLOUDSYNC_UNITTEST=1
$(UNIT_TEST): $(OBJ_FILES) $(SQLITE_OBJ) $(LZ4_OBJ) $(UNIT_TEST_OBJ)
	$(CC) $(CFLAGS) $(OBJ_FILES) $(LZ4_OBJ) $(SQLITE_OBJ) $(UNIT_TEST_OBJ) -o $@ $(LDFLAGS)

# Run code coverage (--css-file $(CUSTOM_CSS))
coverage: $(UNIT_TEST)
	./$(UNIT_TEST)
	mkdir -p $(COV_DIR)
	lcov --capture --directory . --output-file $(COV_DIR)/coverage.info
	genhtml $(COV_DIR)/coverage.info --output-directory $(COV_DIR)

# Clean up generated files
clean:
	rm -rf $(OBJ_DIR) $(UNIT_TEST) $(COV_DIR) *.gcda *.gcno *.gcov dist/*

.PHONY: all clean coverage extension $(DEPS)
