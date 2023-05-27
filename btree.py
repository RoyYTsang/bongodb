
"""
First draft finished on May 15, 2023.

Format of a block with N keys:
* 2 bytes: nkeys, the number of keys
* 2 bytes: is_leaf. 1 for leaf, 0 for node.
* 4 bytes: (unused)
* 8*N bytes: keys
* 8*(N+1) bytes: values. In leaves, last value is a pointer to the next leaf.
Total: 16 + 16*N

header block:
* 8 bytes: pointer to root block
* 8 bytes: pointer to free list

Each free block has this format:
* 2 bytes: set to INVALID_NKEYS
* 6 bytes: (unused)
* 8 bytes: pointer to next free block, or null.
"""

import os
import io
import struct

BLOCK_SIZE = 2**12
MAX_NKEYS = (BLOCK_SIZE - 16) // 16
CACHE_SIZE = 8192

OFFSET_ROOT_PTR = 0
OFFSET_FREE_PTR = 8
OFFSET_NEXT_FREE = 8
OFFSET_NKEYS = 0
OFFSET_IS_LEAF = 2
OFFSET_KEYS_START = 8
OFFSET_VALUES_START = OFFSET_KEYS_START + 8*MAX_NKEYS
OFFSET_VALUES_END = OFFSET_VALUES_START + 8*(MAX_NKEYS + 1)

INVALID_NKEYS = 9999

UINT16 = struct.Struct("H")
UINT64 = struct.Struct("Q")

def pack(data_type, buf, offset, data):
    data_type.pack_into(buf, offset, data)

def unpack(data_type, buf, offset):
    return data_type.unpack_from(buf, offset)[0]

READS = 0
WRITES = 0
IOTRACE = [True]
def iotrace_read():
    global READS
    if IOTRACE[-1]:
        READS += 1
def iotrace_write():
    global WRITES
    if IOTRACE[-1]:
        WRITES += 1
def iotrace_scope(enable):
    global IOTRACE
    IOTRACE.append(enable)
def iotrace_pop():
    IOTRACE.pop()
    if len(IOTRACE) == 0:
        raise ValueError("popped last iotrace scope")
def iotrace_reset():
    global READS, WRITES
    READS = 0
    WRITES = 0


