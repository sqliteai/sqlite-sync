# Makefile for SQLite Sync Extension
# Supports compilation for Linux, macOS, Windows, Android and iOS

# customize sqlite3 executable with 
# make test SQLITE3=/opt/homebrew/Cellar/sqlite/3.49.1/bin/sqlite3
SQLITE3 ?= sqlite3

# set curl version to download and build
CURL_VERSION ?= 8.12.1

# set sqlite version for WASM static build
SQLITE_VERSION ?= 3.49.2

# Set default platform if not specified
ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    HOST := windows
    CPUS := $(shell powershell -Command "[Environment]::ProcessorCount")
else
    HOST = $(shell uname -s | tr '[:upper:]' '[:lower:]')
    ifeq ($(HOST),darwin)
        PLATFORM := macos
        CPUS := $(shell sysctl -n hw.ncpu)
    else
        PLATFORM := $(HOST)
        CPUS := $(shell nproc)
    endif
endif

# Speed up builds by using all available CPU cores
MAKEFLAGS += -j$(CPUS)

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -I$(SRC_DIR) -I$(SQLITE_DIR) -I$(CURL_DIR)/include
T_CFLAGS = $(CFLAGS) -DSQLITE_CORE -DCLOUDSYNC_UNITTEST -DCLOUDSYNC_OMIT_NETWORK -DCLOUDSYNC_OMIT_PRINT_RESULT
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
CURL_DIR = curl
CURL_SRC = $(CURL_DIR)/src/curl-$(CURL_VERSION)
COV_DIR = coverage
CUSTOM_CSS = $(TEST_DIR)/sqliteai.css
BUILD_WASM = build/wasm

