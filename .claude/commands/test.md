Run the SQLite and PostgreSQL tests for this project.

## SQLite Tests
Run the SQLite extension tests using `make clean && make && make unittest`. This builds the extension and runs all tests including unit tests.

## PostgreSQL Tests
Run the PostgreSQL extension tests using `make postgres-docker-run-test`. This runs `test/postgresql/full_test.sql` against the Docker container.

**Note:** PostgreSQL tests require the Docker container to be running. Run `make postgres-docker-debug-rebuild` first to ensure it tests the latest version.

Run both test suites and report any failures.
