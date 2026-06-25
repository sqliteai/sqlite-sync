# Block-Level LWW

Standard CRDT sync resolves conflicts at the **cell level**: if two devices edit the same column of the same row, one value wins entirely. This works for short values like names or statuses, but for longer text content (markdown documents, notes, agent memory files) the entire text is replaced even if edits were in different parts.

**Block-Level LWW** (Last-Writer-Wins) solves this by splitting text columns into **blocks** (lines by default) and tracking each block independently. When two devices edit different lines of the same text, **both edits are preserved** after sync. Only when two devices edit the *same* line does LWW apply.

This feature was specifically designed to keep **markdown files** in sync across devices and AI agents. Agents that independently edit different sections of a shared document (adding notes, updating status, appending logs) can do so without overwriting each other's work.

## How It Works

1. **Enable block tracking** on a text column using `cloudsync_set_column()`.
2. On INSERT or UPDATE, text is automatically split into blocks using a delimiter (default: newline `\n`).
3. Each block gets a unique fractional index position, enabling insertions between blocks without reindexing.
4. During sync, changes are merged block-by-block rather than replacing the whole cell.
5. The base column always contains the current full text, your queries work unchanged.

## Setup

```sql
-- Create a table with a text column for long-form content
CREATE TABLE notes (
    id TEXT PRIMARY KEY NOT NULL,
    title TEXT NOT NULL DEFAULT '',
    body TEXT NOT NULL DEFAULT ''
);

-- Initialize sync on the table
SELECT cloudsync_init('notes');

-- Enable block-level LWW on the "body" column
SELECT cloudsync_set_column('notes', 'body', 'algo', 'block');
```

## Example: Two-Device Merge

```sql
-- Device A: create a note
INSERT INTO notes (id, title, body) VALUES (
    'note-001',
    'Meeting Notes',
    'Line 1: Welcome
Line 2: Agenda
Line 3: Action items'
);

-- Sync Device A -> Cloud -> Device B
-- Both devices now have the same 3-line note.
```

```sql
-- Device A (offline): edit line 1
UPDATE notes SET body = 'Line 1: Welcome everyone
Line 2: Agenda
Line 3: Action items' WHERE id = 'note-001';

-- Device B (offline): edit line 3
UPDATE notes SET body = 'Line 1: Welcome
Line 2: Agenda
Line 3: Action items - DONE' WHERE id = 'note-001';
```

```sql
-- After both devices sync, the merged result:
-- 'Line 1: Welcome everyone
--  Line 2: Agenda
--  Line 3: Action items - DONE'
--
-- Both edits are preserved because they affected different lines.
```

## Custom Delimiter

For paragraph-level tracking (useful for long-form markdown documents), set a custom delimiter:

```sql
-- Use double newline as delimiter (paragraph separator)
SELECT cloudsync_set_column('notes', 'body', 'delimiter', '

');
```

## Materializing Text

After a merge, the `body` column is updated automatically. You can also manually trigger materialization:

```sql
-- Reconstruct body from blocks for a specific row
SELECT cloudsync_text_materialize('notes', 'body', 'note-001');

-- With a composite primary key (e.g., PRIMARY KEY (tenant_id, doc_id))
SELECT cloudsync_text_materialize('docs', 'body', 'tenant-1', 'doc-001');

-- Then read normally
SELECT body FROM notes WHERE id = 'note-001';
```

## Mixed Columns

Block-level LWW can be enabled on specific columns while other columns use standard cell-level LWW:

```sql
CREATE TABLE docs (
    id TEXT PRIMARY KEY NOT NULL,
    title TEXT NOT NULL DEFAULT '',    -- standard LWW (cell-level)
    body TEXT NOT NULL DEFAULT '',     -- block LWW (line-level)
    status TEXT NOT NULL DEFAULT ''    -- standard LWW (cell-level)
);

SELECT cloudsync_init('docs');
SELECT cloudsync_set_column('docs', 'body', 'algo', 'block');

-- Concurrent edits to "title" or "status" use normal LWW.
-- Concurrent edits to "body" merge at the line level.
```

## Key Properties

- **Non-conflicting edits are preserved**: Two users editing different lines both see their changes after sync.
- **Same-line conflicts use LWW**: If two users edit the same line, the last writer wins.
- **Custom delimiters**: Use paragraph separators (`\n\n`), sentence boundaries, or any string.
- **Mixed columns**: A table can have both regular and block-level LWW columns.
- **Transparent reads**: The base column always contains the current full text.

For API details, see the [API Reference](../API.md).
