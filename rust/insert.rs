use crate::block::{BufView, Key, NodeBuf, ValueOrAddr, INVALID_NKEYS, MAX_NKEYS};
use crate::file::NodeFile;
use crate::lookup::{lookup_leaf, lookup_node};

pub enum InsertResult {
    NewRoot(ValueOrAddr),
    SameRoot,
    DuplicateKey,
}

enum RecurseResult {
    NewNode { new_node: NodeBuf, new_key: Key },
    NoNew,
    DuplicateKey,
}

pub fn insert(
    nfile: &mut NodeFile,
    key: Key,
    value: ValueOrAddr,
    allow_update: bool,
) -> InsertResult {
    let mut root = nfile.get_root();
    match insert_recurse(nfile, &mut root, key, value, allow_update) {
        RecurseResult::NewNode { new_node, new_key } => {
            let mut new_root = nfile.new_node();
            new_root.set_is_leaf(false);
            new_root.set_key(0, new_key);
            new_root.set_value(0, root.get_addr());
            new_root.set_value(1, new_node.get_addr());
            new_root.set_nkeys(1);
            nfile.write_node(&new_root);
            InsertResult::NewRoot(new_root.get_addr())
        }
        RecurseResult::NoNew => InsertResult::SameRoot,
        RecurseResult::DuplicateKey => InsertResult::DuplicateKey,
    }
}

fn insert_recurse(
    nfile: &mut NodeFile,
    node: &mut NodeBuf,
    key: Key,
    value: ValueOrAddr,
    allow_update: bool,
) -> RecurseResult {
    if node.is_leaf() {
        let (i, success) = lookup_leaf(node, key);
        if success && allow_update {
            update_leaf(nfile, node, i, value);
        }
        if success {
            RecurseResult::DuplicateKey
        } else if let Some((new_node, new_key)) = insert_leaf(nfile, node, i, key, value) {
            RecurseResult::NewNode { new_node, new_key }
        } else {
            RecurseResult::NoNew
        }
    } else {
        let i = lookup_node(node, key);
        let child_addr = node.get_value(i);
        let mut child = nfile.read_node(child_addr);
        match insert_recurse(nfile, &mut child, key, value, allow_update) {
            RecurseResult::NewNode { new_node, new_key } => {
                if let Some((new_node, new_key)) = insert_node(nfile, node, i, new_key, &new_node) {
                    RecurseResult::NewNode { new_node, new_key }
                } else {
                    RecurseResult::NoNew
                }
            }
            RecurseResult::NoNew => RecurseResult::NoNew,
            RecurseResult::DuplicateKey => RecurseResult::DuplicateKey,
        }
    }
}

fn update_leaf(nfile: &mut NodeFile, node: &mut NodeBuf, i: usize, value: ValueOrAddr) {
    node.set_value(i, value);
    nfile.write_node(node);
}

fn insert_node(
    nfile: &mut NodeFile,
    node: &mut NodeBuf,
    i: usize,
    new_key: Key,
    new_child: &NodeBuf,
) -> Option<(NodeBuf, Key)> {
    let nkeys = node.get_nkeys();
    assert_ne!(nkeys, INVALID_NKEYS);
    if nkeys < MAX_NKEYS {
        node_insert(node, i, i + 1, new_key, new_child.get_addr());
        nfile.write_node(node);
        None
    } else {
        // Note: ceil(N/2) == (N+1)//2 for any positive integer N.
        // split keys by bottom ceil(nkeys/2) and top nkeys//2
        // middle key is at index ceil(nkeys/2)
        // split values by ceil((nvalues+1)/2)
        let kmidpoint = (MAX_NKEYS + 1) / 2; // index of middle key
        let vmidpoint = kmidpoint + 1;
        let mut new_node = nfile.new_node();
        new_node.set_is_leaf(false);

        // split after midpoint, so middle key stays in old node.
        array_insert_split(
            node.key_view(),
            new_node.key_view(),
            MAX_NKEYS,
            i,
            kmidpoint + 1,
            new_key,
        );
        let middle_key = node.get_key(kmidpoint);
        node.set_key(kmidpoint, 0);

        // insert new_child at i+1, since i is the index of the child that was
        // just split, and the new child is to the right.
        array_insert_split(
            node.value_view(),
            new_node.value_view(),
            MAX_NKEYS + 1,
            i + 1,
            vmidpoint,
            new_child.get_addr(),
        );

        node.set_nkeys(kmidpoint);
        new_node.set_nkeys(MAX_NKEYS - kmidpoint);
        nfile.write_node(node);
        nfile.write_node(&new_node);
        Some((new_node, middle_key))
    }
}