SRC_FILES = $(wildcard $(SRC_DIR)/*.c)
TEST_SRC = $(wildcard $(TEST_DIR)/*.c)
TEST_FILES = $(SRC_FILES) $(TEST_SRC) $(wildcard $(SQLITE_DIR)/*.c)
RELEASE_OBJ = $(patsubst %.c, $(BUILD_RELEASE)/%.o, $(notdir $(SRC_FILES)))
TEST_OBJ = $(patsubst %.c, $(BUILD_TEST)/%.o, $(notdir $(TEST_FILES)))
COV_FILES = $(filter-out $(SRC_DIR)/lz4.c $(SRC_DIR)/network.c $(SRC_DIR)/wasm.c, $(SRC_FILES))
CURL_LIB = $(CURL_DIR)/$(PLATFORM)/libcurl.a
TEST_TARGET = $(patsubst %.c,$(DIST_DIR)/%$(EXE), $(notdir $(TEST_SRC)))

# Platform-specific settings
ifeq ($(PLATFORM),windows)
    TARGET := $(DIST_DIR)/cloudsync.dll
    LDFLAGS += -shared -lbcrypt -lcrypt32 -lsecur32 -lws2_32
    T_LDFLAGS = -lws2_32 -lbcrypt
    # Create .def file for Windows
    DEF_FILE := $(BUILD_RELEASE)/cloudsync.def
    CFLAGS += -DCURL_STATICLIB
    CURL_CONFIG = --with-schannel CFLAGS="-DCURL_STATICLIB"
    EXE = .exe
else ifeq ($(PLATFORM),macos)
    TARGET := $(DIST_DIR)/cloudsync.dylib
    LDFLAGS += -arch x86_64 -arch arm64 -framework Security -dynamiclib -undefined dynamic_lookup
    T_LDFLAGS = -framework Security
    CFLAGS += -arch x86_64 -arch arm64
    CURL_CONFIG = --with-secure-transport CFLAGS="-arch x86_64 -arch arm64"
else ifeq ($(PLATFORM),android)
    # Set ARCH to find Android NDK's Clang compiler, the user should set the ARCH
    ifeq ($(filter %,$(ARCH)),)
        $(error "Android ARCH must be set to ARCH=x86_64 or ARCH=arm64-v8a")
    endif
    # Set ANDROID_NDK path to find android build tools
    # e.g. on MacOS: export ANDROID_NDK=/Users/username/Library/Android/sdk/ndk/25.2.9519653
    ifeq ($(filter %,$(ANDROID_NDK)),)
        $(error "Android NDK must be set")
    endif

    BIN = $(ANDROID_NDK)/toolchains/llvm/prebuilt/$(HOST)-x86_64/bin
    PATH := $(BIN):$(PATH)

    ifneq (,$(filter $(ARCH),arm64 arm64-v8a))
        override ARCH := aarch64
    endif

    OPENSSL := $(BIN)/../sysroot/usr/include/openssl
    CC = $(BIN)/$(ARCH)-linux-android26-clang
    CURL_CONFIG = --host $(ARCH)-$(HOST)-android26 --with-openssl=$(BIN)/../sysroot/usr LIBS="-lssl -lcrypto" AR=$(BIN)/llvm-ar AS=$(BIN)/llvm-as CC=$(CC) CXX=$(BIN)/$(ARCH)-linux-android26-clang++ LD=$(BIN)/ld RANLIB=$(BIN)/llvm-ranlib STRIP=$(BIN)/llvm-strip
    TARGET := $(DIST_DIR)/cloudsync.so
    LDFLAGS += -shared -lcrypto -lssl
else ifeq ($(PLATFORM),ios)
    TARGET := $(DIST_DIR)/cloudsync.dylib
    SDK := -isysroot $(shell xcrun --sdk iphoneos --show-sdk-path) -miphoneos-version-min=11.0
    LDFLAGS += -framework Security -framework CoreFoundation -dynamiclib $(SDK)
    T_LDFLAGS = -framework Security
    CFLAGS += -arch arm64 $(SDK)
    CURL_CONFIG = --host=arm64-apple-darwin --with-secure-transport CFLAGS="-arch arm64 -isysroot $$(xcrun --sdk iphoneos --show-sdk-path) -miphoneos-version-min=11.0"
else ifeq ($(PLATFORM),isim)
    TARGET := $(DIST_DIR)/cloudsync.dylib
    SDK := -isysroot $(shell xcrun --sdk iphonesimulator --show-sdk-path) -miphonesimulator-version-min=11.0
    LDFLAGS += -arch x86_64 -arch arm64 -framework Security -framework CoreFoundation -dynamiclib $(SDK)
    T_LDFLAGS = -framework Security
    CFLAGS += -arch x86_64 -arch arm64 $(SDK)
    CURL_CONFIG = --host=arm64-apple-darwin --with-secure-transport CFLAGS="-arch x86_64 -arch arm64 -isysroot $$(xcrun --sdk iphonesimulator --show-sdk-path) -miphonesimulator-version-min=11.0"
else ifeq ($(PLATFORM),wasm)
    TARGET := $(DIST_DIR)/sqlite-wasm.zip
else # linux
    TARGET := $(DIST_DIR)/cloudsync.so
    LDFLAGS += -shared -lssl -lcrypto
    CURL_CONFIG = --with-openssl
endif

ifneq ($(COVERAGE),false)
ifneq (,$(filter $(platform),linux windows))
    T_LDFLAGS += -lgcov
endif
    T_CFLAGS += -fprofile-arcs -ftest-coverage
    T_LDFLAGS += -fprofile-arcs -ftest-coverage
endif

# Windows .def file generation
$(DEF_FILE):
ifeq ($(PLATFORM),windows)
	@echo "LIBRARY cloudsync.dll" > $@
	@echo "EXPORTS" >> $@
	@echo "    sqlite3_cloudsync_init" >> $@
endif

# Make sure the build and dist directories exist
$(shell mkdir -p $(BUILD_DIRS) $(DIST_DIR))

# Default target
extension: $(TARGET)
all: $(TARGET) 

ifneq ($(PLATFORM),wasm)
# Loadable library
$(TARGET): $(RELEASE_OBJ) $(DEF_FILE) $(CURL_LIB)
	$(CC) $(RELEASE_OBJ) $(DEF_FILE) -o $@ $(LDFLAGS)
ifeq ($(PLATFORM),windows)
    # Generate import library for Windows
	dlltool -D $@ -d $(DEF_FILE) -l $(DIST_DIR)/cloudsync.lib
endif
else
#WASM build
EMSDK := $(BUILD_WASM)/emsdk
$(EMSDK):
	git clone https://github.com/emscripten-core/emsdk.git $(EMSDK)
	cd $(EMSDK) && ./emsdk install latest && ./emsdk activate latest

SQLITE_SRC := $(BUILD_WASM)/sqlite
$(SQLITE_SRC): $(EMSDK)
	git clone --branch version-$(SQLITE_VERSION) --depth 1 https://github.com/sqlite/sqlite.git $(SQLITE_SRC)
	cd $(EMSDK) && . ./emsdk_env.sh && cd ../sqlite && ./configure --enable-all

WASM_FLAGS = emcc.jsflags += -sFETCH -pthread
WASM_MAKEFILE = $(SQLITE_SRC)/ext/wasm/GNUMakefile
$(TARGET): $(SQLITE_SRC) $(SRC_FILES)
	@grep '$(WASM_FLAGS)' '$(WASM_MAKEFILE)' >/dev/null 2>&1 || echo '$(WASM_FLAGS)' >> '$(WASM_MAKEFILE)'
	cd $(SQLITE_SRC)/ext/wasm && $(MAKE) dist sqlite3_wasm_extra_init.c=../../../../../src/wasm.c
	mv $(SQLITE_SRC)/ext/wasm/sqlite-wasm-*.zip $(TARGET)
endif

# Test executable
$(TEST_TARGET): $(TEST_OBJ)
	$(CC) $(filter-out $(patsubst $(DIST_DIR)/%$(EXE),$(BUILD_TEST)/%.o, $(filter-out $@,$(TEST_TARGET))), $(TEST_OBJ)) -o $@ $(T_LDFLAGS)

# Object files
$(BUILD_RELEASE)/%.o: %.c
	$(CC) $(CFLAGS) -O3 -fPIC -c $< -o $@
$(BUILD_TEST)/sqlite3.o: $(SQLITE_DIR)/sqlite3.c
	$(CC) $(CFLAGS) -DSQLITE_CORE -c $< -o $@
$(BUILD_TEST)/%.o: %.c
	$(CC) $(T_CFLAGS) -c $< -o $@

# Run code coverage (--css-file $(CUSTOM_CSS))
test: $(TARGET) $(TEST_TARGET)
	$(SQLITE3) ":memory:" -cmd ".bail on" ".load ./$<" "SELECT cloudsync_version();"
	set -e; for t in $(TEST_TARGET); do ./$$t; done
ifneq ($(COVERAGE),false)
	mkdir -p $(COV_DIR)
	lcov --capture --directory . --output-file $(COV_DIR)/coverage.info $(subst src, --include src,${COV_FILES})
	genhtml $(COV_DIR)/coverage.info --output-directory $(COV_DIR)
endif

$(OPENSSL):
	git clone https://github.com/openssl/openssl.git $(CURL_DIR)/src/openssl

	cd $(CURL_DIR)/src/openssl && \
	./Configure android-$(if $(filter aarch64,$(ARCH)),arm64,$(ARCH)) \
	    --prefix=$(BIN)/../sysroot/usr \
	    no-shared no-unit-test \
	    -D__ANDROID_API__=26 && \
	$(MAKE) && $(MAKE) install_sw

ifeq ($(PLATFORM),android)
$(CURL_LIB): $(OPENSSL)
else
$(CURL_LIB):
endif
	mkdir -p $(CURL_DIR)/src
	curl -L -o $(CURL_DIR)/src/curl.zip "https://github.com/curl/curl/releases/download/curl-$(subst .,_,${CURL_VERSION})/curl-$(CURL_VERSION).zip"

    ifeq ($(HOST),windows)
	powershell -Command "Expand-Archive -Path '$(CURL_DIR)\src\curl.zip' -DestinationPath '$(CURL_DIR)\src\'"
    else
	unzip $(CURL_DIR)/src/curl.zip -d $(CURL_DIR)/src/.
    endif
	
	cd $(CURL_SRC) && ./configure \
	--without-libpsl \
	--disable-alt-svc \
	--disable-ares \
	--disable-cookies \
	--disable-basic-auth \
	--disable-digest-auth \
	--disable-kerberos-auth \
	--disable-negotiate-auth \
	--disable-aws \
	--disable-dateparse \
	--disable-dnsshuffle \
	--disable-doh \
	--disable-form-api \
	--disable-hsts \
	--disable-ipv6 \
	--disable-libcurl-option \
	--disable-manual \
	--disable-mime \
	--disable-netrc \
	--disable-ntlm \
	--disable-ntlm-wb \
	--disable-progress-meter \
	--disable-proxy \
	--disable-pthreads \
	--disable-socketpair \
	--disable-threaded-resolver \
	--disable-tls-srp \
	--disable-verbose \
	--disable-versioned-symbols \
	--enable-symbol-hiding \
	--without-brotli \
	--without-zstd \
	--without-libidn2 \
	--without-librtmp \
	--without-zlib \
	--without-nghttp2 \
	--without-ngtcp2 \
	--disable-shared \
	--disable-ftp \
	--disable-file \
	--disable-ipfs \
	--disable-ldap \
	--disable-ldaps \
	--disable-rtsp \
	--disable-dict \
	--disable-telnet \
	--disable-tftp \
	--disable-pop3 \
	--disable-imap \
	--disable-smb \
	--disable-smtp \
	--disable-gopher \
	--disable-mqtt \
	--disable-docs \
	--enable-static \
	$(CURL_CONFIG)

	# save avg 1kb more with these options
	# --disable-debug \
	# --enable-optimize \
	# --disable-curldebug \
	# --disable-get-easy-options \
	# --without-fish-functions-dir \
	# --without-zsh-functions-dir \
	# --without-libgsasl \
	
	cd $(CURL_SRC) && $(MAKE)

	mkdir -p $(CURL_DIR)/$(PLATFORM)
	mv $(CURL_SRC)/lib/.libs/libcurl.a $(CURL_DIR)/$(PLATFORM)
	rm -rf $(CURL_DIR)/src

# Tools
sqlite_version: 
	@echo $(SQLITE_VERSION)
version:
	@echo $(shell sed -n 's/^#define CLOUDSYNC_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' src/cloudsync.h)

# Clean up generated files
clean:
	rm -rf $(BUILD_DIRS) $(DIST_DIR)/* $(COV_DIR) *.gcda *.gcno *.gcov $(CURL_DIR)/src *.sqlite $(BUILD_WASM)

# Help message
help:
	@echo "SQLite Sync Extension Makefile"
	@echo "Usage:"
	@echo "  make [PLATFORM=platform] [ARCH=arch] [ANDROID_NDK=\$$ANDROID_HOME/ndk/26.1.10909125] [target]"
	@echo ""
	@echo "Platforms:"
	@echo "  linux (default on Linux)"
	@echo "  macos (default on macOS)"
	@echo "  windows (default on Windows)"
	@echo "  android (needs ARCH to be set to x86_64 or arm64-v8a and ANDROID_NDK to be set)"
	@echo "  ios (only on macOS)"
	@echo "  isim (only on macOS)"
	@echo "  wasm (needs wabt[brew install wabt/sudo apt install wabt])"
	@echo ""
	@echo "Targets:"
	@echo "  all       				- Build the extension (default)"
	@echo "  clean     				- Remove built files"
	@echo "  test [COVERAGE=true]	- Test the extension with optional coverage output"
	@echo "  help      				- Display this help message"

.PHONY: all clean test extension help
