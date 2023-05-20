
# finished on May 12, 2023

class BTree:
    def __init__(self, n):
        assert n >= 2
        self.N = n
        self.size = 0
        self.depth = 1
        self.root = Node(n, True)

    def get(self, k, default=None):
        v, success = search(k, self.root)
        return v if success else default

    def insert(self, k, v):
        old_root = self.root
        self.root = insert(k, v, self.root)
        if self.root is not old_root:
            self.depth += 1
        self.size += 1

    def delete(self, k):
        if self.size == 0:
            raise ValueError("delete on empty tree")
        old_root = self.root
        self.root = delete(k, self.root)
        if self.root is not old_root:
            self.depth -= 1
        self.size -= 1

    def __iter__(self):
        yield from iter_node(self.root)

    def __repr__(self):
        return f"N={self.N}\n" + "\n".join(self.root._repr_lines())

class Node:
    __slots__ = "N keys nkeys values nvalues is_leaf".split()

    def __init__(self, n, leaf):
        self.N = n
        self.keys: list = [None] * n
        self.nkeys = 0
        self.values: list = [None] * (n + 1)
        self.nvalues = 0
        self.is_leaf = leaf

    def __repr__(self):
        return "Node" + repr((self.keys, self.values))

    def _repr_lines(self):
        ntype = "Leaf" if self.is_leaf else "Node"
        lines = [f"{ntype} {self.nvalues}"]
        if self.is_leaf:
            for i in range(self.nkeys):
                lines.append(f"|   {self.keys[i]} -> {self.values[i]}")
        else:
            for i in range(self.nkeys):
                for ln in self.values[i]._repr_lines():
                    lines.append("|   " + ln)
                lines.append(f"|   {self.keys[i]}")
            for ln in self.values[self.nvalues - 1]._repr_lines():
                lines.append("|   " + ln)
        return lines


    def asserts(self):
        assert len(self.keys) == self.N
        assert len(self.values) == self.N + 1
        if self.is_leaf:
            assert self.nkeys == self.nvalues
        else:
            assert self.nkeys == self.nvalues - 1
        assert 0 <= self.nkeys <= len(self.keys)

        prev_key = None
        for i in range(self.nkeys):
            assert self.keys[i] is not None
            assert prev_key is None or self.keys[i] > prev_key,\
                "keys not in ascending order"
            prev_key = self.keys[i]
        for i in range(self.nkeys, len(self.keys)):
            assert self.keys[i] is None

        for i in range(self.nvalues):
            assert self.values is not None
        for i in range(self.nvalues, len(self.values)):
            assert self.values[i] is None

    def assert_min_capacity(self):
        assert not under_min_nkeys(self)


def iter_node(node):
    if node.is_leaf:
        for i in range(node.nkeys):
            yield node.keys[i], node.values[i]
    else:
        for i in range(node.nvalues):
            yield from iter_node(node.values[i])

def search(k, root):
    """
    Returns (value, success). If success is False, value is None.
    """
    n = root
    while True:
        if n.is_leaf:
            i, success = search_leaf(k, n)
            v = n.values[i] if success else None
            return v, success
        i = search_node(k, n)
        n = n.values[i]

def search_node(k, node):
    """Search for the index of the matching child."""
    node.asserts()
    for i in range(node.nkeys):
        # find the smallest key greater than k
        if k < node.keys[i]:
            return i
    return node.nkeys

def search_leaf(k, leaf):
    """
    Returns (index, success). Search for the value, or where it would be if
    it were in this leaf.  If k is greater than all keys, index will be
    leaf.nkeys, so leaf.values[index] may cause an IndexError.
    """
    leaf.asserts()
    for i in range(leaf.nkeys):
        if k == leaf.keys[i]:
            return i, True
        if k < leaf.keys[i]:
            return i, False
    return leaf.nkeys, False

#######################################################
#                      Insert
#######################################################

def insert(k, v, root):
    """Throws a ValueError if key is already in tree."""
    new_node, new_key = insert_recurse(k, v, root)
    if new_node is not None:
        # split root
        new_root = Node(root.N, False)
        new_root.keys[0] = new_key
        new_root.values[0] = root
        new_root.values[1] = new_node
        new_root.nkeys = 1
        new_root.nvalues = 2
        new_root.asserts()
        return new_root
    return root

