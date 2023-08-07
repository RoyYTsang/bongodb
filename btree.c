

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <inttypes.h>


#define BLOCK_SIZE 4096
#define MAX_NKEYS  ((BLOCK_SIZE - 16) / 16)
#define INVALID_NKEYS 9999
#define NUM_BUFS 5

typedef uint64_t bt_key_t;
typedef uint64_t bt_value_or_addr_t;
typedef char block_buf[BLOCK_SIZE];

/*
 * Buffers are managed using a singly linked free list.
 *
 * buf_count: number of used buffers. Useful for checking for leaked buffers.
 */
struct btree {
    FILE *nfile;
    bt_value_or_addr_t file_size;
    int buf_count;
    block_buf *next_free_block_buf;
    block_buf bufs[NUM_BUFS];
};

struct header {
    bt_value_or_addr_t root_ptr;
    bt_value_or_addr_t free_ptr;
};

struct node {
    uint16_t nkeys;
    uint16_t is_leaf;
    uint32_t padding;
    bt_key_t keys[MAX_NKEYS];
    bt_value_or_addr_t values[MAX_NKEYS + 1];
};


////////////////////////////////////////////////////////////
//                BTree Basic Operations
////////////////////////////////////////////////////////////

/*
 * Returns a new block address and increases the file size.
 */
bt_value_or_addr_t
bt_new_block_addr(struct btree *bt) {
    bt_value_or_addr_t addr = bt->file_size;
    bt->file_size += BLOCK_SIZE;
    return addr;
}

/*
 * You must free the returned buffer.
 */
void *
bt_new_block_buf(struct btree *bt) {
    if (!bt->next_free_block_buf) {
        fprintf(stderr, "bt_new_block_buf: out of buffers\n");
        exit(1);
    }
    block_buf *buf = bt->next_free_block_buf;
    bt->next_free_block_buf = *(block_buf **)buf;
    bt->buf_count++;
    return buf;
}

/*
 * Every time a function returns a struct node* or a struct header*, that
 * pointer must be freed using bt_free_block_buf. There are a limited number
 * of buffers in the btree, so make sure memory doesn't leak.
 */
void
bt_free_block_buf(struct btree *bt, void *buf) {
    assert(bt->bufs <= (block_buf *) buf && (block_buf *) buf < bt->bufs + NUM_BUFS);
    *(block_buf **)buf = bt->next_free_block_buf;
    bt->next_free_block_buf = buf;
    bt->buf_count--;
}

/*
 * You must free the returned buffer.
 */
struct node *
bt_read_node(struct btree *bt, bt_value_or_addr_t addr) {
    int seek_err = fseek(bt->nfile, addr, SEEK_SET);
    if (seek_err) {
        perror("bt_read_node: fseek");
        exit(1);
    }

    void *buf = bt_new_block_buf(bt);
    int count = fread(buf, BLOCK_SIZE, 1, bt->nfile);
    if (count != 1) {
        perror("bt_read_node: fread");
        exit(1);
    }

    return buf;
}

void
bt_write_node(struct btree *bt, struct node *node, bt_value_or_addr_t addr) {
    int seek_err = fseek(bt->nfile, addr, SEEK_SET);
    if (seek_err) {
        perror("bt_write_node: fseek");
        exit(1);
    }

    int count = fwrite(node, BLOCK_SIZE, 1, bt->nfile);
    if (count != 1) {
        perror("bt_write_node: fwrite");
        exit(1);
    }
}

/*
 * You must free the returned buffer.
 */
struct header *
bt_get_header(struct btree *bt) {
    return (struct header *) bt_read_node(bt, 0);
}

/*
 * You must free the returned buffer.
 */
struct node *
bt_new_node(struct btree *bt, bt_value_or_addr_t *outparam_node_addr) {
    struct header *header = bt_get_header(bt);
    bt_value_or_addr_t free_ptr = header->free_ptr;

    struct node *retval;
    if (!free_ptr) {
        // free list is empty. need to extend file.
        *outparam_node_addr = bt_new_block_addr(bt);
        retval = bt_new_block_buf(bt);
    } else {
        struct node *node = bt_read_node(bt, free_ptr);
        assert(node->nkeys == INVALID_NKEYS);
        bt_value_or_addr_t next_free = node->values[0];
        header->free_ptr = next_free;
        bt_write_node(bt, (struct node *) header, 0);
        retval = node;
    }
    bt_free_block_buf(bt, header);
    return retval;
}

