# @sqliteai/sqlite-sync-expo Generator

This directory contains the generator script for the `@sqliteai/sqlite-sync-expo` npm package.

## How It Works

The `generate-expo-package.js` script creates a complete npm package from CI build artifacts:

1. Generates `package.json`, `app.plugin.js`, `src/index.js`, `src/index.d.ts`, `README.md`
2. Copies iOS `CloudSync.xcframework` from artifacts
3. Copies Android `cloudsync.so` files for each architecture

## Usage (CI)

This script is called automatically during the release workflow:

```bash
node generate-expo-package.js <version> <artifacts-dir> <output-dir>
```

Example:

```bash
node generate-expo-package.js 1.0.0 ../../artifacts ./expo-package
cd expo-package && npm publish --provenance --access public
```

## Generated Package Structure

```
expo-package/
├── package.json
├── src/
│   ├── index.js
│   └── index.d.ts
├── app.plugin.js
├── ios/
│   └── CloudSync.xcframework/
├── android/
│   └── jniLibs/
│       ├── arm64-v8a/cloudsync.so
│       ├── armeabi-v7a/cloudsync.so
│       └── x86_64/cloudsync.so
├── README.md
└── LICENSE.md
```

## Testing Locally

To test the generator locally, you need to set up mock artifacts that simulate what CI produces.

### Step 1: Get binaries

**Option A: Download from latest release**

```bash
VERSION="1.0.0"  # or latest version

mkdir -p artifacts/cloudsync-apple-xcframework
mkdir -p artifacts/cloudsync-android-arm64-v8a
mkdir -p artifacts/cloudsync-android-armeabi-v7a
mkdir -p artifacts/cloudsync-android-x86_64

# Download xcframework
curl -L "https://github.com/sqliteai/sqlite-sync/releases/download/${VERSION}/cloudsync-apple-xcframework-${VERSION}.zip" -o xcframework.zip
unzip xcframework.zip -d artifacts/cloudsync-apple-xcframework/
rm xcframework.zip

# Download Android binaries
for arch in arm64-v8a armeabi-v7a x86_64; do
  curl -L "https://github.com/sqliteai/sqlite-sync/releases/download/${VERSION}/cloudsync-android-${arch}-${VERSION}.zip" -o android-${arch}.zip
  unzip android-${arch}.zip -d artifacts/cloudsync-android-${arch}/
  rm android-${arch}.zip
done
```

**Option B: Build from source**

```bash
# Build xcframework (macOS only)
make xcframework

# Build Android (requires Android NDK)
export ANDROID_NDK=/path/to/ndk
make extension PLATFORM=android ARCH=arm64-v8a
make extension PLATFORM=android ARCH=armeabi-v7a
make extension PLATFORM=android ARCH=x86_64

# Move to artifacts structure
mkdir -p artifacts/cloudsync-apple-xcframework
mkdir -p artifacts/cloudsync-android-arm64-v8a
mkdir -p artifacts/cloudsync-android-armeabi-v7a
mkdir -p artifacts/cloudsync-android-x86_64

cp -r dist/CloudSync.xcframework artifacts/cloudsync-apple-xcframework/
# Copy .so files similarly...
```

### Step 2: Run the generator

```bash
cd packages/expo
node generate-expo-package.js 1.0.0 ../../artifacts ./expo-package
```

### Step 3: Test in a Expo app

```bash
# In your Expo app
npm install /path/to/sqlite-sync/packages/expo/expo-package

# Or use file: reference in package.json
# "@sqliteai/sqlite-sync-expo": "file:/path/to/sqlite-sync/packages/expo/expo-package"
```

Update `app.json`:

```json
{
  "expo": {
    "plugins": ["@sqliteai/sqlite-sync-expo"]
  }
}
```

Run prebuild and verify:

```bash
npx expo prebuild --clean

# Check iOS
ls ios/<YourApp>/CloudSync.xcframework

# Check Android
ls android/app/src/main/jniLibs/arm64-v8a/cloudsync.so
```