def insert_recurse(k, v, node):
    if node.is_leaf:
        i, success = search_leaf(k, node)
        if success:
            raise ValueError("insert duplicate key: " + repr(k))
        new_node, new_key = insert_leaf(k, v, i, node)
    else:
        i = search_node(k, node)
        child = node.values[i]
        new_node, new_key = insert_recurse(k, v, child)
        if new_node is not None:
            new_node, new_key = insert_node(new_key, new_node, i, node)
    return new_node, new_key

def insert_node(new_key, new_child, i, node):
    """
    :param new_key: new key to be inserted between new_child and the child
    that was just split.
    :param new_child: a new node split from the child at index i.
    :param i: index within node.values of the child that was just split.
    :param node:
    :returns: (new node, middle key) if node is full, otherwise (None, None)
    """
    assert not node.is_leaf
    node.asserts()
    if node.nkeys < len(node.keys):
        node_insert(new_key, new_child, i, i+1, node)
        return None, None
    else:
        # Note: ceil(N/2) == (N+1)//2 for any positive integer N.
        # split keys by bottom ceil(nkeys/2) and top nkeys//2
        # middle key is at index ceil(nkeys/2)
        # split values by ceil((nvalues+1)/2)
        kmidpoint = (node.nkeys + 1) // 2  # index of middle key
        vmidpoint = (node.nvalues + 2) // 2

        # split after midpoint, so middle key stays in old node.
        new_keys = array_insert_split(i, new_key, kmidpoint + 1, node.keys)
        middle_key = node.keys[kmidpoint]
        node.keys[kmidpoint] = None

        # insert new_child at i+1, since i is the index of the child that was
        # just split, and the new child is to the right.
        new_values = array_insert_split(i+1, new_child, vmidpoint, node.values)

        # modify old node
        node.nkeys = kmidpoint
        node.nvalues = vmidpoint
        node.asserts()
        node.assert_min_capacity()

        # create new node
        new_node = Node(node.N, False)
        new_node.keys = new_keys
        new_node.values = new_values
        new_node.nkeys = len(node.keys) - kmidpoint
        new_node.nvalues = len(node.values) - vmidpoint + 1
        new_node.asserts()
        new_node.assert_min_capacity()
        return new_node, middle_key

def insert_leaf(k, v, i, leaf):
    """
    Returns (new leaf, new leaf's smallest key)
    If leaf is full, modifies the leaf and returns the new leaf split
    from the old one. Otherwise, returns (None, None).
    """
    assert leaf.is_leaf
    leaf.asserts()
    if leaf.nkeys < len(leaf.keys):
        node_insert(k, v, i, i, leaf)
        return None, None
    else:
        # ceil((N+1)/2) == (N+2)//2
        midpoint = (leaf.nkeys + 2) // 2

        new_keys = array_insert_split(i, k, midpoint, leaf.keys)
        new_values = array_insert_split(i, v, midpoint, leaf.values)

        # modify old leaf
        leaf.nkeys = midpoint
        leaf.nvalues = midpoint
        leaf.asserts()
        leaf.assert_min_capacity()

        # create new leaf
        new_leaf = Node(leaf.N, True)
        new_leaf.keys = new_keys
        new_leaf.values = new_values
        new_leaf.nkeys = len(leaf.keys) - midpoint + 1  # includes new key
        new_leaf.nvalues = new_leaf.nkeys
        new_leaf.asserts()
        new_leaf.assert_min_capacity()
        return new_leaf, new_leaf.keys[0]

def array_insert_split(index, x, midpoint, arr):
    """
    :param index: Where to insert x. May be len(arr), which is one greater than
    the maximum index within arr.
    :param x: The element to insert.
    :param midpoint: Where to split. First index that is copied to new array.
    :param arr: The array to split. It is modified in place.
    :return: The new array. The upper half of arr is copied here.
    """
    assert 0 <= index <= len(arr)
    new_arr = [None] * len(arr)
    if index < midpoint:
        # copy to new array
        for i in range(midpoint-1, len(new_arr)):
            new_arr[i - midpoint + 1] = arr[i]
            arr[i] = None

        # insert
        for i in range(midpoint-1, index, -1):
            arr[i] = arr[i - 1]
        arr[index] = x
    else:
        # copy to new array the elements before insertion
        for i in range(midpoint, index):
            new_arr[i - midpoint] = arr[i]
            arr[i] = None

        new_arr[index - midpoint] = x

        # copy after insertion
        for i in range(index, len(new_arr)):
            new_arr[i - midpoint + 1] = arr[i]
            arr[i] = None
    return new_arr

def array_insert(index, x, arr, arr_len):
    assert arr_len < len(arr)
    assert arr[arr_len] is None, "insert into full array"
    for i in range(arr_len, index, -1):
        arr[i] = arr[i - 1]
    arr[index] = x

