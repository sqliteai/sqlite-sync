# Apply fuzzer

`fuzz_apply.c` mutates real payloads (v3 value fragments and block-column edits), repairs the
header checksum so the mutation reaches the row decoder and the merge, and applies the result
to a database that already holds conflicting local rows.

    make fuzz-apply                          # AddressSanitizer + UBSan, a few thousand iterations
    ./test/fuzz/build.sh thread build/fuzz_tsan/fuzz_apply_tsan
    ./build/fuzz_tsan/fuzz_apply_tsan 600 9 6     # iterations, seed, threads (one connection each)
    ./test/fuzz/msan_docker.sh               # MemorySanitizer, in a Linux container

Any sanitizer report is a bug in CloudSync unless it comes from inside `sqlite3.c`'s file I/O
under MemorySanitizer (see `msan_docker.sh`).