class BTree:
    """
    WARNING: ALWAYS call close() on this object. If you don't, data in the
    cache might not be written to disk.
    """

    def __init__(self, file):
        try:
            self.f = io.FileIO(file, "r+b")
        except FileNotFoundError:
            self.f = io.FileIO(file, "x+b")

        st = os.fstat(self.f.fileno())
        self.file_size = st.st_size
        self.bcache = BlockCache(self.f)
        if self.file_size == 0:
            self.init_file()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def close(self):
        self.bcache.flush()
        self.bcache = None
        self.f.close()

    def write_node(self, node):
        self.bcache.write(node.block)

    def init_file(self):
        """Init header block and root."""
        self.extend_file()
        root_addr = self.extend_file()

        # set root pointer to a new root. free list pointer = null.
        header = DiskBlock(0)
        pack(UINT64, header.buf, OFFSET_ROOT_PTR, root_addr)
        pack(UINT64, header.buf, OFFSET_FREE_PTR, 0)
        self.bcache.write(header)

        root = self.node_at(root_addr)
        root.set_nkeys(0)
        root.set_is_leaf(1)
        root.set_next_leaf_ptr(0)
        self.bcache.write(root.block)

    def extend_file(self):
        """Creates a new block. Returns its address."""
        old_size = self.file_size
        # seek past the end of the file and write
        self.f.seek(old_size + BLOCK_SIZE - 1)
        self.f.write(b"\0")
        self.file_size += BLOCK_SIZE
        return old_size

    def new_node(self):
        """Uses the free list if there are free blocks."""
        header = self.bcache.read(0)
        free_ptr = unpack(UINT64, header.buf, OFFSET_FREE_PTR)
        if free_ptr == 0:
            addr = self.extend_file()
            block = DiskBlock(addr)
            return NodeBuf(block)
        else:
            block = self.bcache.read(free_ptr)
            next_free = unpack(UINT64, block.buf, OFFSET_NEXT_FREE)
            pack(UINT64, header.buf, OFFSET_FREE_PTR, next_free)
            self.bcache.write(header)
            return NodeBuf(block)


    def node_at(self, addr):
        return NodeBuf(self.bcache.read(addr))

    def get_root(self):
        header = self.bcache.read(0)
        root_addr = unpack(UINT64, header.buf, OFFSET_ROOT_PTR)
        return self.node_at(root_addr)

    def set_root(self, addr):
        header = self.bcache.read(0)
        pack(UINT64, header.buf, OFFSET_ROOT_PTR, addr)
        self.bcache.write(header)

    def free(self, node):
        header = self.bcache.read(0)
        free_ptr = unpack(UINT64, header.buf, OFFSET_FREE_PTR)
        pack(UINT64, header.buf, OFFSET_FREE_PTR, node.get_addr())
        pack(UINT16, node.block.buf, OFFSET_NKEYS, INVALID_NKEYS)
        pack(UINT64, node.block.buf, OFFSET_NEXT_FREE, free_ptr)
        self.bcache.write(header)
        self.bcache.write(node.block)

    def check_integrity(self):
        """
        Checks that:
        * every node has at most one parent.
        * free list is a single linear list.
        * leaves are connected in a single linear list.
        * every block is either in free list or in the tree (or is header).
        """
        free_list_ptrs = {}
        next_leaf_ptrs = {}
        child_ptrs = {}

        iotrace_scope(False)
        for addr in range(BLOCK_SIZE, self.file_size, BLOCK_SIZE):
            node = self.node_at(addr)
            if node.get_nkeys() == INVALID_NKEYS:
                next_free = unpack(UINT64, node.block.buf, OFFSET_NEXT_FREE)
                free_list_ptrs[addr] = next_free
            elif node.get_is_leaf():
                next_leaf_ptrs[addr] = node.get_next_leaf_ptr()
            else:
                child_ptrs[addr] = node.values.tolist()[:node.get_nkeys()+1]

        # test node parents
        def recurse(n):
            assert n not in tree_blocks, f"Nodes have at most one parent: {n}"
            tree_blocks.add(n)
            if n in next_leaf_ptrs:
                # leaf
                return
            for c in child_ptrs[n]:
                recurse(c)

        tree_blocks = set()
        root_ptr = self.get_root().get_addr()
        recurse(root_ptr)

        # test free list
        if len(free_list_ptrs) > 0:
            free_pointees = set(free_list_ptrs.values())
            free_pointers = set(free_list_ptrs)
            assert len(free_pointers - free_pointees) == 1, \
                "free list must have a single head"
            freeblock = (free_pointers - free_pointees).pop()  # head of free list
            while freeblock != 0:
                free_pointers.remove(freeblock)
                freeblock = free_list_ptrs[freeblock]
            assert len(free_pointers) == 0

        # test leaf list
        leaf_pointees = set(next_leaf_ptrs.values())
        leaf_pointers = set(next_leaf_ptrs)
        assert leaf_pointees - leaf_pointers == {0}
        assert len(leaf_pointers - leaf_pointees) == 1, \
            "leaf list must have a single head"
        leaf = (leaf_pointers - leaf_pointees).pop()  # head of leaf list
        while leaf != 0:
            leaf_pointers.remove(leaf)
            leaf = next_leaf_ptrs[leaf]
        assert len(leaf_pointers) == 0

        # test completeness of free list and tree blocks
        free_blocks = set(free_list_ptrs)
        all_blocks = set(range(BLOCK_SIZE, self.file_size, BLOCK_SIZE))
        orphan_blocks = all_blocks - (free_blocks | tree_blocks)
        assert len(free_blocks & tree_blocks) == 0
        assert len(orphan_blocks) == 0, \
            f"{len(orphan_blocks)} orphan blocks: {orphan_blocks}"

        iotrace_pop()

    def print(self):
        freelist = []
        iotrace_scope(False)
        for addr in range(BLOCK_SIZE, self.file_size, BLOCK_SIZE):
            node = self.node_at(addr)
            if node.get_nkeys() == INVALID_NKEYS:
                next_free = unpack(UINT64, node.block.buf, OFFSET_NEXT_FREE)
                freelist.append((addr, next_free))
                continue
            ntype = "leaf" if node.get_is_leaf() else "node"
            print(f"{addr}: {ntype} nkeys={node.get_nkeys()}")
            print("keys  : " + " ".join(map(str, node.keys)))
            print("values: " + " ".join(map(str, node.values)))

        for addr, next_free in freelist:
            print(f"{addr} -> {next_free}")
        iotrace_pop()

    def get(self, k, default=None):
        assert isinstance(k, int)
        v, success = search(self, k)
        return v if success else default

    def insert(self, k, v):
        self._insert(k, v, False)

    def update(self, k, v):
        self._insert(k, v, True)

    def _insert(self, k, v, allow_update):
        assert isinstance(k, int)
        assert isinstance(v, int)
        new_root_addr = insert(self, k, v, allow_update)
        if new_root_addr is not None:
            self.set_root(new_root_addr)

    def delete(self, k):
        assert isinstance(k, int)
        new_root_addr = delete(self, k)
        if new_root_addr is not None:
            self.set_root(new_root_addr)

    def range(self, *, lo=None, hi=None, limit=None):
        return tree_range(self, lo, hi, limit)

    def depth(self):
        _, d = leftmost_leaf_and_depth(self)
        return d


