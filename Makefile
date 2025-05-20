# Makefile for SQLite Sync Extension
# Supports compilation for Linux, macOS, Windows, Android and iOS

# Set default platform if not specified
ifeq ($(OS),Windows_NT)
    PLATFORM := windows
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM := macos
    else
        PLATFORM := linux
    endif
endif

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -I$(SRC_DIR) -I$(SQLITE_DIR) -I$(CURL_DIR)/include
TEST_FLAGS = $(CFLAGS) -DSQLITE_CORE -DCLOUDSYNC_UNITTEST -DCLOUDSYNC_OMIT_NETWORK -DCLOUDSYNC_OMIT_PRINT_RESULT -fprofile-arcs -ftest-coverage
EXTENSION_FLAGS = $(CFLAGS) -O3 -fPIC
LDFLAGS = -L./$(CURL_DIR)/$(PLATFORM) -lcurl
COVERAGE = false

# Directories
SRC_DIR = src
DIST_DIR = dist
TEST_DIR = test
SQLITE_DIR = sqlite
VPATH = $(SRC_DIR):$(SQLITE_DIR):$(TEST_DIR)
BUILD_RELEASE = build/release
BUILD_TEST = build/test
BUILD_DIRS = $(BUILD_TEST) $(BUILD_RELEASE)
CURL_DIR = network/curl
COV_DIR = coverage
CUSTOM_CSS = $(TEST_DIR)/sqliteai.css

# Files and objects
ifeq ($(PLATFORM),windows)
    TEST_TARGET := $(DIST_DIR)/test.exe
else
    TEST_TARGET := $(DIST_DIR)/test
