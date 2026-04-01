### Fix for Divergence Issue in CRDT



This document outlines a bug discovered in one of the most popular CRDT (Conflict-free Replicated Data Type) SQLite extensions and explains our solution to fix the issue. The bug caused database divergence during conflict resolution involving changes to primary keys. This document provides a detailed description of the problem, the root cause, and the applied solution.



#### Steps to Reproduce the Issue

1. Bob inserts a new row with a unique primary key and synchronizes the change with Alice.

2. After receiving Bob's changes, Alice modifies the primary key of the new row and synchronizes the changes back to Bob.

3. Upon processing Alice's changes, Bob’s database ends up in a **diverged state**, where:

   - The original row remains with all `NULL` values related to the new primary key.
   - Alice's modifications to the primary key are either partially or completely ignored.

   This divergence violates the CRDT model's intended behavior, which requires all nodes to eventually converge to the same state.



#### Root Cause

The bug stems from how primary key modifications are handled during conflict resolution. The following SQL statement was used in the original implementation to update the primary key during synchronization:

```sqlite
UPDATE OR REPLACE table_cloudsync 
SET pk=? 
WHERE pk=? AND col_name!='__RIP__';

```

This approach had the following problems:

1. **Loss of Conflict Context**: The `UPDATE OR REPLACE` operation directly replaces the row without correctly accounting for the versioning and sequencing metadata (`db_version`, `seq`, `site_id`). This leads to inconsistent state updates across nodes.
2. **Incorrect Resolution Logic**: The use of `AND col_name!='__RIP__'` checks for deleted rows but fails to fully manage updates to the `pk` field, particularly when conflict metadata is involved.
3. **Version Mismanagement**: By not updating `site_id` and related versioning fields, the changes appear as remote instead of local, causing downstream synchronization issues.



#### Solution

To resolve this issue, we implemented a new SQL statement that ensures proper conflict resolution by updating the versioning metadata and reclassifying the received changes as local. The key changes are as follows:

```sqlite
UPDATE OR REPLACE table_cloudsync 
SET pk=?, 
    db_version=?, 
    col_version=1, 
    seq=cloudsync_seq(), -- a custom command to correctly compute the right seq value
    site_id=0 
WHERE (pk=? AND col_name!='__RIP__') 
ORDER BY db_version, seq ASC;

```



#### Explanation of Fix

1. **Primary Key Update**:
   - The `SET pk=?` clause updates the primary key to the new value received during synchronization.
2. **Version Metadata Update**:
   - `db_version=?`: Updates the database version to reflect the change accurately.
   - `col_version=1`: Resets the column version to indicate that this is a new local change.
   - `seq=cloudsync_seq()`: Assigns a new sequence number to the row using the local sequence generator.
3. **Local Change Classification**:
   - `site_id=0`: Sets the `site_id` to `0`, marking the change as local. This ensures that the update is treated as a local operation, preventing unnecessary conflicts.
4. **Conflict Resolution Ordering**:
   - `ORDER BY db_version, seq ASC`: Ensures that updates are applied in the correct sequence, respecting versioning priorities.
5. **Handling Deletions**:
   - The `WHERE` clause `(pk=? AND col_name!='__RIP__')` ensures that updates only apply to rows that have not been marked for deletion.



#### Before the Fix

- Database divergence occurred in scenarios where primary keys were modified during synchronization.
- Changes would not converge across nodes, leading to inconsistent states and potential data loss.

#### After the Fix

- The synchronization process correctly handles primary key modifications.
- Updates are properly reflected across all nodes, maintaining CRDT guarantees of eventual consistency.
- The database remains in a consistent and convergent state.



## Example Workflow

### Before the Fix

**Initial State**:

| `pk` | `col_name` | `col_value` | `db_version` | `seq` | `site_id` |
| ---- | ---------- | ----------- | ------------ | ----- | --------- |
| 1    | name       | Bob         | 1            | 1     | 1         |

**Bob’s Insert**:

| `pk` | `col_name` | `col_value` | `db_version` | `seq` | `site_id` |
| ---- | ---------- | ----------- | ------------ | ----- | --------- |
| 1    | name       | Bob         | 1            | 1     | 1         |
| 2    | name       | Alice       | 2            | 2     | 1         |

**Alice’s Update (Primary Key Change)**:

| `pk` | `col_name` | `col_value` | `db_version` | `seq` | `site_id` |
| ---- | ---------- | ----------- | ------------ | ----- | --------- |
| 3    | name       | Alice       | 3            | 3     | 2         |

**Bob’s State After Sync**:

| `pk` | `col_name` | `col_value` | `db_version` | `seq` | `site_id` |
| ---- | ---------- | ----------- | ------------ | ----- | --------- |
| NULL | NULL       | NULL        | NULL         | NULL  | NULL      |
| 3    | name       | Alice       | 3            | 3     | 2         |

------

### After the Fix

**Bob’s State After Sync**:

| `pk` | `col_name` | `col_value` | `db_version` | `seq` | `site_id` |
| ---- | ---------- | ----------- | ------------ | ----- | --------- |
| 3    | name       | Alice       | 3            | 3     | 0         |

------



## Conclusion

This fix ensures that CRDT guarantees of **eventual consistency** are preserved across synchronized SQLite databases. By reclassifying received changes as local and properly managing versioning metadata, we prevent database divergence and maintain a consistent state across all nodes.

For further questions or contributions, feel free to contact us or submit issues via our GitHub repository.