def node_insert(k, v, ind_k, ind_v, node):
    array_insert(ind_k, k, node.keys, node.nkeys)
    array_insert(ind_v, v, node.values, node.nvalues)
    node.nkeys += 1
    node.nvalues += 1

def node_insert_right(k, v, node):
    node_insert(k, v, node.nkeys, node.nvalues, node)

#######################################################
#                      Delete
#######################################################

def under_min_nkeys(node):
    if node.is_leaf:
        return node.nkeys < min_nkeys_leaf(node)
    else:
        return node.nkeys < min_nkeys_node(node)

def min_nkeys_leaf(leaf):
    return (len(leaf.keys)+1)//2

def min_nkeys_node(node):
    return len(node.keys) // 2

def delete_raise_error(k):
    raise ValueError("delete a key that is not in tree: " + repr(k))

def delete(k, root):
    """Returns new root. Throws a ValueError if key is not in tree."""
    if root.is_leaf:
        assert root.nkeys > 0
        i, success = search_leaf(k, root)
        if not success:
            delete_raise_error(k)
        node_pop(i, i, root)
        return root

    i = search_node(k, root)
    child = root.values[i]
    del_ind = delete_recurse(k, root, i, child)
    if del_ind is not None:
        # delete from root
        # make child the root if it's the only one left
        node_pop(del_ind, del_ind, root)
        if root.nkeys == 0:
            return root.values[0]
    return root

def delete_recurse(k, parent, p_ind, node):
    """Returns index in parent to delete"""
    if node.is_leaf:
        i, success = search_leaf(k, node)
        if not success:
            delete_raise_error(k)
        return delete_leaf(i, parent, p_ind, node)
    else:
        i = search_node(k, node)
        child = node.values[i]
        del_ind = delete_recurse(k, node, i, child)
        if del_ind is not None:
            return delete_node(del_ind, parent, p_ind, node)
        return None

def delete_node(i, parent, p_ind, node):
    """
    This function may modify parent's keys.
    :param i: index within node of both the key and the value to delete.
    :param parent: parent of node.
    :param p_ind: index within parent of node.
    :param node:
    :return: index of a child of parent that should be deleted. None if no
    such element.
    """
    node.asserts()
    node_pop(i, i, node)
    min_nkeys = min_nkeys_node(node)
    if node.nkeys >= min_nkeys:
        return None

    left_sib, right_sib, left_sib_nkeys, right_sib_nkeys \
        = get_sibs_and_nkeys(p_ind, parent)
    if left_sib_nkeys > min_nkeys or right_sib_nkeys > min_nkeys:
        # When moving a value from a sibling, the key in the parent between
        # the node and the sibling is pulled down, and the key from the
        # sibling is pulled up to the parent.
        if left_sib_nkeys > right_sib_nkeys:
            parent_key = parent.keys[p_ind - 1]
            key, value = node_pop_right(left_sib)
            node_insert(parent_key, value, 0, 0, node)
            parent.keys[p_ind - 1] = key
            left_sib.asserts()
            left_sib.assert_min_capacity()
        else:
            parent_key = parent.keys[p_ind]
            key, value = node_pop(0, 0, right_sib)
            node_insert_right(parent_key, value, node)
            parent.keys[p_ind] = key
            right_sib.asserts()
            right_sib.assert_min_capacity()
        node.asserts()
        node.assert_min_capacity()
        return None
    else:
        # merge with one sibling
        # always move elements from left to right
        if left_sib is not None:
            merge_left = left_sib
            merge_right = node
            ind_key_between = p_ind - 1
        else:
            merge_left = node
            merge_right = right_sib
            ind_key_between = p_ind

        # when merging siblings, the key between them in the parent is pulled
        # down to the newly merged node.
        array_insert(0, parent.keys[ind_key_between],
                     merge_right.keys, merge_right.nkeys)
        array_merge_prepend(merge_left.keys, merge_left.nkeys,
                            merge_right.keys, merge_right.nkeys+1)
        array_merge_prepend(merge_left.values, merge_left.nvalues,
                            merge_right.values, merge_right.nvalues)
        merge_right.nkeys += merge_left.nkeys + 1
        merge_right.nvalues += merge_left.nvalues
        merge_right.asserts()
        merge_right.assert_min_capacity()
        return ind_key_between