endif
SRC_FILES = $(wildcard $(SRC_DIR)/*.c)
TEST_FILES = $(SRC_FILES) $(wildcard $(TEST_DIR)/*.c) $(wildcard $(SQLITE_DIR)/*.c)
RELEASE_OBJ = $(patsubst %.c, $(BUILD_RELEASE)/%.o, $(notdir $(SRC_FILES)))
TEST_OBJ = $(patsubst %.c, $(BUILD_TEST)/%.o, $(notdir $(TEST_FILES)))
COV_FILES = $(filter-out $(SRC_DIR)/lz4.c $(SRC_DIR)/network.c, $(SRC_FILES))

# Platform-specific settings
ifeq ($(PLATFORM),windows)
    TARGET := $(DIST_DIR)/cloudsync.dll
    LDFLAGS += -shared -lws2_32
    # Create .def file for Windows
    DEF_FILE := $(BUILD_RELEASE)/cloudsync.def
    CFLAGS += -DCURL_STATICLIB
else ifeq ($(PLATFORM),macos)
    TARGET := $(DIST_DIR)/cloudsync.dylib
    LDFLAGS += -arch x86_64 -arch arm64 -framework Security -dynamiclib -undefined dynamic_lookup
    # macOS-specific flags
    CFLAGS += -arch x86_64 -arch arm64
else ifeq ($(PLATFORM),android)
    # Use Android NDK's Clang compiler, the user should set the CC
    # example CC=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang
    ifeq ($(filter %-clang,$(CC)),)
        $(error "CC must be set to the Android NDK's Clang compiler")
    endif
    TARGET := $(DIST_DIR)/cloudsync.so
    LDFLAGS += -shared -lm -lssl -lcrypto
    # Android-specific flags
    CFLAGS += -D__ANDROID__
else ifeq ($(PLATFORM),ios)
    TARGET := $(DIST_DIR)/cloudsync.dylib
    SDK := -isysroot $(shell xcrun --sdk iphoneos --show-sdk-path) -miphoneos-version-min=11.0
    LDFLAGS += -framework Security -dynamiclib $(SDK)
    # iOS-specific flags
    CFLAGS += -arch arm64 $(SDK)
else ifeq ($(PLATFORM),isim)
    TARGET := $(DIST_DIR)/cloudsync.dylib
    SDK := -isysroot $(shell xcrun --sdk iphonesimulator --show-sdk-path) -miphonesimulator-version-min=11.0
    LDFLAGS += -arch x86_64 -arch arm64 -framework Security -dynamiclib $(SDK)
    # iphonesimulator-specific flags
    CFLAGS += -arch x86_64 -arch arm64 $(SDK)
else # linux
    TARGET := $(DIST_DIR)/cloudsync.so
    LDFLAGS += -shared -lssl -lcrypto
    CFLAGS += -lm
    TEST_FLAGS += -lgcov
endif

# Windows .def file generation
$(DEF_FILE):
ifeq ($(PLATFORM),windows)
	@echo "LIBRARY js.dll" > $@
	@echo "EXPORTS" >> $@
	@echo "    sqlite3_cloudsync_init" >> $@
	@echo "    cloudsync_config_exists" >> $@
	@echo "    cloudsync_context_init" >> $@
	@echo "    cloudsync_merge_insert" >> $@
	@echo "    cloudsync_sync_key" >> $@
	@echo "    cloudsync_sync_table_key" >> $@
	@echo "    cloudsync_get_auxdata" >> $@
	@echo "    cloudsync_set_auxdata" >> $@
	@echo "    cloudsync_payload_apply" >> $@
endif

# Make sure the build and dist directories exist
$(shell mkdir -p $(BUILD_DIRS) $(DIST_DIR))

# Default target
extension: $(TARGET)
all: $(TARGET) 

# Loadable library
$(TARGET): $(RELEASE_OBJ) $(DEF_FILE)
	$(CC) $? -o $@ $(LDFLAGS)
ifeq ($(PLATFORM),windows)
    # Generate import library for Windows
	dlltool -D $@ -d $(DEF_FILE) -l $(DIST_DIR)/js.lib
endif

# Test executable
$(TEST_TARGET): $(TEST_OBJ)
	$(CC) $(TEST_FLAGS) $? -o $@

# Object files
$(BUILD_RELEASE)/%.o: %.c
	$(CC) $(EXTENSION_FLAGS) -c $< -o $@
$(BUILD_TEST)/sqlite3.o: $(SQLITE_DIR)/sqlite3.c
	$(CC) $(CFLAGS) -DSQLITE_CORE=1 -c $< -o $@
$(BUILD_TEST)/%.o: %.c
	$(CC) $(TEST_FLAGS) -c $< -o $@

# Run code coverage (--css-file $(CUSTOM_CSS))
test: $(TARGET) $(TEST_TARGET)
	sqlite3 ":memory:" -cmd ".bail on" ".load ./$<" "SELECT cloudsync_version();"
	./$(TEST_TARGET)
ifneq ($(COVERAGE),false)
	mkdir -p $(COV_DIR)
	lcov --capture --directory . --output-file $(COV_DIR)/coverage.info $(subst src, --include src,${COV_FILES})
	genhtml $(COV_DIR)/coverage.info --output-directory $(COV_DIR)
endif

# Clean up generated files
clean:
	rm -rf $(BUILD_DIRS) $(DIST_DIR)/* $(COV_DIR) *.gcda *.gcno *.gcov

# Help message
help:
	@echo "SQLite Sync Extension Makefile"
	@echo "Usage:"
	@echo "  make [PLATFORM=platform] [target]"
	@echo ""
	@echo "Platforms:"
	@echo "  linux (default on Linux)"
	@echo "  macos (default on macOS)"
	@echo "  windows (default on Windows)"
	@echo "  android (needs CC to be set to Android NDK's Clang compiler)"
	@echo "  ios (only on macOS)"
	@echo "  isim (only on macOS)"
	@echo ""
	@echo "Targets:"
	@echo "  all       				- Build the extension (default)"
	@echo "  clean     				- Remove built files"
	@echo "  test [COVERAGE=true]	- Test the extension with optional coverage output"
	@echo "  help      				- Display this help message"

.PHONY: all clean test extension help