class DiskBlock:
    __slots__ = ["addr", "buf"]

    def __init__(self, addr):
        self.addr = addr
        self.buf = bytearray(BLOCK_SIZE)

class NodeBuf:
    __slots__ = ["block", "keys", "values"]

    def __init__(self, block):
        self.block = block
        view = memoryview(block.buf)
        self.keys = view[OFFSET_KEYS_START:OFFSET_VALUES_START].cast("Q")
        self.values = view[OFFSET_VALUES_START:OFFSET_VALUES_END].cast("Q")

    def get_addr(self):
        return self.block.addr

    def get_nkeys(self):
        return unpack(UINT16, self.block.buf, OFFSET_NKEYS)

    def set_nkeys(self, n):
        pack(UINT16, self.block.buf, OFFSET_NKEYS, n)

    def get_is_leaf(self):
        return unpack(UINT16, self.block.buf, OFFSET_IS_LEAF)

    def set_is_leaf(self, is_leaf):
        pack(UINT16, self.block.buf, OFFSET_IS_LEAF, is_leaf)

    def get_next_leaf_ptr(self):
        return self.values[MAX_NKEYS]

    def set_next_leaf_ptr(self, addr):
        self.values[MAX_NKEYS] = addr


class _LRUNode:
    """next goes toward the front, i.e. the most recently used."""
    __slots__ = ["prev", "next", "block"]
    def __init__(self):
        self.block, self.prev, self.next = None, None, None

