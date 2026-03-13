//
//  block.c
//  cloudsync
//
//  Block-level LWW CRDT support for text/blob fields.
//

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "block.h"
#include "utils.h"
#include "fractional_indexing.h"

// MARK: - Col name helpers -

bool block_is_block_colname(const char *col_name) {
    if (!col_name) return false;
    return strchr(col_name, BLOCK_SEPARATOR) != NULL;
}

char *block_extract_base_colname(const char *col_name) {
    if (!col_name) return NULL;
    const char *sep = strchr(col_name, BLOCK_SEPARATOR);
    if (!sep) return cloudsync_string_dup(col_name);
    return cloudsync_string_ndup(col_name, (size_t)(sep - col_name));
}

const char *block_extract_position_id(const char *col_name) {
    if (!col_name) return NULL;
    const char *sep = strchr(col_name, BLOCK_SEPARATOR);
    if (!sep) return NULL;
    return sep + 1;
}

char *block_build_colname(const char *base_col, const char *position_id) {
    if (!base_col || !position_id) return NULL;
    size_t blen = strlen(base_col);
    size_t plen = strlen(position_id);
    char *result = (char *)cloudsync_memory_alloc(blen + 1 + plen + 1);
    if (!result) return NULL;
    memcpy(result, base_col, blen);
    result[blen] = BLOCK_SEPARATOR;
    memcpy(result + blen + 1, position_id, plen);
    result[blen + 1 + plen] = '\0';
    return result;
}

// MARK: - Text splitting -

static block_list_t *block_list_create(void) {
    block_list_t *list = (block_list_t *)cloudsync_memory_zeroalloc(sizeof(block_list_t));
    return list;
}

static bool block_list_append(block_list_t *list, const char *content, size_t content_len, const char *position_id) {
    if (list->count >= list->capacity) {
        int new_cap = list->capacity ? list->capacity * 2 : 16;
        block_entry_t *new_entries = (block_entry_t *)cloudsync_memory_realloc(
            list->entries, (uint64_t)(new_cap * sizeof(block_entry_t)));
        if (!new_entries) return false;
        list->entries = new_entries;
        list->capacity = new_cap;
    }
    block_entry_t *e = &list->entries[list->count];
    e->content = cloudsync_string_ndup(content, content_len);
    e->position_id = position_id ? cloudsync_string_dup(position_id) : NULL;
    if (!e->content) return false;
    list->count++;
    return true;
}

void block_list_free(block_list_t *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        if (list->entries[i].content) cloudsync_memory_free(list->entries[i].content);
        if (list->entries[i].position_id) cloudsync_memory_free(list->entries[i].position_id);
    }
    if (list->entries) cloudsync_memory_free(list->entries);
    cloudsync_memory_free(list);
}

block_list_t *block_list_create_empty(void) {
    return block_list_create();
}

bool block_list_add(block_list_t *list, const char *content, const char *position_id) {
    if (!list) return false;
    return block_list_append(list, content, strlen(content), position_id);
}

block_list_t *block_split(const char *text, const char *delimiter) {
    block_list_t *list = block_list_create();
    if (!list) return NULL;

    if (!text || !*text) {
        // Empty text produces a single empty block
        block_list_append(list, "", 0, NULL);
        return list;
    }

    size_t dlen = strlen(delimiter);
    if (dlen == 0) {
        // No delimiter: entire text is one block
        block_list_append(list, text, strlen(text), NULL);
        return list;
    }

    const char *start = text;
    const char *found;
    while ((found = strstr(start, delimiter)) != NULL) {
        size_t seg_len = (size_t)(found - start);
        if (!block_list_append(list, start, seg_len, NULL)) {
            block_list_free(list);
            return NULL;
        }
        start = found + dlen;
    }
    // Last segment (after last delimiter or entire string if no delimiter found)
    if (!block_list_append(list, start, strlen(start), NULL)) {
        block_list_free(list);
        return NULL;
    }

    return list;
}

// MARK: - Fractional indexing (via fractional-indexing submodule) -

// Wrapper for calloc: fractional_indexing expects (count, size) but cloudsync_memory_zeroalloc takes a single size.
static void *fi_calloc_wrapper(size_t count, size_t size) {
    return cloudsync_memory_zeroalloc((uint64_t)(count * size));
}

void block_init_allocator(void) {
    fractional_indexing_allocator alloc = {
        .malloc = (void *(*)(size_t))cloudsync_memory_alloc,
        .calloc = fi_calloc_wrapper,
        .free   = cloudsync_memory_free
    };
    fractional_indexing_set_allocator(&alloc);
}

char *block_position_between(const char *before, const char *after) {
    return generate_key_between(before, after);
}

char **block_initial_positions(int count) {
    if (count <= 0) return NULL;
    return generate_n_keys_between(NULL, NULL, count);
}

// MARK: - Block diff -

static block_diff_t *block_diff_create(void) {
    block_diff_t *diff = (block_diff_t *)cloudsync_memory_zeroalloc(sizeof(block_diff_t));
    return diff;
}