/*
 * You must free the returned buffer.
 */
struct node *
bt_get_root(struct btree *bt, bt_value_or_addr_t *outparam_root_addr) {
    struct header *header = bt_get_header(bt);
    bt_value_or_addr_t root_ptr = header->root_ptr;
    bt_free_block_buf(bt, header);
    *outparam_root_addr = root_ptr;
    return bt_read_node(bt, root_ptr);
}

void
bt_set_root(struct btree *bt, bt_value_or_addr_t addr) {
    struct header *header = bt_get_header(bt);
    header->root_ptr = addr;
    bt_write_node(bt, (struct node *) header, 0);
    bt_free_block_buf(bt, header);
}

void
bt_open(struct btree *bt, const char *filename) {
    FILE *file = fopen(filename, "r+");

    // If we could not open, file may not exist yet. Try creating the file.
    if (!file) {
        file = fopen(filename, "w+");
    }

    if (!file) {
        perror("bt_open: fopen");
        exit(1);
    }
    bt->nfile = file;

    // seek to end
    int seek_err = fseek(file, 0, SEEK_END);
    if (seek_err) {
        perror("bt_open: fseek");
        exit(1);
    }

    // get file position of the end
    long size = ftell(file);
    if (size == -1L) {
        perror("bt_open: ftell");
        exit(1);
    }
    bt->file_size = size;

    // init bufs
    bt->next_free_block_buf = bt->bufs;
    for (int i = 0; i < NUM_BUFS - 1; i++) {
        char *ptr = (char *) &bt->bufs[i];
        *(char **) ptr = (char *) &bt->bufs[i + 1];
    }
    *(char **) &bt->bufs[NUM_BUFS - 1] = NULL;
    bt->buf_count = 0;

    // if file is empty
    if (bt->file_size == 0) {
        bt_value_or_addr_t header_addr = bt_new_block_addr(bt);
        assert(header_addr == 0);

        // init header
        struct header *header = bt_new_block_buf(bt);
        header->root_ptr = BLOCK_SIZE;
        header->free_ptr = 0;
        bt_write_node(bt, (struct node *) header, 0);
        bt_free_block_buf(bt, header);

        // init root
        bt_value_or_addr_t root_addr;
        struct node *root = bt_new_node(bt, &root_addr);
        assert(root_addr == BLOCK_SIZE);
        root->nkeys = 0;
        root->is_leaf = 1;
        bt_write_node(bt, root, root_addr);
        bt_free_block_buf(bt, root);
        assert(bt->buf_count == 0);
    }
}

void
bt_close(struct btree *bt) {
    fflush(bt->nfile);
    int close_err = fclose(bt->nfile);
    if (close_err) {
        perror("bt_close");
        exit(1);
    }
}

////////////////////////////////////////////////////////////
//                       Lookup
////////////////////////////////////////////////////////////

int
lookup_intnode(struct node *intnode, bt_key_t key) {
    int left = 0, right = intnode->nkeys - 1;
    while (left <= right) {
        const int mid = (left + right) >> 1;
        const bt_key_t midkey = intnode->keys[mid];
        if (midkey < key) {
            left = mid + 1;
        } else if (midkey > key) {
            right = mid - 1;
        } else {
            return mid + 1;
        }
    }
    return left;
}

int
lookup_leaf(struct node *leaf, bt_key_t key, int *success) {
    int left = 0, right = leaf->nkeys - 1;
    while (left <= right) {
        const int mid = (left + right) >> 1;
        const bt_key_t midkey = leaf->keys[mid];
        if (midkey < key) {
            left = mid + 1;
        } else if (midkey > key) {
            right = mid - 1;
        } else {
            *success = 1;
            return mid;
        }
    }
    *success = 0;
    return left;
}

bt_value_or_addr_t
bt_lookup(struct btree *bt, bt_key_t key, int *success) {
    bt_value_or_addr_t node_addr;
    struct node *node = bt_get_root(bt, &node_addr);
    while (!node->is_leaf) {
        int i = lookup_intnode(node, key);
        bt_value_or_addr_t value = node->values[i];
        bt_free_block_buf(bt, node);
        node = bt_read_node(bt, value);
    }

    int i = lookup_leaf(node, key, success);
    bt_value_or_addr_t value = *success ? node->values[i] : 0;
    bt_free_block_buf(bt, node);
    return value;
}