class BlockCache:
    """
    To disable the cache, set cache size to 0.
    """
    def __init__(self, f):
        self.f = f
        self.cache = {}
        self.dirty = set()
        self.end_dummy = _LRUNode()
        self.head_dummy = _LRUNode()
        self.end_dummy.next = self.head_dummy
        self.head_dummy.prev = self.end_dummy

    def _remove(self, node):
        prev, next = node.prev, node.next
        prev.next = next
        next.prev = prev

    def _push(self, node):
        self.head_dummy.prev.next = node
        node.prev = self.head_dummy.prev
        node.next = self.head_dummy
        self.head_dummy.prev = node

    def _access(self, addr):
        if addr in self.cache:
            node = self.cache[addr]
            self._remove(node)
            self._push(node)
            return node
        elif len(self.cache) < CACHE_SIZE:
            node = _LRUNode()
            self._push(node)
            self.cache[addr] = node
            return node
        else:
            # evict LRU
            node = self.end_dummy.next
            del self.cache[node.block.addr]
            if node.block.addr in self.dirty:
                self.dirty.remove(node.block.addr)
                self._write_block(node.block)
            self._remove(node)
            self._push(node)
            self.cache[addr] = node
            node.block = None
            return node

    def read(self, addr):
        if CACHE_SIZE == 0:
            return self._read_block(addr)
        node = self._access(addr)
        if node.block is None:
            node.block = self._read_block(addr)
        return node.block

    def write(self, block):
        if CACHE_SIZE == 0:
            self._write_block(block)
            return
        node = self._access(block.addr)
        node.block = block
        self.dirty.add(block.addr)

    def flush(self):
        for addr in self.dirty:
            self._write_block(self.cache[addr].block)
        self.dirty.clear()

    def _write_block(self, block):
        iotrace_write()
        self.f.seek(block.addr)
        nbytes = self.f.write(block.buf)
        os.fsync(self.f.fileno())
        assert nbytes == len(block.buf)

    def _read_block(self, addr):
        iotrace_read()
        block = DiskBlock(addr)
        self.f.seek(addr)
        self.f.readinto(block.buf)
        return block

#######################################################
#                      Ranges
#######################################################

def tree_range(tree, lo, hi, limit):
    """lo, hi, and limit may be None."""
    if lo is None:
        n, _ = leftmost_leaf_and_depth(tree)
        i = 0
    else:
        n = search_until_leaf(tree, lo)
        i, success = search_leaf(n, lo)

    result = []
    for k, v in iter_leaves(tree, n, i):
        if hi is not None and k > hi:
            return result
        result.append((k, v))
        if limit is not None and len(result) >= limit:
            return result
    return result

def iter_leaves(tree, leaf, i):
    """If i >= nkeys, we start iterating from the next leaf."""
    while True:
        assert leaf.get_is_leaf()
        for j in range(i, leaf.get_nkeys()):
            yield leaf.keys[j], leaf.values[j]
        if leaf.get_next_leaf_ptr() == 0:
            break
        leaf = tree.node_at(leaf.get_next_leaf_ptr())
        i = 0

def leftmost_leaf_and_depth(tree):
    n = tree.get_root()
    i = 1
    while not n.get_is_leaf():
        n = tree.node_at(n.values[0])
        i += 1
    return n, i

#######################################################
#                      Search
#######################################################

def search(tree, k):
    """
    Returns (value, success). If success is False, value is None.
    """
    n = search_until_leaf(tree, k)
    i, success = search_leaf(n, k)
    v = n.values[i] if success else None
    return v, success

def search_until_leaf(tree, k):
    """Returns the leaf that contains k."""
    n = tree.get_root()
    while not n.get_is_leaf():
        i = search_node(n, k)
        n = tree.node_at(n.values[i])
    return n

def search_leaf(leaf, k):
    """
    Returns (index, success). Search for the key, or the correct index at
    which to insert the key. If k is greater than all keys, index will be
    nkeys, so values[index] will not be a valid value.
    """
    left, right = 0, leaf.get_nkeys() - 1
    while left <= right:
        mid = (left + right) >> 1
        midkey = leaf.keys[mid]
        if midkey < k:
            left = mid + 1
        elif midkey > k:
            right = mid - 1
        else:
            return mid, True
    return left, False

def search_node(node, k):
    """Search for the index of the matching child."""
    left, right = 0, node.get_nkeys() - 1
    while left <= right:
        mid = (left + right) >> 1
        midkey = node.keys[mid]
        if midkey < k:
            left = mid + 1
        elif midkey > k:
            right = mid - 1
        else:
            return mid + 1
    return left


#######################################################
#                      Insert
#######################################################

def insert(tree, k, v, allow_update):
    """
    Returns new root address, or None if no new root.
    If allow_update is False, throws a ValueError if key is already in
    the tree. Otherwise, lookup the key and update its value.
    """
    root = tree.get_root()
    new_node, new_key = insert_recurse(tree, root, k, v, allow_update)
    if new_node is not None:
        # split root
        new_root = tree.new_node()
        new_root.set_is_leaf(0)
        new_root.keys[0] = new_key
        new_root.values[0] = root.get_addr()
        new_root.values[1] = new_node.get_addr()
        new_root.set_nkeys(1)
        tree.write_node(new_root)
        return new_root.get_addr()
    return None

