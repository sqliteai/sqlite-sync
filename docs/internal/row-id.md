# RowID Uniqueness

The **cloudsync_changes** virtual table aggregates information from all augmented tables and requires a unique way to identify `rowid` values across these tables.

Each augmented table includes two Lamport clock values, `db_version` and `seq`. The `db_version` is incremented with each transaction, while the `seq` is incremented with each operation within a transaction. Both values are global and incremented for each table involved in the transaction.

To create a unique identifier that can serve as the `rowid` in the virtual table, we can combine the `db_version` and `seq` values into a single 64-bit integer using bitwise operations. By encoding both values within this range, we can ensure a unique identifier for each entry. The formula used is:
```
unique_id = (db_version << 32) | seq
```
where:
- `db_version` is constrained to 32 bits (with values up to approximately 4.3 billion),
- `seq` also fits within 32 bits.

This setup allows for up to 4 billion transactions in the database, each involving a maximum of 4 billion statements.

However, since controlling the number of atomic statements within a transaction is generally more manageable for developers than limiting the total number of transactions, we propose adjusting the formula to increase the maximum number of allowable transactions. The modified formula is:
```
unique_id = (db_version << 30) | seq
```
With this adjustment:
- The maximum `db_version` value increases to \(2^{34} - 1\), or 17,179,869,183.
- The maximum `seq` value is \(2^{30} - 1\), or 1,073,741,823.

This modification allows for up to 17 billion transactions in the database, each accommodating up to 1 billion operations while maintaining `rowid` uniqueness.

Assuming one transaction per second, this limit would be reached in approximately 544.4 years.