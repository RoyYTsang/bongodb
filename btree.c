

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define PERROR_AND_EXIT()  \
    do {  \
        perror(__func__); \
        exit(1); \
    } while (0)

#define BLOCK_SIZE          4096
#define MAX_NKEYS           ((BLOCK_SIZE - 16) / 16)
#define MIN_NKEYS_LEAF      (MAX_NKEYS + 1) / 2
#define MIN_NKEYS_INTNODE   MAX_NKEYS / 2

#define INVALID_NKEYS 9999
#define NUM_BUFS 10

typedef uint64_t bt_key_t;
typedef uint64_t bt_value_or_addr_t;
typedef char block_buf[BLOCK_SIZE];

/*
 * Buffers are managed using a singly linked free list.
 * In each free block, the pointer to the next is in block->values[0].
 *
 * buf_count: number of used buffers. Useful for checking for leaked buffers.
 */
struct btree {
    bt_value_or_addr_t file_size;
    int fd;
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

#define NEXT_LEAF_PTR(leaf)  (leaf->values[MAX_NKEYS])

////////////////////////////////////////////////////////////
//                BTree Basic Operations
////////////////////////////////////////////////////////////

int highest_buf_count = 0;

/*
 * Returns a new block address and increases the file size.
 */
bt_value_or_addr_t bt_new_block_addr(struct btree *bt) {
    bt_value_or_addr_t addr = bt->file_size;
    bt->file_size += BLOCK_SIZE;
    return addr;
}

/*
 * You must free the returned buffer.
 */
void *bt_new_block_buf(struct btree *bt) {
    if (!bt->next_free_block_buf) {
        fprintf(stderr, "%s: out of buffers\n", __func__);
        exit(1);
    }
    block_buf *buf = bt->next_free_block_buf;
    bt->next_free_block_buf = *(block_buf **)buf;
    bt->buf_count++;

    if (bt->buf_count > highest_buf_count) {
        highest_buf_count = bt->buf_count;
    }
    return buf;
}

/*
 * Every time a function returns a struct node* or a struct header*, that
 * pointer must be freed using bt_free_block_buf. There are a limited number
 * of buffers in the btree, so make sure memory doesn't leak.
 */
void bt_free_block_buf(struct btree *bt, void *buf) {
    assert(bt->bufs <= (block_buf *) buf && (block_buf *) buf < bt->bufs + NUM_BUFS);
    *(block_buf **)buf = bt->next_free_block_buf;
    bt->next_free_block_buf = buf;
    bt->buf_count--;
}

/*
 * You must free the returned buffer.
 */
struct node *bt_read_node(struct btree *bt, bt_value_or_addr_t addr) {
    assert(addr < bt->file_size && addr % BLOCK_SIZE == 0);

    off_t seek_err = lseek(bt->fd, addr, SEEK_SET);
    if (seek_err == -1) {
        PERROR_AND_EXIT();
    }

    void *buf = bt_new_block_buf(bt);
    ssize_t count = read(bt->fd, buf, BLOCK_SIZE);
    if (count == -1) {
        PERROR_AND_EXIT();
    } else if (count != BLOCK_SIZE) {
        fprintf(stderr, "%s: read %zi (not equal to a block) at address %" PRIu64 "\n",
                __func__, count, addr);
        exit(1);
    }

    return buf;
}

void bt_write_node(struct btree *bt, struct node *node, bt_value_or_addr_t addr) {
    int seek_err = lseek(bt->fd, addr, SEEK_SET);
    if (seek_err == -1) {
        PERROR_AND_EXIT();
    }

    ssize_t count = write(bt->fd, node, BLOCK_SIZE);
    if (count == -1) {
        PERROR_AND_EXIT();
    } else if (count != BLOCK_SIZE) {
        fprintf(stderr, "%s: wrote %zi (not equal to a block) at address %" PRIu64 "\n",
                __func__, count, addr);
        exit(1);
    }
}

/*
 * You must free the returned buffer.
 */
struct header *bt_get_header(struct btree *bt) {
    return (struct header *) bt_read_node(bt, 0);
}

/*
 * You must free the returned buffer.
 */
struct node *bt_new_node(struct btree *bt, bt_value_or_addr_t *outparam_node_addr) {
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

void bt_free_node(struct btree *bt, bt_value_or_addr_t node_addr) {
    struct header *header = bt_get_header(bt);
    bt_value_or_addr_t free_ptr = header->free_ptr;

    struct node *free_block = bt_new_block_buf(bt);
    free_block->values[0] = free_ptr;
    header->free_ptr = node_addr;

    bt_write_node(bt, (struct node *) header, 0);
    bt_write_node(bt, free_block, node_addr);
    bt_free_block_buf(bt, header);
    bt_free_block_buf(bt, free_block);
}

/*
 * You must free the returned buffer.
 */
struct node *bt_get_root(struct btree *bt, bt_value_or_addr_t *outparam_root_addr) {
    struct header *header = bt_get_header(bt);
    bt_value_or_addr_t root_ptr = header->root_ptr;
    bt_free_block_buf(bt, header);
    *outparam_root_addr = root_ptr;
    return bt_read_node(bt, root_ptr);
}

void bt_set_root(struct btree *bt, bt_value_or_addr_t addr) {
    struct header *header = bt_get_header(bt);
    header->root_ptr = addr;
    bt_write_node(bt, (struct node *) header, 0);
    bt_free_block_buf(bt, header);
}

void bt_open(struct btree *bt, const char *filename) {
    int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        perror(__func__);
        exit(1);
    }
    bt->fd = fd;