////////////////////////////////////////////////////////////
//                 Helpers for Insert
////////////////////////////////////////////////////////////

/*
 * Adds an element to an array, splits the array, and moves one half to
 * a new array.
 *
 * @param arr: The array to split.
 * @param new_arr: The new array.
 * @param arr_len: length of arr.
 * @param index: Where to insert x. May be arr_len, which is one greater than
 *               the maximum index within arr.
 * @param midpoint: Where to split. First index that is copied to new array.
 * @param x: The element to insert.
 */
void
array_insert_split(
    uint64_t *arr,
    uint64_t *new_arr,
    int arr_len,
    int index,
    int midpoint,
    uint64_t x
) {
    assert(0 <= index && index <= arr_len);
    if (index < midpoint) {
        // copy to new array
        for (int i = midpoint - 1; i < arr_len; i++) {
            new_arr[i - midpoint + 1] = arr[i];
            arr[i] = 0;
        }

        // insert
        for (int i = midpoint - 1; i > index; i--) {
            arr[i] = arr[i - 1];
        }
        arr[index] = x;
    } else {
        // copy to new array the elements before insertion
        for (int i = midpoint; i < index; i++) {
            new_arr[i - midpoint] = arr[i];
            arr[i] = 0;
        }

        new_arr[index - midpoint] = x;

        // copy after insertion
        for (int i = index; i < arr_len; i++) {
            new_arr[i - midpoint + 1] = arr[i];
            arr[i] = 0;
        }
    }
}

void
array_insert(uint64_t *arr, int arr_len, int index, uint64_t x) {
    for (int i = arr_len; i > index; i--) {
        arr[i] = arr[i - 1];
    }
    arr[index] = x;
}

void
leaf_insert(struct node *leaf, int ind_k, int ind_v, bt_key_t key, bt_value_or_addr_t value) {
    assert(leaf->is_leaf);
    array_insert(leaf->keys, leaf->nkeys, ind_k, key);
    array_insert(leaf->values, leaf->nkeys, ind_v, value);
    leaf->nkeys++;
}

void
intnode_insert(struct node *intnode, int ind_k, int ind_v, bt_key_t key, bt_value_or_addr_t value) {
    assert(!intnode->is_leaf);
    array_insert(intnode->keys, intnode->nkeys, ind_k, key);
    array_insert(intnode->values, intnode->nkeys + 1, ind_v, value);
    intnode->nkeys++;
}


////////////////////////////////////////////////////////////
//                       INSERT
////////////////////////////////////////////////////////////

/*
 * Three Cases:
 * - New Node: new_node, new_node_addr, and new_key are set. is_dup_key is false.
 * - No New: new_node is NULL. is_dup_key is false.
 * - Dup Key: is_dup_key is true.
 * 
 * Any field that is not set has an undefined value.
 * Note that is_dup_key is always set. You should check the fields in this order:
 * 1) is_dup_key
 * 2) new_node (it is set if is_dup_key is false.)
 * 3) everything else (it is set if new_node is non-null.)
 * 
 * Note that new_node must be freed using bt_free_block_buf if it's set and
 * not NULL, i.e. in the case of New Node.
 */
struct insert_recurse_result {
    struct node *new_node;
    bt_value_or_addr_t new_node_addr;
    bt_key_t new_key;
    int is_dup_key;
};

/*
 * This function fully takes care of setting the fields in result.
 */