static bool block_diff_append(block_diff_t *diff, block_diff_type type, const char *position_id, const char *content) {
    if (diff->count >= diff->capacity) {
        int new_cap = diff->capacity ? diff->capacity * 2 : 16;
        block_diff_entry_t *new_entries = (block_diff_entry_t *)cloudsync_memory_realloc(
            diff->entries, (uint64_t)(new_cap * sizeof(block_diff_entry_t)));
        if (!new_entries) return false;
        diff->entries = new_entries;
        diff->capacity = new_cap;
    }
    block_diff_entry_t *e = &diff->entries[diff->count];
    e->type = type;
    e->position_id = cloudsync_string_dup(position_id);
    e->content = content ? cloudsync_string_dup(content) : NULL;
    diff->count++;
    return true;
}

void block_diff_free(block_diff_t *diff) {
    if (!diff) return;
    for (int i = 0; i < diff->count; i++) {
        if (diff->entries[i].position_id) cloudsync_memory_free(diff->entries[i].position_id);
        if (diff->entries[i].content) cloudsync_memory_free(diff->entries[i].content);
    }
    if (diff->entries) cloudsync_memory_free(diff->entries);
    cloudsync_memory_free(diff);
}

// Content-based matching diff algorithm:
// 1. Build a consumed-set from old blocks
// 2. For each new block, find the first unconsumed old block with matching content
// 3. Matched blocks keep their position_id (UNCHANGED)
// 4. Unmatched new blocks get new position_ids (ADDED)
// 5. Unconsumed old blocks are REMOVED
// Modified blocks are detected when content changed but position stayed (handled as MODIFIED)
block_diff_t *block_diff(block_entry_t *old_blocks, int old_count,
                         const char **new_parts, int new_count) {
    block_diff_t *diff = block_diff_create();
    if (!diff) return NULL;

    // Track which old blocks have been consumed
    bool *old_consumed = NULL;
    if (old_count > 0) {
        old_consumed = (bool *)cloudsync_memory_zeroalloc((uint64_t)(old_count * sizeof(bool)));
        if (!old_consumed) {
            block_diff_free(diff);
            return NULL;
        }
    }

    // For each new block, try to find a matching unconsumed old block
    // Use a simple forward scan to preserve ordering
    int old_scan = 0;
    char *last_position = NULL;

    for (int ni = 0; ni < new_count; ni++) {
        bool found = false;

        // Scan forward in old blocks for a content match
        for (int oi = old_scan; oi < old_count; oi++) {
            if (old_consumed[oi]) continue;

            if (strcmp(old_blocks[oi].content, new_parts[ni]) == 0) {
                // Exact match — mark any skipped old blocks as REMOVED
                for (int si = old_scan; si < oi; si++) {
                    if (!old_consumed[si]) {
                        block_diff_append(diff, BLOCK_DIFF_REMOVED, old_blocks[si].position_id, NULL);
                        old_consumed[si] = true;
                    }
                }
                old_consumed[oi] = true;
                old_scan = oi + 1;
                last_position = old_blocks[oi].position_id;
                found = true;
                break;
            }
        }

        if (!found) {
            // New block — needs a new position_id
            const char *next_pos = NULL;
            // Find the next unconsumed old block's position for the upper bound
            for (int oi = old_scan; oi < old_count; oi++) {
                if (!old_consumed[oi]) {
                    next_pos = old_blocks[oi].position_id;
                    break;
                }
            }

            char *new_pos = block_position_between(last_position, next_pos);
            if (new_pos) {
                block_diff_append(diff, BLOCK_DIFF_ADDED, new_pos, new_parts[ni]);
                last_position = diff->entries[diff->count - 1].position_id;
                cloudsync_memory_free(new_pos);
            }
        }
    }

    // Mark remaining unconsumed old blocks as REMOVED
    for (int oi = old_scan; oi < old_count; oi++) {
        if (!old_consumed[oi]) {
            block_diff_append(diff, BLOCK_DIFF_REMOVED, old_blocks[oi].position_id, NULL);
        }
    }

    if (old_consumed) cloudsync_memory_free(old_consumed);
    return diff;
}

// MARK: - Materialization -

char *block_materialize_text(const char **blocks, int count, const char *delimiter) {
    if (count == 0) return cloudsync_string_dup("");
    if (!delimiter) delimiter = BLOCK_DEFAULT_DELIMITER;

    size_t dlen = strlen(delimiter);
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        total += strlen(blocks[i]);
        if (i < count - 1) total += dlen;
    }

    char *result = (char *)cloudsync_memory_alloc(total + 1);
    if (!result) return NULL;

    size_t offset = 0;
    for (int i = 0; i < count; i++) {
        size_t blen = strlen(blocks[i]);
        memcpy(result + offset, blocks[i], blen);
        offset += blen;
        if (i < count - 1) {
            memcpy(result + offset, delimiter, dlen);
            offset += dlen;
        }
    }
    result[offset] = '\0';

    return result;
}