    struct stat stbuf;
    fstat(fd, &stbuf);
    if (errno == -1) {
        PERROR_AND_EXIT();
    }
    bt->file_size = stbuf.st_size;
    if (bt->file_size % BLOCK_SIZE != 0) {
        fprintf(stderr, "%s: size of file \"%s\" not multiple of block size\n", __func__, filename);
    }

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
        NEXT_LEAF_PTR(root) = 0;
        bt_write_node(bt, root, root_addr);
        bt_free_block_buf(bt, root);
        assert(bt->buf_count == 0);
    }
}

void bt_close(struct btree *bt) {
    int close_err = close(bt->fd);
    if (close_err == -1) {
        PERROR_AND_EXIT();
    }
}

////////////////////////////////////////////////////////////
//                       Lookup
////////////////////////////////////////////////////////////

int lookup_intnode(struct node *intnode, bt_key_t key) {
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

/*
 * Look for the key, or the correct index at which to insert the key.
 * If k is greater than all keys, index will be nkeys, so values[index]
 * will not be a valid value.
 */
int lookup_leaf(struct node *leaf, bt_key_t key, int *outparam_success) {
    int left = 0, right = leaf->nkeys - 1;
    while (left <= right) {
        const int mid = (left + right) >> 1;
        const bt_key_t midkey = leaf->keys[mid];
        if (midkey < key) {
            left = mid + 1;
        } else if (midkey > key) {
            right = mid - 1;
        } else {
            *outparam_success = 1;
            return mid;
        }
    }
    *outparam_success = 0;
    return left;
}

bt_value_or_addr_t bt_lookup(struct btree *bt, bt_key_t key, int *outparam_success) {
    bt_value_or_addr_t node_addr;
    struct node *node = bt_get_root(bt, &node_addr);
    while (!node->is_leaf) {
        int i = lookup_intnode(node, key);
        bt_value_or_addr_t value = node->values[i];
        bt_free_block_buf(bt, node);
        node = bt_read_node(bt, value);
    }

    int i = lookup_leaf(node, key, outparam_success);
    bt_value_or_addr_t value = *outparam_success ? node->values[i] : 0;
    bt_free_block_buf(bt, node);
    return value;
}

////////////////////////////////////////////////////////////
//                       Ranges
////////////////////////////////////////////////////////////

struct key_value_pair {
    bt_key_t key;
    bt_value_or_addr_t value;
};

size_t bt_range(
    struct btree *bt,
    bt_key_t lo,
    bt_key_t hi,
    size_t limit,
    struct key_value_pair *out_array
) {
    assert(bt->buf_count == 0);

    bt_value_or_addr_t node_addr;
    struct node *node = bt_get_root(bt, &node_addr);

    // lookup leaf containing lo
    while (!node->is_leaf) {
        int i = lookup_intnode(node, lo);
        bt_value_or_addr_t next_addr = node->values[i];
        bt_free_block_buf(bt, node);
        node = bt_read_node(bt, next_addr);
    }

    int success;
    int i = lookup_leaf(node, lo, &success);
    size_t elems_copied = 0;

    for(;;) {
        for (; i < node->nkeys; i++) {
            if (elems_copied == limit || node->keys[i] > hi) {
                goto end;
            }
            out_array[elems_copied].key = node->keys[i];
            out_array[elems_copied].value = node->values[i];
            elems_copied++;
        }

        bt_value_or_addr_t next_addr = NEXT_LEAF_PTR(node);
        if (!next_addr) {
            break;
        }

        bt_free_block_buf(bt, node);
        node = bt_read_node(bt, next_addr);
    }

    end:
    bt_free_block_buf(bt, node);
    return elems_copied;
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
void array_insert_split(
    uint64_t *arr,
    uint64_t *new_arr,
    int arr_len,
    int index,
    int midpoint,
    uint64_t x
) {
    assert(0 <= index && index <= arr_len);
    if (index < midpoint) {
        // copy from old array to new array
        for (int i = midpoint - 1; i < arr_len; i++) {
            new_arr[i - midpoint + 1] = arr[i];
        }

        // move elements right in old array to make room for insertion
        for (int i = midpoint - 1; i > index; i--) {
            arr[i] = arr[i - 1];
        }

        // insert
        arr[index] = x;
    } else {
        // copy to new array the elements before insertion
        for (int i = midpoint; i < index; i++) {
            new_arr[i - midpoint] = arr[i];
        }

        new_arr[index - midpoint] = x;

        // copy after insertion
        for (int i = index; i < arr_len; i++) {
            new_arr[i - midpoint + 1] = arr[i];
        }
    }
}

void array_insert(uint64_t *arr, int arr_len, int index, uint64_t x) {
    for (int i = arr_len; i > index; i--) {
        arr[i] = arr[i - 1];
    }
    arr[index] = x;
}

////////////////////////////////////////////////////////////
//                       INSERT
////////////////////////////////////////////////////////////

enum insert_recurse_cases {
    INSERT_RECURSE_CASE_DUP_KEY,
    INSERT_RECURSE_CASE_NO_NEW,
    INSERT_RECURSE_CASE_NEW_NODE,
};

/*
 * Three Cases:
 * - New Node
 * - No New
 * - Dup Key
 *
 * new_node_addr and new_key have defined values in case New Node.
 * In other cases, they may have undefined values.
 *
 * cond always has a defined value, so it's always safe to check.
 */
struct insert_recurse_result {
    bt_value_or_addr_t new_node_addr;
    bt_key_t new_key;
    enum insert_recurse_cases cond;
};

/*
 * This function fully takes care of setting the fields in result.
 */
void bt_insert_leaf(
    struct btree *bt,
    struct node *leaf,
    bt_value_or_addr_t leaf_addr,
    int i,
    bt_key_t key,
    bt_value_or_addr_t value,
    struct insert_recurse_result *outparam
) {
    assert(leaf->nkeys != INVALID_NKEYS);
    if (leaf->nkeys < MAX_NKEYS) {
        // Node has room. Insert key and value.
        array_insert(leaf->keys, leaf->nkeys, i, key);
        array_insert(leaf->values, leaf->nkeys, i, value);
        leaf->nkeys++;
        bt_write_node(bt, leaf, leaf_addr);

        outparam->cond = INSERT_RECURSE_CASE_NO_NEW;
    } else {
        // ceil((N+1)/2) == (N+2)//2
        const int midpoint = (leaf->nkeys + 2) / 2;

        bt_value_or_addr_t new_leaf_addr;
        struct node *new_leaf = bt_new_node(bt, &new_leaf_addr);
        new_leaf->is_leaf = 1;
        NEXT_LEAF_PTR(new_leaf) = NEXT_LEAF_PTR(leaf);

        array_insert_split(leaf->keys, new_leaf->keys, leaf->nkeys, i, midpoint, key);
        array_insert_split(leaf->values, new_leaf->values, leaf->nkeys, i, midpoint, value);

        leaf->nkeys = midpoint;
        NEXT_LEAF_PTR(leaf) = new_leaf_addr;
        new_leaf->nkeys = MAX_NKEYS - midpoint + 1; // includes new key
        bt_write_node(bt, leaf, leaf_addr);
        bt_write_node(bt, new_leaf, new_leaf_addr);

        outparam->cond = INSERT_RECURSE_CASE_NEW_NODE;
        outparam->new_node_addr = new_leaf_addr;
        outparam->new_key = new_leaf->keys[0];

        bt_free_block_buf(bt, new_leaf);
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
void bt_insert_intnode(
    struct btree *bt,
    struct node *intnode,
    bt_value_or_addr_t node_addr,
    int i,
    bt_key_t new_key,
    bt_value_or_addr_t new_child_addr,
    struct insert_recurse_result *outparam
) {
    assert(intnode->nkeys != INVALID_NKEYS);
    if (intnode->nkeys < MAX_NKEYS) {
        // Node has room. Insert key and child address.
        array_insert(intnode->keys, intnode->nkeys, i, new_key);
        array_insert(intnode->values, intnode->nkeys + 1, i + 1, new_child_addr);
        intnode->nkeys++;
        bt_write_node(bt, intnode, node_addr);

        outparam->cond = INSERT_RECURSE_CASE_NO_NEW;
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
        array_insert_split(
            intnode->keys,
            new_node->keys,
            MAX_NKEYS,
            i,
            kmidpoint + 1,
            new_key
        );
        bt_key_t middle_key = intnode->keys[kmidpoint];

        // insert new_child at i+1, since i is the index of the child that was
        // just split, and the new child is to the right.
        array_insert_split(
            intnode->values,
            new_node->values,
            MAX_NKEYS + 1,
            i + 1,
            vmidpoint,
            new_child_addr
        );

        intnode->nkeys = kmidpoint;
        new_node->nkeys = MAX_NKEYS - kmidpoint;
        bt_write_node(bt, intnode, node_addr);
        bt_write_node(bt, new_node, new_node_addr);

        outparam->cond = INSERT_RECURSE_CASE_NEW_NODE;
        outparam->new_node_addr = new_node_addr;
        outparam->new_key = middle_key;

        bt_free_block_buf(bt, new_node);
    }
}

void bt_insert_recurse(
    struct btree *bt,
    struct node *node,
    bt_value_or_addr_t node_addr,
    bt_key_t key,
    bt_value_or_addr_t value,
    int allow_update,
    struct insert_recurse_result *outparam
) {
    if (node->is_leaf) {
        int success;
        int i = lookup_leaf(node, key, &success);
        if (success && allow_update) {
            // update leaf node.
            node->values[i] = value;
            bt_write_node(bt, node, node_addr);
            outparam->cond = INSERT_RECURSE_CASE_DUP_KEY;
        } else if (success && !allow_update) {
            // duplicate, but no update
            outparam->cond = INSERT_RECURSE_CASE_DUP_KEY;
        } else {
            bt_insert_leaf(bt, node, node_addr, i, key, value, outparam);
        }
    } else {
        int i = lookup_intnode(node, key);
        bt_value_or_addr_t child_addr = node->values[i];
        struct node *child = bt_read_node(bt, child_addr);

        struct insert_recurse_result sub_result;
        bt_insert_recurse(bt, child, child_addr, key, value, allow_update, &sub_result);
        bt_free_block_buf(bt, child);

        switch (sub_result.cond) {
            case INSERT_RECURSE_CASE_DUP_KEY:
            case INSERT_RECURSE_CASE_NO_NEW:
                outparam->cond = sub_result.cond;
                break;

            case INSERT_RECURSE_CASE_NEW_NODE:
                bt_insert_intnode(
                    bt,
                    node,
                    node_addr,
                    i,
                    sub_result.new_key,
                    sub_result.new_node_addr,
                    outparam
                );
                break;
        }
    }
}


/*
 * Return zero on success, non-zero on error.
 * Error occurs in one situation: a duplicate key when allow_update is false.
 */
int bt_insert(struct btree* bt, bt_key_t key, bt_value_or_addr_t value, int allow_update) {
    assert(bt->buf_count == 0);
    bt_value_or_addr_t root_addr;
    struct node *root = bt_get_root(bt, &root_addr);
    assert(bt->buf_count == 1);

    struct insert_recurse_result result;
    bt_insert_recurse(bt, root, root_addr, key, value, allow_update, &result);
    bt_free_block_buf(bt, root);

    switch (result.cond) {
        case INSERT_RECURSE_CASE_DUP_KEY:
            return !allow_update;

        case INSERT_RECURSE_CASE_NO_NEW:
            return 0;

        case INSERT_RECURSE_CASE_NEW_NODE:
        {
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
            return 0;
        }
    }
}

////////////////////////////////////////////////////////////
//                 Helpers for Delete
////////////////////////////////////////////////////////////

void array_delete(uint64_t *arr, int arr_len, int index) {
    assert(arr_len > 0);
    // move all later elements back, overwriting the deleted element
    for (int i = index + 1; i < arr_len; i++) {
        arr[i-1] = arr[i];
    }
}

/*
 * Moves the elements of the array left, overwriting the first element.
 */
void array_move_left(uint64_t *arr, int arr_len) {
    array_delete(arr, arr_len, 0);
}

/*
 * Moves the elements of the array right, vacating the first index.
 */
void array_move_right(uint64_t *arr, int arr_len) {
    for (int i = arr_len; i > 0; i--) {
        arr[i] = arr[i - 1];
    }
}

void array_merge_append(uint64_t *src, int src_len, uint64_t *dest, int dest_len) {
    for (int i = 0; i < src_len; i++)
        dest[dest_len + i] = src[i];
}

/*
 * sibling is never null.
 * You must free the returned buffer.
 */
struct delete_get_sibling {
    struct node *sibling;
    bt_value_or_addr_t sibling_addr;
    int is_left_sib;
};

/*
 * Returns a sibling suitable for taking from during the delete algorithm.
 *
 * Returns a sibling with nkeys > min_nkeys if one exists, or if not,
 * returns either sibling. Picks one sibling to read first, then
 * reads the other if needed.
 *
 * @param min_nkeys: If the first sib read has greater than min_nkeys,
 *                   don't bother to read the other sib.
 * @param i: index within parent. The sibs are in the neighboring indexes.
 */
void bt_delete_get_sibling(
    struct btree *bt,
    int min_nkeys,
    struct node *parent_node,
    int i,
    struct delete_get_sibling *outparam
) {
    // Alternate reading left and right first
    static int read_left_first = 1;
    int first_ind, second_ind, is_edge_case;

    if (i == 0) {
        first_ind = i + 1;
        is_edge_case = 1;
    } else if (i == parent_node->nkeys) {
        first_ind = i - 1;
        is_edge_case = 1;
    } else {
        is_edge_case = 0;
    }

    if (!is_edge_case) {
        if (read_left_first) {
            first_ind = i - 1;
            second_ind = i + 1;
        } else {
            first_ind = i + 1;
            second_ind = i - 1;
        }
        read_left_first = !read_left_first;
    }

    // note: second_ind is undefined if is_edge_case is true.

    struct node *first_sib;
    bt_value_or_addr_t first_sib_addr = parent_node->values[first_ind];
    first_sib = bt_read_node(bt, first_sib_addr);

    // in edge cases, there is only one sibling, so return that.
    if (is_edge_case || first_sib->nkeys > min_nkeys) {
        outparam->sibling = first_sib;
        outparam->sibling_addr = first_sib_addr;
        outparam->is_left_sib = first_ind - i == -1;
        return;
    }

    // is_edge_case is never true after this point,
    // so second_ind is always defined.

    // first sib failed, so free its buffer
    bt_free_block_buf(bt, first_sib);

    struct node *second_sib;
    bt_value_or_addr_t second_sib_addr = parent_node->values[second_ind];
    second_sib = bt_read_node(bt, second_sib_addr);

    // We always return a sib, and first_sib has too small nkeys, so
    // return second_sib. Either second_sib is big enough, or neither
    // sibling has a big enough nkeys.
    outparam->sibling = second_sib;
    outparam->sibling_addr = second_sib_addr;
    outparam->is_left_sib = second_ind - i == -1;
}

////////////////////////////////////////////////////////////
//                       DELETE
////////////////////////////////////////////////////////////

enum delete_recurse_cases {
    DELETE_RECURSE_CASE_MODIFY_KEY,
    DELETE_RECURSE_CASE_DELETE_KEY,
    DELETE_RECURSE_CASE_NO_CHANGE,
    DELETE_RECURSE_CASE_KEY_NOT_FOUND,
};

/*
 * Four Cases:
 * - Modify Key
 * - Delete Key
 * - No Change
 * - Key Not Found
 *
 * cond is defined in all cases.
 * index is defined for Modify Key and Delete Key.
 * modified_key is only defined for Modify Key.
 */
struct delete_recurse_result {
    int index;
    int modified_key;
    enum delete_recurse_cases cond;
};

/*
 * This function writes node if it won't be deleted by the algorithm.
 * @param parent: parent of leaf
 * @param p_ind: index within parent of leaf.
 * @param i: index within leaf of element to delete.
 */
void bt_delete_leaf(
    struct btree *bt,
    struct node *leaf,
    bt_value_or_addr_t leaf_addr,
    struct node *parent,
    int parent_index,
    int del_ind,
    struct delete_recurse_result *outparam
) {
    assert(leaf->nkeys != INVALID_NKEYS);
    array_delete(leaf->keys, leaf->nkeys, del_ind);
    array_delete(leaf->values, leaf->nkeys, del_ind);
    leaf->nkeys--;

    if (leaf->nkeys >= MIN_NKEYS_LEAF) {
        bt_write_node(bt, leaf, leaf_addr);
        outparam->cond = DELETE_RECURSE_CASE_NO_CHANGE;
        return;
    }

    struct delete_get_sibling sibling_result;
    bt_delete_get_sibling(bt, MIN_NKEYS_LEAF, parent, parent_index, &sibling_result);
    struct node *sibling = sibling_result.sibling;  // a shortened alias

    if (sibling_result.sibling->nkeys > MIN_NKEYS_LEAF) {
        if (sibling_result.is_left_sib) {
            // move left sib's rightmost key-value to leaf
            // steps:
            // * move leaf's key-values right by one
            // * copy left sib's rightmost.
            // * adjust both nkeys.

            // since the following loops iterate through decreasing
            // memory addresses, and since a node's values are stored after
            // its keys, handle the values first for cache locality.
            // (Does it make a difference?)

            array_move_right(leaf->values, leaf->nkeys);
            leaf->values[0] = sibling->values[sibling->nkeys - 1];
            array_move_right(leaf->keys, leaf->nkeys);
            leaf->keys[0] = sibling->keys[sibling->nkeys - 1];

            leaf->nkeys++;
            sibling->nkeys--;

            outparam->cond = DELETE_RECURSE_CASE_MODIFY_KEY;
            outparam->index = parent_index - 1;
            outparam->modified_key = leaf->keys[0];

            bt_write_node(bt, leaf, leaf_addr);
            bt_write_node(bt, sibling, sibling_result.sibling_addr);
            bt_free_block_buf(bt, sibling);
        } else {
            // move right sib's leftmost key-value to leaf
            // steps:
            // * append right sib's leftmost to leaf
            // * move right sib's key-values left by one.
            // * adjust both nkeys.
            leaf->keys[leaf->nkeys] = sibling->keys[0];
            array_move_left(sibling->keys, sibling->nkeys);
            leaf->values[leaf->nkeys] = sibling->values[0];
            array_move_left(sibling->values, sibling->nkeys);

            leaf->nkeys++;
            sibling->nkeys--;

            outparam->cond = DELETE_RECURSE_CASE_MODIFY_KEY;
            outparam->index = parent_index;
            outparam->modified_key = sibling->keys[0];

            bt_write_node(bt, leaf, leaf_addr);
            bt_write_node(bt, sibling, sibling_result.sibling_addr);
            bt_free_block_buf(bt, sibling);
        }
    } else {
        // If neither sibling is above min capacity, pick one to merge.
        struct node *merge_left, *merge_right;
        bt_value_or_addr_t merge_left_addr, merge_right_addr;
        int ind_key_between;

        if (sibling_result.is_left_sib) {
            merge_right = leaf;
            merge_right_addr = leaf_addr;
            merge_left = sibling;
            merge_left_addr = sibling_result.sibling_addr;
            ind_key_between = parent_index - 1;
        } else {
            merge_right = sibling;
            merge_right_addr = sibling_result.sibling_addr;
            merge_left = leaf;
            merge_left_addr = leaf_addr;
            ind_key_between = parent_index;
        }

        // Always move elements from right to left so the next_leaf_ptr of
        // the prev leaf remains valid.
        array_merge_append(
            merge_right->keys,
            merge_right->nkeys,
            merge_left->keys,
            merge_left->nkeys
        );
        array_merge_append(
            merge_right->values,
            merge_right->nkeys,
            merge_left->values,
            merge_left->nkeys
        );

        merge_left->nkeys += merge_right->nkeys;
        NEXT_LEAF_PTR(merge_left) = NEXT_LEAF_PTR(merge_right);
        bt_write_node(bt, merge_left, merge_left_addr);
        bt_free_node(bt, merge_right_addr);
        bt_free_block_buf(bt, sibling);

        outparam->cond = DELETE_RECURSE_CASE_DELETE_KEY;
        outparam->index = ind_key_between;
    }
}

void bt_delete_intnode(
    struct btree *bt,
    struct node *intnode,
    bt_value_or_addr_t intnode_addr,
    struct node *parent,
    int parent_index,
    int del_ind,
    struct delete_recurse_result *outparam
) {
    assert(intnode->nkeys != INVALID_NKEYS);
    array_delete(intnode->keys, intnode->nkeys, del_ind);
    array_delete(intnode->values, intnode->nkeys + 1, del_ind + 1);
    intnode->nkeys--;

    if (intnode->nkeys >= MIN_NKEYS_INTNODE) {
        bt_write_node(bt, intnode, intnode_addr);
        outparam->cond = DELETE_RECURSE_CASE_NO_CHANGE;
        return;
    }

    struct delete_get_sibling sibling_result;
    bt_delete_get_sibling(bt, MIN_NKEYS_INTNODE, parent, parent_index, &sibling_result);
    struct node *sibling = sibling_result.sibling;  // a shortened alias

    if (sibling->nkeys > MIN_NKEYS_INTNODE) {
        // When moving a value from a sibling, the key in the parent between
        // the node and the sibling is pulled down, and the key from the
        // sibling is pulled up to the parent.
        if (sibling_result.is_left_sib) {
            bt_key_t parent_key = parent->keys[parent_index - 1],
                     new_between_key = sibling->keys[sibling->nkeys - 1];

            // move left sib's rightmost key-value to intnode
            // steps:
            // * move intnode's key-values right by one
            // * copy parent key and left sib's rightmost value.
            // * adjust both nkeys.

            // since the following loops iterate through decreasing
            // memory addresses, and since a node's values are stored after
            // its keys, handle the values first for cache locality.
            // (Does it make a difference?)

            array_move_right(intnode->values, intnode->nkeys + 1);
            intnode->values[0] = sibling->values[sibling->nkeys];
            array_move_right(intnode->keys, intnode->nkeys);
            intnode->keys[0] = parent_key;

            intnode->nkeys++;
            sibling->nkeys--;

            outparam->cond = DELETE_RECURSE_CASE_MODIFY_KEY;
            outparam->index = parent_index - 1;
            outparam->modified_key = new_between_key;

            bt_write_node(bt, intnode, intnode_addr);
            bt_write_node(bt, sibling, sibling_result.sibling_addr);
            bt_free_block_buf(bt, sibling);
        } else {
            bt_key_t parent_key = parent->keys[parent_index],
                     new_key_between = sibling->keys[0];

            // move right sib's leftmost key-value to intnode
            // steps:
            // * append right sib's leftmost to intnode
            // * move right sib's key-values left by one.
            // * adjust both nkeys.
            intnode->keys[intnode->nkeys] = parent_key;
            array_move_left(sibling->keys, sibling->nkeys);
            intnode->values[intnode->nkeys + 1] = sibling->values[0];
            array_move_left(sibling->values, sibling->nkeys + 1);

            intnode->nkeys++;
            sibling->nkeys--;

            outparam->cond = DELETE_RECURSE_CASE_MODIFY_KEY;
            outparam->index = parent_index;
            outparam->modified_key = new_key_between;

            bt_write_node(bt, intnode, intnode_addr);
            bt_write_node(bt, sibling, sibling_result.sibling_addr);
            bt_free_block_buf(bt, sibling);
        }
    } else {
        // If neither sibling is above min capacity, pick one to merge.
        struct node *merge_left, *merge_right;
        bt_value_or_addr_t merge_left_addr, merge_right_addr;
        int ind_key_between;

        if (sibling_result.is_left_sib) {
            merge_right = intnode;
            merge_right_addr = intnode_addr;
            merge_left = sibling;
            merge_left_addr = sibling_result.sibling_addr;
            ind_key_between = parent_index - 1;
        } else {
            merge_right = sibling;
            merge_right_addr = sibling_result.sibling_addr;
            merge_left = intnode;
            merge_left_addr = intnode_addr;
            ind_key_between = parent_index;
        }

        // when merging siblings, the key between them in the parent is pulled
        // down to the newly merged node.
        merge_left->keys[merge_left->nkeys] = parent->keys[ind_key_between];

        // Since the key was pulled down and appended to merge_left->keys,
        // merge_left->nkeys is one less than the real number of keys.
        // However, merge_left->nkeys + 1 is still the real number of values.
        array_merge_append(
            merge_right->keys,
            merge_right->nkeys,
            merge_left->keys,
            merge_left->nkeys + 1
        );
        array_merge_append(
            merge_right->values,
            merge_right->nkeys + 1,
            merge_left->values,
            merge_left->nkeys + 1
        );

        merge_left->nkeys += merge_right->nkeys + 1;

        bt_write_node(bt, merge_left, merge_left_addr);
        bt_free_node(bt, merge_right_addr);
        bt_free_block_buf(bt, sibling);

        outparam->cond = DELETE_RECURSE_CASE_DELETE_KEY;
        outparam->index = ind_key_between;
    }
}

void bt_delete_recurse(
    struct btree *bt,
    struct node *node,
    bt_value_or_addr_t node_addr,
    struct node *parent,
    int parent_index,
    bt_key_t key,
    struct delete_recurse_result *outparam
) {
    if (node->is_leaf) {
        int success;
        int i = lookup_leaf(node, key, &success);
        if (success) {
            bt_delete_leaf(bt, node, node_addr, parent, parent_index, i, outparam);
        } else {
            outparam->cond = DELETE_RECURSE_CASE_KEY_NOT_FOUND;
        }
    } else {
        int i = lookup_intnode(node, key);
        bt_value_or_addr_t child_addr = node->values[i];
        struct node *child = bt_read_node(bt, child_addr);

        struct delete_recurse_result sub_result;
        bt_delete_recurse(bt, child, child_addr, node, i, key, &sub_result);
        bt_free_block_buf(bt, child);

        switch (sub_result.cond) {
            case DELETE_RECURSE_CASE_DELETE_KEY:
                bt_delete_intnode(
                    bt,
                    node,
                    node_addr,
                    parent,
                    parent_index,
                    sub_result.index,
                    outparam
                );
                break;

            case DELETE_RECURSE_CASE_MODIFY_KEY:
                node->keys[sub_result.index] = sub_result.modified_key;
                bt_write_node(bt, node, node_addr);
                outparam->cond = DELETE_RECURSE_CASE_NO_CHANGE;
                break;

            case DELETE_RECURSE_CASE_NO_CHANGE:
                outparam->cond = DELETE_RECURSE_CASE_NO_CHANGE;
                break;

            case DELETE_RECURSE_CASE_KEY_NOT_FOUND:
                outparam->cond = DELETE_RECURSE_CASE_KEY_NOT_FOUND;
                break;
        }
    }
}

int bt_delete(struct btree *bt, bt_key_t key) {
    assert(bt->buf_count == 0);
    bt_value_or_addr_t root_addr;
    struct node *root = bt_get_root(bt, &root_addr);
    assert(root->nkeys != INVALID_NKEYS);

    if (root->is_leaf) {
        if (root->nkeys == 0) {
            // error: delete from empty tree
            bt_free_block_buf(bt, root);
            return 1;
        }

        int success;
        int i = lookup_leaf(root, key, &success);
        if (!success) {
            // error: key not found
            bt_free_block_buf(bt, root);
            return 1;
        }

        // delete from root
        array_delete(root->keys, root->nkeys, i);
        array_delete(root->values, root->nkeys, i);
        root->nkeys--;
        bt_write_node(bt, root, root_addr);
        bt_free_block_buf(bt, root);
        return 0;
    } else {
        int i = lookup_intnode(root, key);
        bt_value_or_addr_t child_addr = root->values[i];
        struct node *child = bt_read_node(bt, child_addr);

        struct delete_recurse_result result;
        bt_delete_recurse(bt, child, child_addr, root, i, key, &result);
        bt_free_block_buf(bt, child);

        switch (result.cond) {
            case DELETE_RECURSE_CASE_DELETE_KEY:
                // delete from root
                array_delete(root->keys, root->nkeys, result.index);
                array_delete(root->values, root->nkeys + 1, result.index + 1);
                root->nkeys--;

                // make child the root if it's the only one left
                if (root->nkeys == 0) {
                    bt_free_node(bt, root_addr);
                    bt_set_root(bt, root->values[0]);
                    bt_free_block_buf(bt, root);
                    return 0;
                }

                bt_write_node(bt, root, root_addr);
                bt_free_block_buf(bt, root);
                return 0;

            case DELETE_RECURSE_CASE_MODIFY_KEY:
                root->keys[result.index] = result.modified_key;
                bt_write_node(bt, root, root_addr);
                bt_free_block_buf(bt, root);
                return 0;

            case DELETE_RECURSE_CASE_NO_CHANGE:
                bt_free_block_buf(bt, root);
                return 0;

            case DELETE_RECURSE_CASE_KEY_NOT_FOUND:
                bt_free_block_buf(bt, root);
                return 1;
        }
    }
}

////////////////////////////////////////////////////////////
//                 Testing and Debugging
////////////////////////////////////////////////////////////

void print_n_times(const char *str, int n) {
    for (int i = 0; i < n; i++)
            printf("%s", str);
}

void
dump_btree_helper(struct btree *bt, struct node *node, bt_value_or_addr_t node_addr, int depth) {
    print_n_times("| ", depth);
    printf(
        "%" PRIu64 ": %s nkeys=%i %s\n",
        node_addr,
        node->is_leaf ? "leaf" : "intnode",
        node->nkeys,
        node->nkeys > MAX_NKEYS ? "(TOO BIG)" : ""
    );

    int iter_limit = node->nkeys > MAX_NKEYS ?  MAX_NKEYS : node->nkeys;

    if (node->is_leaf) {
        for (int i = 0; i < iter_limit; i++) {
            print_n_times("| ", depth + 1);
            printf("%" PRIu64 " => %" PRIu64 "\n", node->keys[i], node->values[i]);
        }
        print_n_times("| ", depth);
        printf("%" PRIu64 " <- NEXT_LEAF\n", NEXT_LEAF_PTR(node));
    } else {
        bt_value_or_addr_t child_addr = node->values[0];
        struct node *child = bt_read_node(bt, child_addr);
        dump_btree_helper(bt, child, child_addr, depth + 1);
        bt_free_block_buf(bt, child);

        for (int i = 0; i < iter_limit; i++) {
            print_n_times("| ", depth);
            printf("%" PRIu64 "---\n", node->keys[i]);

            child_addr = node->values[i + 1];
            child = bt_read_node(bt, child_addr);
            dump_btree_helper(bt, child, child_addr, depth + 1);
            bt_free_block_buf(bt, child);
        }
    }
}

void dump_btree(struct btree *bt) {
    bt_value_or_addr_t root_addr;
    struct node *root = bt_get_root(bt, &root_addr);
    printf("************ START DUMP *************\n");
    dump_btree_helper(bt, root, root_addr, 0);
    printf("************ <END DUMP> *************\n");
    bt_free_block_buf(bt, root);
}

int * gen_range(int n) {
    int *nums = malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) {
        nums[i] = i + 1;
    }
    return nums;
}

void shuffle(int *nums, int n){
    for (int i = 0; i < n - 1; i++) {
        int swap_ind = i + rand() % (n - i);
        int temp = nums[i];
        nums[i] = nums[swap_ind];
        nums[swap_ind] = temp;
    }
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char* argv[]) {
    struct btree bt;
    bt_open(&bt, "cbtree.index");

    srand(time(NULL));
    // srand(0);
    const int size = 2000;
    int *nums = gen_range(size);
    shuffle(nums, size);
    for (int i = 0; i < size; i++) {
        int key = nums[i];
        int value = 100 * key;
        int err = bt_insert(&bt, key, value, 0);
        if (err) {
            fprintf(stderr, "Error during insertion of key %i and value %i\n", key, value);
        }
    }

    shuffle(nums, size);
    const int delete_size = 1200;
    for (int i = 0; i < delete_size; i++) {
        int key = nums[i];

        // dump_btree(&bt);
        int err = bt_delete(&bt, key);
        if (err) {
            fprintf(stderr, "Error during deletion of key %i\n", key);
        }
    }
    free(nums);

    dump_btree(&bt);

    struct key_value_pair *kv = malloc(sizeof(struct key_value_pair) * 100);
    int num_copied = bt_range(&bt, 0, 80, 100, kv);
    for (int i = 0; i < num_copied; i++) {
        printf("elem %i: (%" PRIu64 ", %" PRIu64 ")\n", i, kv[i].key, kv[i].value);
    }
    free(kv);

    int success;
    int value = bt_lookup(&bt, 42, &success);
    printf("lookup %s. value = %i\n", success ? "success" : "failure", value);
    printf("leaked buffers: %i\n", bt.buf_count);
    printf("highest buf count: %i\n", highest_buf_count);
    bt_close(&bt);
    return 0;
}