void
bt_insert_leaf(
    struct btree *bt,
    struct node *leaf,
    bt_value_or_addr_t leaf_addr,
    int i,
    bt_key_t key,
    bt_value_or_addr_t value,
    struct insert_recurse_result *result
) {
    assert(leaf->nkeys != INVALID_NKEYS);
    if (leaf->nkeys < MAX_NKEYS) {
        leaf_insert(leaf, i, i, key, value);
        bt_write_node(bt, leaf, leaf_addr);

        result->new_node = NULL;
        result->is_dup_key = 0;
    } else {
        // ceil((N+1)/2) == (N+2)//2
        const int midpoint = (leaf->nkeys + 2) / 2;

        bt_value_or_addr_t new_leaf_addr;
        struct node *new_leaf = bt_new_node(bt, &new_leaf_addr);
        new_leaf->is_leaf = 1;
        // TODO set new_leaf->next_leaf_ptr

        array_insert_split(leaf->keys, new_leaf->keys, leaf->nkeys, i, midpoint, key);
        array_insert_split(leaf->values, new_leaf->values, leaf->nkeys, i, midpoint, value);

        leaf->nkeys = midpoint;
        // TODO set leaf->next_leaf_ptr
        new_leaf->nkeys = MAX_NKEYS - midpoint + 1; // includes new key
        bt_write_node(bt, leaf, leaf_addr);
        bt_write_node(bt, new_leaf, new_leaf_addr);

        result->new_node = new_leaf;
        result->new_node_addr = new_leaf_addr;
        result->new_key = new_leaf->keys[0];
        result->is_dup_key = 0;
    }
}

/*
 * This function fully takes care of setting the fields in result.
 *
 * @param i: index within node values of the child that was just split.
 * @param new_key: new key to be inserted between new_child and the child
 *                 that was just split.
 * @param new_child_addr: a new node split from the child at index i.
 */
void
bt_insert_intnode(
    struct btree *bt,
    struct node *intnode,
    bt_value_or_addr_t node_addr,
    int i,
    bt_key_t new_key,
    bt_value_or_addr_t new_child_addr,
    struct insert_recurse_result *result
) {
    assert(intnode->nkeys != INVALID_NKEYS);
    if (intnode->nkeys < MAX_NKEYS) {
        intnode_insert(intnode, i, i + 1, new_key, new_child_addr);
        bt_write_node(bt, intnode, node_addr);

        result->new_node = NULL;
        result->is_dup_key = 0;
    } else {
        // Note: ceil(N/2) == (N+1)/2 for any positive integer N.
        // split keys by bottom ceil(nkeys/2) and top nkeys/2
        // middle key is at index ceil(nkeys/2)
        // split values by ceil((nvalues+1)/2)
        const int kmidpoint = (MAX_NKEYS + 1) / 2; // index of middle key
        const int vmidpoint = kmidpoint + 1;

        bt_value_or_addr_t new_node_addr;
        struct node *new_node = bt_new_node(bt, &new_node_addr);
        new_node->is_leaf = 0;

        // split after midpoint, so middle key stays in old node.
        array_insert_split(intnode->keys, new_node->keys, MAX_NKEYS, i, kmidpoint + 1, new_key);
        bt_key_t middle_key = intnode->keys[kmidpoint];
        intnode->keys[kmidpoint] = 0;

        // insert new_child at i+1, since i is the index of the child that was
        // just split, and the new child is to the right.
        array_insert_split(intnode->values, new_node->values, MAX_NKEYS + 1, i + 1, vmidpoint, new_child_addr);

        intnode->nkeys = kmidpoint;
        new_node->nkeys = MAX_NKEYS - kmidpoint;
        bt_write_node(bt, intnode, node_addr);
        bt_write_node(bt, new_node, new_node_addr);

        result->new_node = new_node;
        result->new_node_addr = new_node_addr;
        result->new_key = middle_key;
        result->is_dup_key = 0;
    }
}

void
bt_insert_recurse(
    struct btree *bt,
    struct node *node,
    bt_value_or_addr_t node_addr,
    bt_key_t key,
    bt_value_or_addr_t value,
    int allow_update,
    struct insert_recurse_result *result
) {
    if (node->is_leaf) {
        int success;
        int i = lookup_leaf(node, key, &success);
        if (success && allow_update) {
            // update leaf node.
            node->values[i] = value;
            bt_write_node(bt, node, node_addr);
            result->is_dup_key = 1;
        } else if (success && !allow_update) {
            // duplicate, but no update
            result->is_dup_key = 1;
        } else {
            bt_insert_leaf(bt, node, node_addr, i, key, value, result);
        }
    } else {
        int i = lookup_intnode(node, key);
        bt_value_or_addr_t child_addr = node->values[i];
        struct node *child = bt_read_node(bt, child_addr);

        struct insert_recurse_result sub_result;
        bt_insert_recurse(bt, child, child_addr, key, value, allow_update, &sub_result);
        bt_free_block_buf(bt, child);

        if (sub_result.is_dup_key) {
            result->is_dup_key = 1;
        } else if (!sub_result.new_node) {
            result->new_node = NULL;
            result->is_dup_key = 0;
        } else {
            bt_free_block_buf(bt, sub_result.new_node);
            bt_insert_intnode(bt, node, node_addr, i, sub_result.new_key, sub_result.new_node_addr, result);
        }
    }
}