def insert_recurse(tree, node, k, v, allow_update):
    if node.get_is_leaf():
        i, success = search_leaf(node, k)
        if success and allow_update:
            update_leaf(tree, node, i, v)
            return None, None
        elif success and not allow_update:
            raise ValueError("insert duplicate key: " + repr(k))
        new_node, new_key = insert_leaf(tree, node, i, k, v)
    else:
        i = search_node(node, k)
        child_addr = node.values[i]
        child = tree.node_at(child_addr)
        new_node, new_key = insert_recurse(tree, child, k, v, allow_update)
        if new_node is not None:
            new_node, new_key = insert_node(tree, node, i, new_key, new_node)
    return new_node, new_key

def update_leaf(tree, node, i, v):
    node.values[i] = v
    tree.write_node(node)

def insert_node(tree, node, i, new_key, new_child):
    """
    :param tree: the BTree.
    :param node:
    :param i: index within node values of the child that was just split.
    :param new_key: new key to be inserted between new_child and the child
    that was just split.
    :param new_child: a new node split from the child at index i.
    :returns: (new node, middle key) if node is full, otherwise (None, None)
    """
    nkeys = node.get_nkeys()
    assert nkeys != INVALID_NKEYS
    if nkeys < MAX_NKEYS:
        node_insert(node, nkeys, i, i+1, new_key, new_child.get_addr())
        tree.write_node(node)
        return None, None
    else:
        # Note: ceil(N/2) == (N+1)//2 for any positive integer N.
        # split keys by bottom ceil(nkeys/2) and top nkeys//2
        # middle key is at index ceil(nkeys/2)
        # split values by ceil((nvalues+1)/2)
        kmidpoint = (MAX_NKEYS + 1) // 2  # index of middle key
        vmidpoint = kmidpoint + 1

        new_node = tree.new_node()
        new_node.set_is_leaf(0)

        # split after midpoint, so middle key stays in old node.
        array_insert_split(node.keys, new_node.keys, MAX_NKEYS,
                           i, kmidpoint+1, new_key)
        middle_key = node.keys[kmidpoint]
        node.keys[kmidpoint] = 0

        # insert new_child at i+1, since i is the index of the child that was
        # just split, and the new child is to the right.
        array_insert_split(node.values, new_node.values, MAX_NKEYS+1,
                           i+1, vmidpoint, new_child.get_addr())

        node.set_nkeys(kmidpoint)
        new_node.set_nkeys(MAX_NKEYS - kmidpoint)
        tree.write_node(node)
        tree.write_node(new_node)
        return new_node, middle_key

def insert_leaf(tree, leaf, i, k, v):
    """
    Returns (new leaf, new leaf's smallest key)
    If leaf is full, modifies the leaf and returns the new leaf split
    from the old one. Otherwise, returns (None, None).
    """
    nkeys = leaf.get_nkeys()
    assert nkeys != INVALID_NKEYS
    if nkeys < MAX_NKEYS:
        leaf_insert(leaf, nkeys, i, i, k, v)
        tree.write_node(leaf)
        return None, None
    else:
        # ceil((N+1)/2) == (N+2)//2
        midpoint = (nkeys + 2) // 2
        new_leaf = tree.new_node()
        new_leaf.set_is_leaf(1)
        new_leaf.set_next_leaf_ptr(leaf.get_next_leaf_ptr())

        array_insert_split(leaf.keys, new_leaf.keys, nkeys, i, midpoint, k)
        array_insert_split(leaf.values, new_leaf.values, nkeys, i, midpoint, v)

        leaf.set_nkeys(midpoint)
        leaf.set_next_leaf_ptr(new_leaf.get_addr())
        new_leaf.set_nkeys(MAX_NKEYS - midpoint + 1)  # includes new key
        tree.write_node(leaf)
        tree.write_node(new_leaf)
        return new_leaf, new_leaf.keys[0]