fn insert_leaf(
    nfile: &mut NodeFile,
    leaf: &mut NodeBuf,
    i: usize,
    key: Key,
    value: ValueOrAddr,
) -> Option<(NodeBuf, Key)> {
    let nkeys = leaf.get_nkeys();
    assert_ne!(nkeys, INVALID_NKEYS);
    if nkeys < MAX_NKEYS {
        leaf_insert(leaf, i, i, key, value);
        nfile.write_node(leaf);
        None
    } else {
        // ceil((N+1)/2) == (N+2)//2
        let midpoint = (nkeys + 2) / 2;
        let mut new_leaf = nfile.new_node();
        new_leaf.set_is_leaf(true);
        // new_leaf.set_next_leaf_ptr(leaf.get_next_leaf_ptr())

        array_insert_split(
            leaf.key_view(),
            new_leaf.key_view(),
            nkeys,
            i,
            midpoint,
            key,
        );
        array_insert_split(
            leaf.value_view(),
            new_leaf.value_view(),
            nkeys,
            i,
            midpoint,
            value,
        );

        leaf.set_nkeys(midpoint);
        // leaf.set_next_leaf_ptr(new_leaf.get_addr());
        new_leaf.set_nkeys(MAX_NKEYS - midpoint + 1); // includes new key
        nfile.write_node(leaf);
        nfile.write_node(&new_leaf);
        let smallest_key = new_leaf.get_key(0);
        Some((new_leaf, smallest_key))
    }
}

fn array_insert_split<T: Default>(
    mut arr: impl BufView<ElemType = T>,
    mut new_arr: impl BufView<ElemType = T>,
    arr_len: usize,
    index: usize,
    midpoint: usize,
    x: T,
) {
    assert!(index <= arr_len);
    if index < midpoint {
        // copy to new array
        for i in midpoint - 1..arr_len {
            new_arr.set(1 + i - midpoint, arr.get(i));
            arr.set(i, T::default());
        }

        // insert
        for i in (index + 1..midpoint).rev() {
            arr.set(i, arr.get(i - 1));
        }
        arr.set(index, x);
    } else {
        // copy to new array the elements before insertion
        for i in midpoint..index {
            new_arr.set(i - midpoint, arr.get(i));
            arr.set(i, T::default());
        }

        new_arr.set(index - midpoint, x);

        // copy after insertion
        for i in index..arr_len {
            new_arr.set(1 + i - midpoint, arr.get(i));
            arr.set(i, T::default());
        }
    }
}

fn array_insert<T>(mut arr: impl BufView<ElemType = T>, arr_len: usize, index: usize, x: T) {
    for i in (index + 1..=arr_len).rev() {
        arr.set(i, arr.get(i - 1));
    }
    arr.set(index, x);
}

fn leaf_insert(leaf: &mut NodeBuf, ind_k: usize, ind_v: usize, key: Key, value: ValueOrAddr) {
    assert!(leaf.is_leaf());
    let nkeys = leaf.get_nkeys();
    array_insert(leaf.key_view(), nkeys, ind_k, key);
    array_insert(leaf.value_view(), nkeys, ind_v, value);
    leaf.set_nkeys(nkeys + 1);
}

fn node_insert(node: &mut NodeBuf, ind_k: usize, ind_v: usize, key: Key, value: ValueOrAddr) {
    assert!(!node.is_leaf());
    let nkeys = node.get_nkeys();
    array_insert(node.key_view(), nkeys, ind_k, key);
    array_insert(node.value_view(), nkeys + 1, ind_v, value);
    node.set_nkeys(nkeys + 1);
}