/*
 * Return zero on success, non-zero on error.
 * Error occurs in one situation: a duplicate key when allow_update is false.
 */
int
bt_insert(struct btree* bt, bt_key_t key, bt_value_or_addr_t value, int allow_update) {
    assert(bt->buf_count == 0);
    bt_value_or_addr_t root_addr;
    struct node *root = bt_get_root(bt, &root_addr);
    assert(bt->buf_count == 1);

    struct insert_recurse_result result;
    bt_insert_recurse(bt, root, root_addr, key, value, allow_update, &result);
    bt_free_block_buf(bt, root);

    if (result.is_dup_key) {
        return allow_update ? 0 : 1;
    } else if (!result.new_node) {
        return 0;
    } else {
        // split root
        bt_value_or_addr_t new_root_addr;
        struct node *new_root = bt_new_node(bt, &new_root_addr);
        new_root->is_leaf = 0;
        new_root->keys[0] = result.new_key;
        new_root->values[0] = root_addr;
        new_root->values[1] = result.new_node_addr;
        new_root->nkeys = 1;
        bt_write_node(bt, new_root, new_root_addr);
        bt_set_root(bt, new_root_addr);
        bt_free_block_buf(bt, new_root);
        bt_free_block_buf(bt, result.new_node);
        return 0;
    }
}



////////////////////////////////////////////////////////////
//                 Testing and Debugging
////////////////////////////////////////////////////////////

void
print_n_times(const char *str, int n) {
    for (int i = 0; i < n; i++)
            printf("%s", str);
}

void
dump_btree_helper(struct btree *bt, struct node *node, bt_value_or_addr_t node_addr, int depth) {
    print_n_times("| ", depth);
    printf("%" PRIu64 ": %s nkeys=%i\n", node_addr, node->is_leaf ? "leaf" : "intnode", node->nkeys);

    if (node->is_leaf) {
        for (int i = 0; i < node->nkeys; i++) {
            print_n_times("| ", depth + 1);
            printf("%" PRIu64 " => %" PRIu64 "\n", node->keys[i], node->values[i]);
        }
    } else {
        bt_value_or_addr_t child_addr = node->values[0];
        struct node *child = bt_read_node(bt, child_addr);
        dump_btree_helper(bt, child, child_addr, depth + 1);
        bt_free_block_buf(bt, child);

        for (int i = 0; i < node->nkeys; i++) {
            print_n_times("| ", depth);
            printf("%" PRIu64 "---\n", node->keys[i]);

            child_addr = node->values[i + 1];
            child = bt_read_node(bt, child_addr);
            dump_btree_helper(bt, child, child_addr, depth + 1);
            bt_free_block_buf(bt, child);
        }
    }
}

void
dump_btree(struct btree *bt) {
    bt_value_or_addr_t root_addr;
    struct node *root = bt_get_root(bt, &root_addr);
    dump_btree_helper(bt, root, root_addr, 0);
    bt_free_block_buf(bt, root);
}

int *
gen_rand(int n) {
    int *nums = malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) {
        nums[i] = i + 1;
    }

    srand(time(NULL));
    for (int i = 0; i < n - 1; i++) {
        int swap_ind = i + rand() % (n - i);
        int temp = nums[i];
        nums[i] = nums[swap_ind];
        nums[swap_ind] = temp;
    }

    return nums;
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char* argv[]) {
    struct btree bt;
    bt_open(&bt, "cbtree.index");
    const int size = 100000;
    int *nums = gen_rand(size);
    for (int i = 0; i < size; i++) {
        int key = nums[i];
        int value = 100 * key;
        int err = bt_insert(&bt, key, value, 0);
        if (err) {
            fprintf(stderr, "Error during insertion of key %i and value %i\n", key, value);
        }
    }
    free(nums);

    int success;
    int value = bt_lookup(&bt, 42, &success);
    printf("lookup %s. value = %i\n", success ? "success" : "failure", value);
    // dump_btree(&bt);
    printf("leaked buffers: %i\n", bt.buf_count);
    bt_close(&bt);
    return 0;
}