def array_insert_split(arr, new_arr, arr_len, index, midpoint, x):
    """
    :param arr: The array to split. It is modified in place.
    :param new_arr: The new array. It is modified in place.
    :param arr_len: length of arr.
    :param index: Where to insert x. May be arr_len, which is one greater than
    the maximum index within arr.
    :param midpoint: Where to split. First index that is copied to new array.
    :param x: The element to insert.
    """
    assert 0 <= index <= arr_len
    if index < midpoint:
        # copy to new array
        for i in range(midpoint-1, arr_len):
            new_arr[i - midpoint + 1] = arr[i]
            arr[i] = 0

        # insert
        for i in range(midpoint-1, index, -1):
            arr[i] = arr[i - 1]
        arr[index] = x
    else:
        # copy to new array the elements before insertion
        for i in range(midpoint, index):
            new_arr[i - midpoint] = arr[i]
            arr[i] = 0

        new_arr[index - midpoint] = x

        # copy after insertion
        for i in range(index, arr_len):
            new_arr[i - midpoint + 1] = arr[i]
            arr[i] = 0

def array_insert(arr, arr_len, index, x):
    for i in range(arr_len, index, -1):
        arr[i] = arr[i - 1]
    arr[index] = x

def leaf_insert(leaf, nkeys, ind_k, ind_v, k, v):
    assert leaf.get_is_leaf()
    array_insert(leaf.keys, nkeys, ind_k, k)
    array_insert(leaf.values, nkeys, ind_v, v)
    leaf.set_nkeys(nkeys + 1)

def node_insert(node, nkeys, ind_k, ind_v, k, v):
    assert not node.get_is_leaf()
    array_insert(node.keys, nkeys, ind_k, k)
    array_insert(node.values, nkeys + 1, ind_v, v)
    node.set_nkeys(nkeys + 1)


#######################################################
#                      Delete
#######################################################

MIN_NKEYS_LEAF = (MAX_NKEYS + 1) // 2
MIN_NKEYS_NODE = MAX_NKEYS // 2

def delete_raise_error(k):
    raise ValueError("delete a key that is not in tree: " + repr(k))

def delete(tree, k):
    root = tree.get_root()
    assert root.get_nkeys() != INVALID_NKEYS
    if root.get_is_leaf():
        if root.get_nkeys() == 0:
            raise ValueError("delete from an empty tree")
        i, success = search_leaf(root, k)
        if not success:
            delete_raise_error(k)
        leaf_pop(root, i, i)
        tree.write_node(root)
        return None

    i = search_node(root, k)
    child_addr = root.values[i]
    child = tree.node_at(child_addr)
    del_ind, p_modified = delete_recurse(tree, child, root, i, k)
    if del_ind is not None:
        # delete from root
        # make child the root if it's the only one left
        node_pop(root, del_ind, del_ind+1)
        if root.get_nkeys() == 0:
            tree.free(root)
            return root.values[0]
    if p_modified:
        tree.write_node(root)
    return None

def delete_recurse(tree, node, parent, p_ind, k):
    if node.get_is_leaf():
        i, success = search_leaf(node, k)
        if not success:
            delete_raise_error(k)
        del_ind, p_modified = delete_leaf(tree, node, parent, p_ind, i)
        return del_ind, p_modified
    else:
        i = search_node(node, k)
        child_addr = node.values[i]
        child = tree.node_at(child_addr)
        node_del_ind, node_modified = delete_recurse(tree, child, node, i, k)
        if node_del_ind is not None:
            del_ind, p_modified = delete_node(tree, node, parent,
                                              p_ind, node_del_ind)
            return del_ind, p_modified
        if node_modified:
            tree.write_node(node)
        return None, False

