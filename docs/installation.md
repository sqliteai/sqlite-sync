# Installation

Download the appropriate pre-built binary for your platform from the official [Releases](https://github.com/sqliteai/sqlite-sync/releases) page:

- Linux: x86 and ARM
- macOS: x86 and ARM
- Windows: x86
- Android
- iOS

## SQLite CLI / C

```sql
-- In SQLite CLI
.load ./cloudsync

-- In SQL
SELECT load_extension('./cloudsync');
```

## Swift Package

[Add this repository as a package dependency to your Swift project](https://developer.apple.com/documentation/xcode/adding-package-dependencies-to-your-app#Add-a-package-dependency). After adding the package, set up SQLite with extension loading by following steps 4 and 5 of [this guide](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/platforms/ios.md#4-set-up-sqlite-with-extension-loading).

```swift
import CloudSync

var db: OpaquePointer?
sqlite3_open(":memory:", &db)
sqlite3_enable_load_extension(db, 1)
var errMsg: UnsafeMutablePointer<Int8>? = nil
sqlite3_load_extension(db, CloudSync.path, nil, &errMsg)
var stmt: OpaquePointer?
sqlite3_prepare_v2(db, "SELECT cloudsync_version()", -1, &stmt, nil)
defer { sqlite3_finalize(stmt) }
sqlite3_step(stmt)
log("cloudsync_version(): \(String(cString: sqlite3_column_text(stmt, 0)))")
sqlite3_close(db)
```

## Android

Add the [following](https://central.sonatype.com/artifact/ai.sqlite/sync) to your Gradle dependencies:

```gradle
implementation 'ai.sqlite:sync:1.0.0'
```

```java
SQLiteCustomExtension cloudsyncExtension = new SQLiteCustomExtension(
    getApplicationInfo().nativeLibraryDir + "/cloudsync", null);
SQLiteDatabaseConfiguration config = new SQLiteDatabaseConfiguration(
    getCacheDir().getPath() + "/cloudsync_test.db",
    SQLiteDatabase.CREATE_IF_NECESSARY | SQLiteDatabase.OPEN_READWRITE,
    Collections.emptyList(),
    Collections.emptyList(),
    Collections.singletonList(cloudsyncExtension)
);
SQLiteDatabase db = SQLiteDatabase.openDatabase(config, null, null);
```

For full implementation details, see the [complete Android example](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/examples/android/README.md).

## Expo

Install the Expo package:

```bash
npm install @sqliteai/sqlite-sync-expo
```

Add to your `app.json`:

```json
{
  "expo": {
    "plugins": ["@sqliteai/sqlite-sync-expo"]
  }
}
```

Run prebuild:

```bash
npx expo prebuild --clean
```

Load the extension:

```typescript
import { Platform } from 'react-native';
import { getDylibPath, open } from '@op-engineering/op-sqlite';

const db = open({ name: 'mydb.db' });

// Load SQLite Sync extension
if (Platform.OS === 'ios') {
  const path = getDylibPath('ai.sqlite.cloudsync', 'CloudSync');
  db.loadExtension(path);
} else {
  db.loadExtension('cloudsync');
}
```

## React Native

Install the React Native library:

```bash
npm install @sqliteai/sqlite-sync-react-native
```

Then follow the instructions from the [README](https://www.npmjs.com/package/@sqliteai/sqlite-sync-react-native).

## Flutter

Add the [sqlite_sync](https://pub.dev/packages/sqlite_sync) package to your project:

```bash
flutter pub add sqlite_sync  # Flutter projects
dart pub add sqlite_sync     # Dart projects
```

```dart
import 'package:sqlite3/sqlite3.dart';
import 'package:sqlite_sync/sqlite_sync.dart';

sqlite3.loadSqliteSyncExtension();
final db = sqlite3.openInMemory();
print(db.select('SELECT cloudsync_version()'));
```

For a complete example, see the [Flutter example](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/examples/flutter/README.md).

## WASM

The WebAssembly version of SQLite with the SQLite Sync extension is available on npm:

```bash
npm install @sqliteai/sqlite-wasm
```

See the [npm package](https://www.npmjs.com/package/@sqliteai/sqlite-wasm) for usage details.
