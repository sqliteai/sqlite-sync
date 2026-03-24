# sqlite_sync

SQLite Sync is a multi-platform extension that brings a true local-first experience to your applications with minimal effort. It extends standard SQLite tables with built-in support for offline work and automatic synchronization, allowing multiple devices to operate independently—even without a network connection—and seamlessly stay in sync.

## Installation

```
dart pub add sqlite_sync
```

Requires Dart 3.10+ / Flutter 3.38+.

## Usage

### With `sqlite3`

```dart
import 'package:sqlite3/sqlite3.dart';
import 'package:sqlite_sync/sqlite_sync.dart';

void main() {
  // Load once at startup.
  sqlite3.loadSqliteSyncExtension();

  final db = sqlite3.openInMemory();

  // Check version.
  final result = db.select('SELECT cloudsync_version() AS version');
  print(result.first['version']);

  db.dispose();
}
```

### With `drift`

```dart
import 'package:sqlite3/sqlite3.dart';
import 'package:sqlite_sync/sqlite_sync.dart';
import 'package:drift/native.dart';

Sqlite3 loadExtensions() {
  sqlite3.loadSqliteSyncExtension();
  return sqlite3;
}

// Use when creating the database:
NativeDatabase.createInBackground(
  File(path),
  sqlite3: loadExtensions,
);
```

## Supported platforms

| Platform | Architectures |
|----------|---------------|
| Android  | arm64, arm, x64 |
| iOS      | arm64 (device + simulator) |
| macOS    | arm64, x64 |
| Linux    | arm64, x64 |
| Windows  | x64 |

## API

See the full [sqlite-sync API documentation](https://github.com/sqliteai/sqlite-sync/blob/main/API.md).

## License

See [LICENSE](LICENSE).