def delete_node(tree, node, parent, p_ind, i):
    """
    This function may modify parent's keys. Writes node if it won't be
    deleted by the delete algorithm.
    :param tree:
    :param node:
    :param parent: parent of node.
    :param p_ind: index within parent of node.
    :param i: index within node of the key to delete. The value to delete is
    at index i+1.
    :return: (index, p_modified). index is of the key of parent that
    should be deleted. None if no such element. p_modified is True if the
    parent has been modified or index is not None.
    """
    assert node.get_nkeys() != INVALID_NKEYS
    node_pop(node, i, i+1)
    nkeys = node.get_nkeys()
    if nkeys >= MIN_NKEYS_NODE:
        tree.write_node(node)
        return None, False

    left_sib, right_sib, left_sib_nkeys, right_sib_nkeys \
        = get_sibs_and_nkeys(tree, MIN_NKEYS_NODE, parent, p_ind)
    if left_sib_nkeys > MIN_NKEYS_NODE or right_sib_nkeys > MIN_NKEYS_NODE:
        # When moving a value from a sibling, the key in the parent between
        # the node and the sibling is pulled down, and the key from the
        # sibling is pulled up to the parent.
        if left_sib_nkeys > right_sib_nkeys:
            parent_key = parent.keys[p_ind - 1]
            key, value = node_pop(left_sib, left_sib_nkeys-1, left_sib_nkeys)
            node_insert(node, nkeys, 0, 0, parent_key, value)
            parent.keys[p_ind - 1] = key
            tree.write_node(left_sib)
        else:
            parent_key = parent.keys[p_ind]
            key, value = node_pop(right_sib, 0, 0)
            node_insert(node, nkeys, nkeys, nkeys+1, parent_key, value)
            parent.keys[p_ind] = key
            tree.write_node(right_sib)
        tree.write_node(node)
        return None, True
    else:
        # merge with one sibling
        # always move elements from right to left
        if left_sib is not None:
            merge_right = node
            merge_right_nkeys = nkeys
            merge_left = left_sib
            merge_left_nkeys = left_sib_nkeys
            ind_key_between = p_ind - 1
        else:
            merge_right = right_sib
            merge_right_nkeys = right_sib_nkeys
            merge_left = node
            merge_left_nkeys = nkeys
            ind_key_between = p_ind

        # when merging siblings, the key between them in the parent is pulled
        # down to the newly merged node.
        array_insert(merge_left.keys, merge_left_nkeys,
                     merge_left_nkeys, parent.keys[ind_key_between])

        # Since the key was pulled down and appended to merge_left.keys,
        # merge_left_nkeys is one less than the real number of keys.
        # However, merge_left_nkeys+1 is still the real number of values.
        array_merge_append(merge_right.keys, merge_right_nkeys,
                           merge_left.keys, merge_left_nkeys+1)
        array_merge_append(merge_right.values, merge_right_nkeys+1,
                           merge_left.values, merge_left_nkeys+1)
        merge_left.set_nkeys(merge_right_nkeys + merge_left_nkeys + 1)

        tree.free(merge_right)
        tree.write_node(merge_left)
        return ind_key_between, True

