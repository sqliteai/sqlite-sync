# Makefile for SQLite Sync Extension
# Supports compilation for Linux, macOS, Windows, Android and iOS

# customize sqlite3 executable with 
# make test SQLITE3=/opt/homebrew/Cellar/sqlite/3.49.1/bin/sqlite3
SQLITE3 ?= sqlite3

# set curl version to download and build
CURL_VERSION ?= 8.12.1

# set mbedTLS version for Android (3.6.x is LTS, 4.x has breaking API changes)
MBEDTLS_VERSION ?= 3.6.5

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
OPT_LEVEL = -O3
CFLAGS = -Wall -Wextra -Wno-unused-parameter -I$(SRC_DIR) -I$(SQLITE_DIR) -I$(CURL_DIR)/include
T_CFLAGS = $(CFLAGS) -DSQLITE_CORE -DCLOUDSYNC_UNITTEST -DCLOUDSYNC_OMIT_NETWORK -DCLOUDSYNC_OMIT_PRINT_RESULT
COVERAGE = false
ifndef NATIVE_NETWORK
	LDFLAGS = -L./$(dir $(CURL_LIB)) -lcurl
endif

# Directories
SRC_DIR = src
DIST_DIR = dist
TEST_DIR = test
SQLITE_DIR = sqlite
VPATH = $(SRC_DIR):$(SQLITE_DIR):$(TEST_DIR)
BUILD_RELEASE = build/release
BUILD_TEST = build/test
BUILD_DIRS = $(BUILD_TEST) $(BUILD_RELEASE)
MBEDTLS_DIR = mbedtls
CURL_DIR = curl
CURL_SRC = $(CURL_DIR)/src/curl-$(CURL_VERSION)
CURL_ZIP = $(CURL_DIR)/src/curl-$(CURL_VERSION).zip
COV_DIR = coverage
CUSTOM_CSS = $(TEST_DIR)/sqliteai.css

# Android SSL library installation directory
ifeq ($(PLATFORM),android)
	MBEDTLS_INSTALL_DIR = $(MBEDTLS_DIR)/$(PLATFORM)/$(ARCH)
endif

