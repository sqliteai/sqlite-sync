//
//  block.h
//  cloudsync
//
//  Block-level LWW CRDT support for text/blob fields.
//  Instead of replacing an entire text column on conflict,
//  the text is split into blocks (lines/paragraphs) that are
//  independently version-tracked and merged.
//

#ifndef __CLOUDSYNC_BLOCK__
#define __CLOUDSYNC_BLOCK__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// The separator character used in col_name to distinguish block entries
// from regular column entries. Format: "col_name\x1Fposition_id"
#define BLOCK_SEPARATOR         '\x1F'
#define BLOCK_SEPARATOR_STR     "\x1F"
#define BLOCK_DEFAULT_DELIMITER "\n"

// Column-level algorithm for block tracking
typedef enum {
    col_algo_normal = 0,
    col_algo_block  = 1
} col_algo_t;

// A single block from splitting text
typedef struct {
    char    *content;       // block text (owned, must be freed)
    char    *position_id;   // fractional index position (owned, must be freed)
} block_entry_t;

// Array of blocks
typedef struct {
    block_entry_t   *entries;
    int             count;
    int             capacity;
} block_list_t;

// Diff result for comparing old and new block lists
typedef enum {
    BLOCK_DIFF_UNCHANGED = 0,
    BLOCK_DIFF_ADDED     = 1,
    BLOCK_DIFF_MODIFIED  = 2,
    BLOCK_DIFF_REMOVED   = 3
} block_diff_type;

typedef struct {
    block_diff_type type;
    char    *position_id;   // the position_id (owned, must be freed)
    char    *content;       // new content (owned, must be freed; NULL for REMOVED)
} block_diff_entry_t;

typedef struct {
    block_diff_entry_t  *entries;
    int                 count;
    int                 capacity;
} block_diff_t;

// Initialize the fractional-indexing library to use cloudsync's allocator.
// Must be called once before any block_position_between / block_initial_positions calls.
void block_init_allocator(void);

// Check if a col_name is a block entry (contains BLOCK_SEPARATOR)
bool block_is_block_colname(const char *col_name);

// Extract the base column name from a block col_name (caller must free)
// e.g., "body\x1F0.5" -> "body"
char *block_extract_base_colname(const char *col_name);

// Extract the position_id from a block col_name
// e.g., "body\x1F0.5" -> "0.5"
const char *block_extract_position_id(const char *col_name);

// Build a block col_name from base + position_id (caller must free)
// e.g., ("body", "0.5") -> "body\x1F0.5"
char *block_build_colname(const char *base_col, const char *position_id);

// Split text into blocks using the given delimiter
block_list_t *block_split(const char *text, const char *delimiter);

// Free a block list
void block_list_free(block_list_t *list);

// Generate fractional index position IDs for N initial blocks
// Returns array of N strings (caller must free each + the array)
char **block_initial_positions(int count);

// Generate a position ID that sorts between 'before' and 'after'
// Either can be NULL (meaning beginning/end of sequence)
// Caller must free the result
char *block_position_between(const char *before, const char *after);

// Compute diff between old blocks (with position IDs) and new content blocks
// old_blocks: existing blocks from metadata (with position_ids)
// new_parts: new text split by delimiter (no position_ids yet)
// new_count: number of new parts
block_diff_t *block_diff(block_entry_t *old_blocks, int old_count,
                         const char **new_parts, int new_count);

// Free a diff result
void block_diff_free(block_diff_t *diff);

// Create an empty block list (for accumulating existing blocks)
block_list_t *block_list_create_empty(void);

// Add a block entry to a list (content and position_id are copied)
bool block_list_add(block_list_t *list, const char *content, const char *position_id);

// Concatenate block values with delimiter
// blocks: array of content strings (in position order)
// count: number of blocks
// delimiter: separator between blocks
// Returns allocated string (caller must free)
char *block_materialize_text(const char **blocks, int count, const char *delimiter);

#endif