def delete_leaf(tree, leaf, parent, p_ind, i):
    """
    This function may modify parent's keys. Writes node if it won't be
    deleted by the delete algorithm.
    :param tree:
    :param leaf:
    :param parent: parent of leaf
    :param p_ind: index within parent of leaf.
    :param i: index within leaf of element to delete.
    :return: (index, p_modified). index is of the key of parent that
    should be deleted. None if no such element. p_modified is True if the
    parent has been modified or index is not None.
    """
    assert leaf.get_nkeys() != INVALID_NKEYS
    leaf_pop(leaf, i, i)
    nkeys = leaf.get_nkeys()
    if nkeys >= MIN_NKEYS_LEAF:
        tree.write_node(leaf)
        return None, False

    left_sib, right_sib, left_sib_nkeys, right_sib_nkeys \
        = get_sibs_and_nkeys(tree, MIN_NKEYS_LEAF, parent, p_ind)
    if left_sib_nkeys > MIN_NKEYS_LEAF or right_sib_nkeys > MIN_NKEYS_LEAF:
        # If at least one sibling is above the min capacity, move an element
        # from that sibling, and change the key between them in the parent.
        if left_sib_nkeys > right_sib_nkeys:
            key, value = leaf_pop(left_sib, left_sib_nkeys-1, left_sib_nkeys-1)
            leaf_insert(leaf, nkeys, 0, 0, key, value)
            parent.keys[p_ind - 1] = key
            tree.write_node(left_sib)
        else:
            key, value = leaf_pop(right_sib, 0, 0)
            leaf_insert(leaf, nkeys, nkeys, nkeys, key, value)
            parent.keys[p_ind] = right_sib.keys[0]
            tree.write_node(right_sib)
        tree.write_node(leaf)
        return None, True
    else:
        # If neither sibling is above min capacity, pick one to merge.
        # Delete the newly emptied node in the parent.
        # Always move elements from right to left so the next_leaf_ptr of
        # the prev leaf remains valid.
        if left_sib is not None:
            merge_right = leaf
            merge_right_nkeys = nkeys
            merge_left = left_sib
            merge_left_nkeys = left_sib_nkeys
            ind_key_between = p_ind - 1
        else:
            merge_right = right_sib
            merge_right_nkeys = right_sib_nkeys
            merge_left = leaf
            merge_left_nkeys = nkeys
            ind_key_between = p_ind

        array_merge_append(merge_right.keys, merge_right_nkeys,
                           merge_left.keys, merge_left_nkeys)
        array_merge_append(merge_right.values, merge_right_nkeys,
                           merge_left.values, merge_left_nkeys)
        merge_left.set_nkeys(merge_right_nkeys + merge_left_nkeys)
        merge_left.set_next_leaf_ptr(merge_right.get_next_leaf_ptr())

        tree.free(merge_right)
        tree.write_node(merge_left)
        return ind_key_between, True

def get_sibs_and_nkeys(tree, min_nkeys, parent_node, i):
    """
    sibs are None if they don't exist.
    sib nkeys are 0 if the sib doesn't exist.
    :param tree:
    :param min_nkeys: If left sib has greater than min_nkeys, don't bother
    to read right sib.
    :param parent_node:
    :param i: index within parent
    :return: (left sib, right sib, left sib nkeys, right sib nkeys)
    """
    # avoid reading right sibling if left is known to be good.
    left_sib = tree.node_at(parent_node.values[i-1]) if i > 0 else None
    left_sib_nkeys = left_sib.get_nkeys() if left_sib else 0
    if left_sib is not None and left_sib_nkeys > min_nkeys:
        return left_sib, None, left_sib_nkeys, 0
    right_sib = tree.node_at(parent_node.values[i+1]) if i < parent_node.get_nkeys() else None
    right_sib_nkeys = right_sib.get_nkeys() if right_sib else 0
    return left_sib, right_sib, left_sib_nkeys, right_sib_nkeys

def array_delete(arr, arr_len, index):
    assert arr_len <= len(arr)
    assert arr_len > 0, "delete from empty array"
    # move all later elements back, overwriting the deleted element
    for i in range(index+1, arr_len):
        arr[i-1] = arr[i]
    arr[arr_len-1] = 0

def array_merge_append(src, src_len, dest, dest_len):
    assert src_len + dest_len <= len(dest)
    for i in range(src_len):
        dest[i+dest_len] = src[i]

def node_pop(node, ind_k, ind_v):
    assert not node.get_is_leaf()
    key = node.keys[ind_k]
    value = node.values[ind_v]
    nkeys = node.get_nkeys()
    array_delete(node.keys, nkeys, ind_k)
    array_delete(node.values, nkeys + 1, ind_v)
    node.set_nkeys(nkeys - 1)
    return key, value

def leaf_pop(leaf, ind_k, ind_v):
    assert leaf.get_is_leaf()
    key = leaf.keys[ind_k]
    value = leaf.values[ind_v]
    nkeys = leaf.get_nkeys()
    array_delete(leaf.keys, nkeys, ind_k)
    array_delete(leaf.values, nkeys, ind_v)
    leaf.set_nkeys(nkeys - 1)
    return key, value
