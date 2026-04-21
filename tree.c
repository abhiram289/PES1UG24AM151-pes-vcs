// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>

extern int object_write(ObjectType type,const void *data,size_t len,ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    if (!data_out || !len_out) return -1;

    size_t exact_size = 0;
    for (int i = 0; i < tree->count; i++) {
        // up to 8 octal chars for mode + space + name + null + binary hash
        exact_size += 8 + 1 + strlen(tree->entries[i].name) + 1 + HASH_SIZE;
    }

    uint8_t *buffer = malloc(exact_size > 0 ? exact_size : 1);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = snprintf((char *)buffer + offset,
                               exact_size - offset,
                               "%o %s",
                               entry->mode,
                               entry->name);
        if (written < 0 || (size_t)written >= exact_size - offset) {
            free(buffer);
            return -1;
        }
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.
typedef struct {
    uint32_t mode;
    ObjectID hash;
    char path[256];
} TempEntry;

static int path_under_prefix(const char *path, const char *prefix) {
    if (!prefix) return 1;
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0;
}

static int load_temp_entries_from_index(TempEntry *entries, int *count_out) {
    FILE *fp = fopen(INDEX_FILE, "r");
    if (!fp) {
        *count_out = 0;
        return 0;
    }

    int count = 0;
    while (count < MAX_INDEX_ENTRIES) {
        char hash_hex[HASH_HEX_SIZE + 1];
        uint64_t mtime;
        uint32_t size;
        int rc = fscanf(fp, "%o %64s %" SCNu64 " %u %255s",
                        &entries[count].mode,
                        hash_hex,
                        &mtime,
                        &size,
                        entries[count].path);
        if (rc == EOF) break;
        if (rc != 5 || hex_to_hash(hash_hex, &entries[count].hash) != 0) {
            fclose(fp);
            return -1;
        }
        count++;
    }

    fclose(fp);
    *count_out = count;
    return 0;
}

static int build_tree_level(TempEntry *entries, int count, const char *prefix, ObjectID *id_out)
{
    Tree tree;
    tree.count = 0;
    for (int i = 0; i < count; i++) {

        const char *rel = entries[i].path;

        if (!path_under_prefix(rel, prefix))
            continue;

        const char *name = prefix ? rel + strlen(prefix) : rel;

        if (strchr(name, '/') != NULL)
            continue;

        if (tree.count >= MAX_TREE_ENTRIES)
            return -1;

        TreeEntry *t = &tree.entries[tree.count++];

        t->mode = entries[i].mode;
        if (snprintf(t->name, sizeof(t->name), "%s", name) >= (int)sizeof(t->name))
            return -1;
        t->hash = entries[i].hash;
    }
    for (int i = 0; i < count; i++) {

        const char *rel = entries[i].path;

        if (!path_under_prefix(rel, prefix))
            continue;

        const char *name = prefix ? rel + strlen(prefix) : rel;

        const char *slash = strchr(name, '/');
        if (!slash)
            continue;

        char dirname[256];
        size_t dirname_len = (size_t)(slash - name);
        if (dirname_len == 0 || dirname_len >= sizeof(dirname))
            return -1;
        strncpy(dirname, name, dirname_len);
        dirname[dirname_len] = '\0';

        int exists = 0;
        for (int j = 0; j < tree.count; j++) {
            if (strcmp(tree.entries[j].name, dirname) == 0)
                exists = 1;
            if (exists) break;
        }

        if (exists)
            continue;

        char new_prefix[256];
    if (snprintf(new_prefix,
                sizeof(new_prefix),
                "%s%s/",
                prefix ? prefix : "",
                dirname) >= (int)sizeof(new_prefix)) {
        return -1;
    }
new_prefix[sizeof(new_prefix) - 1] = '\0';

        ObjectID sub_id;

        if (build_tree_level(entries, count,
                            new_prefix,
                            &sub_id) != 0)
            return -1;

        if (tree.count >= MAX_TREE_ENTRIES)
            return -1;

        TreeEntry *t = &tree.entries[tree.count++];

        t->mode = MODE_DIR;
        if (snprintf(t->name, sizeof(t->name), "%s", dirname) >= (int)sizeof(t->name))
            return -1;
        t->hash = sub_id;
    }
    void *data;
    size_t len;

    if (tree_serialize(&tree, &data, &len) != 0)
        return -1;

    if (object_write(OBJ_TREE, data, len, id_out) != 0) {
        free(data);
        return -1;
    }

    free(data);
    return 0;
}

int tree_from_index(ObjectID *id_out) {
    TempEntry entries[MAX_INDEX_ENTRIES];
    int count = 0;
    if (load_temp_entries_from_index(entries, &count) != 0)
        return -1;

    return build_tree_level(entries, count, NULL, id_out);
}