def delete_leaf(i, parent, p_ind, leaf):
    """
    This function may modify parent's keys.
    :param i: index within leaf of element to delete.
    :param parent: parent of leaf
    :param p_ind: index within parent of leaf.
    :param leaf:
    :return: index of a child of parent that should be deleted. None if no
    such element.
    """
    leaf.asserts()
    node_pop(i, i, leaf)
    min_nkeys = min_nkeys_leaf(leaf)
    if leaf.nkeys >= min_nkeys:
        return None

    left_sib, right_sib, left_sib_size, right_sib_size \
        = get_sibs_and_nkeys(p_ind, parent)
    if left_sib_size > min_nkeys or right_sib_size > min_nkeys:
        # If at least one sibling is above the min capacity, move an element
        # from that sibling, and change the key between them in the parent.
        if left_sib_size > right_sib_size:
            key, value = node_pop_right(left_sib)
            node_insert(key, value, 0, 0, leaf)
            parent.keys[p_ind - 1] = key
            left_sib.asserts()
            left_sib.assert_min_capacity()
        else:
            key, value = node_pop(0, 0, right_sib)
            node_insert_right(key, value, leaf)
            parent.keys[p_ind] = right_sib.keys[0]
            right_sib.asserts()
            right_sib.assert_min_capacity()
        leaf.asserts()
        leaf.assert_min_capacity()
        return None
    else:
        # If neither sibling is above min capacity, pick one to merge.
        # Delete the newly emptied node in the parent.
        # Always move elements from left to right so the key and the value to
        # delete in the parent have the same index.
        if left_sib is not None:
            merge_left = left_sib
            merge_right = leaf
            ind_key_between = p_ind - 1
        else:
            merge_left = leaf
            merge_right = right_sib
            ind_key_between = p_ind

        array_merge_prepend(merge_left.keys, merge_left.nkeys,
                            merge_right.keys, merge_right.nkeys)
        array_merge_prepend(merge_left.values, merge_left.nvalues,
                            merge_right.values, merge_right.nvalues)
        merge_right.nkeys += merge_left.nkeys
        merge_right.nvalues += merge_left.nvalues
        merge_right.asserts()
        merge_right.assert_min_capacity()
        return ind_key_between

def get_sibs_and_nkeys(i, parent_node):
    """
    sibs are None if they don't exist.
    sib nkeys are 0 if the sib doesn't exist.
    :param i: index within parent
    :param parent_node:
    :return: (left sib, right sib, left sib nkeys, right sib nkeys)
    """
    left_sib = parent_node.values[i-1] if i > 0 else None
    right_sib = parent_node.values[i+1] if i < parent_node.nvalues-1 else None
    left_sib_nkeys = left_sib.nkeys if left_sib else 0
    right_sib_nkeys = right_sib.nkeys if right_sib else 0
    return left_sib, right_sib, left_sib_nkeys, right_sib_nkeys

def array_delete(index, arr, arr_len):
    assert arr_len <= len(arr)
    assert arr_len > 0, "delete from empty array"
    # move all later elements back, overwriting the deleted element
    for i in range(index+1, arr_len):
        arr[i-1] = arr[i]
    arr[arr_len-1] = None

def array_merge_prepend(src, src_len, dest, dest_len):
    assert src_len + dest_len <= len(dest)
    for i in range(src_len+dest_len-1, src_len-1, -1):
        dest[i] = dest[i-src_len]
    for i in range(src_len):
        dest[i] = src[i]

def node_pop(ind_k, ind_v, node):
    """
    Deletes and returns key and value. Adjusts nkeys and nvalues.
    :param ind_k: index of key
    :param ind_v: index of value
    :param node:
    :return: (key, value)
    """
    key = node.keys[ind_k]
    value = node.values[ind_v]
    array_delete(ind_k, node.keys, node.nkeys)
    array_delete(ind_v, node.values, node.nvalues)
    node.nkeys -= 1
    node.nvalues -= 1
    return key, value

def node_pop_right(node):
    return node_pop(node.nkeys-1, node.nvalues-1, node)


if __name__ == "__main__":
    import random
    nums = list(range(1000))
    random.shuffle(nums)
    ins_1, ins_2 = nums[:600], nums[600:]
    random.shuffle(nums)
    del_1 = ins_1[:500]
    del_2 = ins_1[500:] + ins_2[:350]
    random.shuffle(del_1)
    random.shuffle(del_2)

    t = BTree(2)

    for num in ins_1:
        t.insert(num, num + 10000)

    for num in del_1:
        t.delete(num)

    for num in ins_2:
        t.insert(num, num + 10000)

    for num in del_2:
        t.delete(num)