SRC_FILES = $(wildcard $(SRC_DIR)/*.c)
TEST_SRC = $(wildcard $(TEST_DIR)/*.c)
TEST_FILES = $(SRC_FILES) $(TEST_SRC) $(wildcard $(SQLITE_DIR)/*.c)
RELEASE_OBJ = $(patsubst %.c, $(BUILD_RELEASE)/%.o, $(notdir $(SRC_FILES)))
TEST_OBJ = $(patsubst %.c, $(BUILD_TEST)/%.o, $(notdir $(TEST_FILES)))
COV_FILES = $(filter-out $(SRC_DIR)/lz4.c $(SRC_DIR)/network.c, $(SRC_FILES))
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
	STRIP = strip --strip-unneeded $@
else ifeq ($(PLATFORM),macos)
	TARGET := $(DIST_DIR)/cloudsync.dylib
	ifndef ARCH
		LDFLAGS += -arch x86_64 -arch arm64
		CFLAGS += -arch x86_64 -arch arm64
		CURL_CONFIG = --with-secure-transport CFLAGS="-arch x86_64 -arch arm64"
	else
		LDFLAGS += -arch $(ARCH)
		CFLAGS += -arch $(ARCH)
		CURL_CONFIG = --with-secure-transport CFLAGS="-arch $(ARCH)"
	endif
	LDFLAGS += -framework Security -dynamiclib -undefined dynamic_lookup -headerpad_max_install_names
	T_LDFLAGS = -framework Security
	STRIP = strip -x -S $@
else ifeq ($(PLATFORM),android)
	ifndef ARCH # Set ARCH to find Android NDK's Clang compiler, the user should set the ARCH
		$(error "Android ARCH must be set to ARCH=x86_64, ARCH=arm64-v8a, or ARCH=armeabi-v7a")
	endif
	ifndef ANDROID_NDK # Set ANDROID_NDK path to find android build tools; e.g. on MacOS: export ANDROID_NDK=/Users/username/Library/Android/sdk/ndk/25.2.9519653
		$(error "Android NDK must be set")
	endif

	BIN = $(ANDROID_NDK)/toolchains/llvm/prebuilt/$(HOST)-x86_64/bin
	PATH := $(BIN):$(PATH)

	ifneq (,$(filter $(ARCH),arm64 arm64-v8a))
		override ARCH := aarch64
		ANDROID_ABI := android26
	else ifeq ($(ARCH),armeabi-v7a)
		override ARCH := armv7a
		ANDROID_ABI := androideabi26
	else
		ANDROID_ABI := android26
	endif

	MBEDTLS := $(MBEDTLS_INSTALL_DIR)/lib/libmbedtls.a
	CC = $(BIN)/$(ARCH)-linux-$(ANDROID_ABI)-clang
	CURL_LIB = $(CURL_DIR)/$(PLATFORM)/$(ARCH)/libcurl.a
	CURL_CONFIG = --host $(ARCH)-linux-$(ANDROID_ABI) --with-mbedtls=$(CURDIR)/$(MBEDTLS_INSTALL_DIR) LDFLAGS="-L$(CURDIR)/$(MBEDTLS_INSTALL_DIR)/lib" LIBS="-lmbedtls -lmbedx509 -lmbedcrypto" AR=$(BIN)/llvm-ar AS=$(BIN)/llvm-as CC=$(CC) CXX=$(BIN)/$(ARCH)-linux-$(ANDROID_ABI)-clang++ LD=$(BIN)/ld RANLIB=$(BIN)/llvm-ranlib STRIP=$(BIN)/llvm-strip
	TARGET := $(DIST_DIR)/cloudsync.so
	OPT_LEVEL = -Os
	CFLAGS += -fPIC -I$(MBEDTLS_INSTALL_DIR)/include -ffunction-sections -fdata-sections -flto
	LDFLAGS += -shared -fPIC -L$(MBEDTLS_INSTALL_DIR)/lib -lmbedtls -lmbedx509 -lmbedcrypto -Wl,-z,max-page-size=16384 -Wl,--gc-sections -flto
	STRIP = $(BIN)/llvm-strip --strip-unneeded $@
else ifeq ($(PLATFORM),ios)
	TARGET := $(DIST_DIR)/cloudsync.dylib
	SDK := -isysroot $(shell xcrun --sdk iphoneos --show-sdk-path) -miphoneos-version-min=11.0
	LDFLAGS += -framework Security -framework CoreFoundation -dynamiclib $(SDK) -headerpad_max_install_names
	T_LDFLAGS = -framework Security
	CFLAGS += -arch arm64 $(SDK)
	CURL_CONFIG = --host=arm64-apple-darwin --with-secure-transport CFLAGS="-arch arm64 -isysroot $$(xcrun --sdk iphoneos --show-sdk-path) -miphoneos-version-min=11.0"
	STRIP = strip -x -S $@
else ifeq ($(PLATFORM),ios-sim)
	TARGET := $(DIST_DIR)/cloudsync.dylib
	SDK := -isysroot $(shell xcrun --sdk iphonesimulator --show-sdk-path) -miphonesimulator-version-min=11.0
	LDFLAGS += -arch x86_64 -arch arm64 -framework Security -framework CoreFoundation -dynamiclib $(SDK) -headerpad_max_install_names
	T_LDFLAGS = -framework Security
	CFLAGS += -arch x86_64 -arch arm64 $(SDK)
	CURL_CONFIG = --host=arm64-apple-darwin --with-secure-transport CFLAGS="-arch x86_64 -arch arm64 -isysroot $$(xcrun --sdk iphonesimulator --show-sdk-path) -miphonesimulator-version-min=11.0"
	STRIP = strip -x -S $@
else # linux
	TARGET := $(DIST_DIR)/cloudsync.so
	LDFLAGS += -shared -lssl -lcrypto
	T_LDFLAGS += -lpthread
	CURL_CONFIG = --with-openssl
	STRIP = strip --strip-unneeded $@
endif

ifneq ($(COVERAGE),false)
ifneq (,$(filter $(platform),linux windows))
	T_LDFLAGS += -lgcov
endif
	T_CFLAGS += -fprofile-arcs -ftest-coverage
	T_LDFLAGS += -fprofile-arcs -ftest-coverage
endif

# Native network support only for Apple platforms
ifdef NATIVE_NETWORK
	RELEASE_OBJ += $(patsubst %.m, $(BUILD_RELEASE)/%_m.o, $(notdir $(wildcard $(SRC_DIR)/*.m)))
	LDFLAGS += -framework Foundation
	CFLAGS += -DCLOUDSYNC_OMIT_CURL

$(BUILD_RELEASE)/%_m.o: %.m
	$(CC) $(CFLAGS) -fobjc-arc -O3 -fPIC -c $< -o $@
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

# Loadable library
ifdef NATIVE_NETWORK
$(TARGET): $(RELEASE_OBJ) $(DEF_FILE)
else
$(TARGET): $(RELEASE_OBJ) $(DEF_FILE) $(CURL_LIB)
endif
	$(CC) $(RELEASE_OBJ) $(DEF_FILE) -o $@ $(LDFLAGS)
ifeq ($(PLATFORM),windows)
	# Generate import library for Windows
	dlltool -D $@ -d $(DEF_FILE) -l $(DIST_DIR)/cloudsync.lib
endif
	# Strip debug symbols
	$(STRIP)

# Test executable
$(TEST_TARGET): $(TEST_OBJ)
	$(CC) $(filter-out $(patsubst $(DIST_DIR)/%$(EXE),$(BUILD_TEST)/%.o, $(filter-out $@,$(TEST_TARGET))), $(TEST_OBJ)) -o $@ $(T_LDFLAGS)

# Object files
$(BUILD_RELEASE)/%.o: %.c
	$(CC) $(CFLAGS) $(OPT_LEVEL) -fPIC -c $< -o $@
$(BUILD_TEST)/sqlite3.o: $(SQLITE_DIR)/sqlite3.c
	$(CC) $(CFLAGS) -DSQLITE_DQS=0 -DSQLITE_CORE -c $< -o $@
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

# mbedTLS for Android - minimal TLS library (much smaller than OpenSSL)
# Matches rustls capabilities: TLS 1.2/1.3, AES-GCM, ChaCha20-Poly1305, ECDHE
MBEDTLS_TARBALL = $(MBEDTLS_DIR)/mbedtls-$(MBEDTLS_VERSION).tar.bz2

$(MBEDTLS_TARBALL):
	mkdir -p $(MBEDTLS_DIR)
	curl -L -o $(MBEDTLS_TARBALL) https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBEDTLS_VERSION)/mbedtls-$(MBEDTLS_VERSION).tar.bz2

$(MBEDTLS): $(MBEDTLS_TARBALL)
	mkdir -p $(MBEDTLS_DIR)
	tar -xjf $(MBEDTLS_TARBALL) -C $(MBEDTLS_DIR)
	mkdir -p $(MBEDTLS_DIR)/mbedtls-$(MBEDTLS_VERSION)/build
	cd $(MBEDTLS_DIR)/mbedtls-$(MBEDTLS_VERSION)/build && \
	cmake .. \
		-DCMAKE_TOOLCHAIN_FILE=$(ANDROID_NDK)/build/cmake/android.toolchain.cmake \
		-DANDROID_ABI=$(if $(filter aarch64,$(ARCH)),arm64-v8a,$(if $(filter armv7a,$(ARCH)),armeabi-v7a,x86_64)) \
		-DANDROID_PLATFORM=android-26 \
		-DCMAKE_BUILD_TYPE=MinSizeRel \
		-DCMAKE_INSTALL_PREFIX=$(CURDIR)/$(MBEDTLS_INSTALL_DIR) \
		-DENABLE_PROGRAMS=OFF \
		-DENABLE_TESTING=OFF \
		-DUSE_STATIC_MBEDTLS_LIBRARY=ON \
		-DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
		-DCMAKE_C_FLAGS="-Os -ffunction-sections -fdata-sections" && \
	$(MAKE) && $(MAKE) install
	rm -rf $(MBEDTLS_DIR)/mbedtls-$(MBEDTLS_VERSION)

$(CURL_ZIP):
	mkdir -p $(CURL_DIR)/src
	curl -L -o $(CURL_ZIP) "https://github.com/curl/curl/releases/download/curl-$(subst .,_,${CURL_VERSION})/curl-$(CURL_VERSION).zip"

ifeq ($(PLATFORM),android)
$(CURL_LIB): $(MBEDTLS) $(CURL_ZIP)
else
$(CURL_LIB): $(CURL_ZIP)
endif
ifeq ($(HOST),windows)
	powershell -Command "Expand-Archive -Path '$(CURL_ZIP)' -DestinationPath '$(CURL_DIR)\src\'"
else
	unzip -o $(CURL_ZIP) -d $(CURL_DIR)/src/.
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

	mkdir -p $(dir $(CURL_LIB))
	mv $(CURL_SRC)/lib/.libs/libcurl.a $(CURL_LIB)
	rm -rf $(CURL_DIR)/src/curl-$(CURL_VERSION)

.NOTPARALLEL: %.dylib
%.dylib:
	rm -rf $(BUILD_DIRS) && $(MAKE) PLATFORM=$*
	mv $(DIST_DIR)/cloudsync.dylib $(DIST_DIR)/$@

define PLIST
<?xml version=\"1.0\" encoding=\"UTF-8\"?>\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\
<plist version=\"1.0\">\
<dict>\
<key>CFBundleDevelopmentRegion</key>\
<string>en</string>\
<key>CFBundleExecutable</key>\
<string>CloudSync</string>\
<key>CFBundleIdentifier</key>\
<string>ai.sqlite.cloudsync</string>\
<key>CFBundleInfoDictionaryVersion</key>\
<string>6.0</string>\
<key>CFBundlePackageType</key>\
<string>FMWK</string>\
<key>CFBundleSignature</key>\
<string>????</string>\
<key>CFBundleVersion</key>\
<string>$(shell make version)</string>\
<key>CFBundleShortVersionString</key>\
<string>$(shell make version)</string>\
<key>MinimumOSVersion</key>\
<string>11.0</string>\
</dict>\
</plist>
endef

define MODULEMAP
framework module CloudSync {\
  umbrella header \"CloudSync.h\"\
  export *\
}
endef

LIB_NAMES = ios.dylib ios-sim.dylib macos.dylib
FMWK_NAMES = ios-arm64 ios-arm64_x86_64-simulator macos-arm64_x86_64
$(DIST_DIR)/%.xcframework: $(LIB_NAMES)
	@$(foreach i,1 2 3,\
		lib=$(word $(i),$(LIB_NAMES)); \
		fmwk=$(word $(i),$(FMWK_NAMES)); \
		mkdir -p $(DIST_DIR)/$$fmwk/CloudSync.framework/Headers; \
		mkdir -p $(DIST_DIR)/$$fmwk/CloudSync.framework/Modules; \
		cp src/cloudsync.h $(DIST_DIR)/$$fmwk/CloudSync.framework/Headers/CloudSync.h; \
		printf "$(PLIST)" > $(DIST_DIR)/$$fmwk/CloudSync.framework/Info.plist; \
		printf "$(MODULEMAP)" > $(DIST_DIR)/$$fmwk/CloudSync.framework/Modules/module.modulemap; \
		mv $(DIST_DIR)/$$lib $(DIST_DIR)/$$fmwk/CloudSync.framework/CloudSync; \
		install_name_tool -id "@rpath/CloudSync.framework/CloudSync" $(DIST_DIR)/$$fmwk/CloudSync.framework/CloudSync; \
	)
	xcodebuild -create-xcframework $(foreach fmwk,$(FMWK_NAMES),-framework $(DIST_DIR)/$(fmwk)/CloudSync.framework) -output $@
	rm -rf $(foreach fmwk,$(FMWK_NAMES),$(DIST_DIR)/$(fmwk))

xcframework: $(DIST_DIR)/CloudSync.xcframework

AAR_ARM64 = packages/android/src/main/jniLibs/arm64-v8a/
AAR_ARM = packages/android/src/main/jniLibs/armeabi-v7a/
AAR_X86 = packages/android/src/main/jniLibs/x86_64/
aar:
	mkdir -p $(AAR_ARM64) $(AAR_ARM) $(AAR_X86)
	$(MAKE) clean && $(MAKE) PLATFORM=android ARCH=arm64-v8a
	mv $(DIST_DIR)/cloudsync.so $(AAR_ARM64)
	$(MAKE) clean && $(MAKE) PLATFORM=android ARCH=armeabi-v7a
	mv $(DIST_DIR)/cloudsync.so $(AAR_ARM)
	$(MAKE) clean && $(MAKE) PLATFORM=android ARCH=x86_64
	mv $(DIST_DIR)/cloudsync.so $(AAR_X86)
	cd packages/android && ./gradlew clean assembleRelease
	cp packages/android/build/outputs/aar/android-release.aar $(DIST_DIR)/cloudsync.aar

# Tools
version:
	@echo $(shell sed -n 's/^#define CLOUDSYNC_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' src/cloudsync.h)

# Clean up generated files
clean:
	rm -rf $(BUILD_DIRS) $(DIST_DIR)/* $(COV_DIR) *.gcda *.gcno *.gcov *.sqlite

# Help message
help:
	@echo "SQLite Sync Extension Makefile"
	@echo "Usage:"
	@echo "  make [PLATFORM=platform] [ARCH=arch] [ANDROID_NDK=\$$ANDROID_HOME/ndk/26.1.10909125] [NATIVE_NETWORK=ON] [target]"
	@echo ""
	@echo "Platforms:"
	@echo "  linux (default on Linux)"
	@echo "  macos (default on macOS - can be compiled with native network support)"
	@echo "  windows (default on Windows)"
	@echo "  android (needs ARCH to be set to x86_64, arm64-v8a, or armeabi-v7a and ANDROID_NDK to be set)"
	@echo "  ios (only on macOS - can be compiled with native network support)"
	@echo "  ios-sim (only on macOS - can be compiled with native network support)"
	@echo ""
	@echo "Targets:"
	@echo "  all	   				- Build the extension (default)"
	@echo "  clean	 				- Remove built files"
	@echo "  test [COVERAGE=true]	- Test the extension with optional coverage output"
	@echo "  help	  				- Display this help message"
	@echo "  xcframework			- Build the Apple XCFramework"
	@echo "  aar					- Build the Android AAR package"

.PHONY: all clean test extension help version xcframework aar
