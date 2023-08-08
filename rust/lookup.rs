use crate::block::{Key, NodeBuf, ValueOrAddr};
use crate::file::NodeFile;

pub fn lookup(nfile: &mut NodeFile, key: Key) -> Option<ValueOrAddr> {
    let mut n = lookup_until_leaf(nfile, key);
    let (i, success) = lookup_leaf(&mut n, key);
    if success {
        Some(n.get_value(i))
    } else {
        None
    }
}

pub fn lookup_until_leaf(nfile: &mut NodeFile, key: Key) -> NodeBuf {
    let mut n = nfile.get_root();
    while !n.is_leaf() {
        let i = lookup_node(&mut n, key);
        n = nfile.read_node(n.get_value(i));
    }
    n
}

pub fn lookup_leaf(leaf: &NodeBuf, key: Key) -> (usize, bool) {
    let mut left = 0;
    let mut right: isize = isize::try_from(leaf.get_nkeys()).unwrap() - 1;
    while left <= right {
        let mid = (left + right) >> 1;
        let midkey = leaf.get_key(usize::try_from(mid).unwrap());
        if midkey < key {
            left = mid + 1;
        } else if midkey > key {
            right = mid - 1;
        } else {
            return (usize::try_from(mid).unwrap(), true);
        }
    }
    (usize::try_from(left).unwrap(), false)
}

pub fn lookup_node(node: &NodeBuf, key: Key) -> usize {
    let mut left: isize = 0;
    let mut right: isize = isize::try_from(node.get_nkeys()).unwrap() - 1;
    while left <= right {
        let mid = (left + right) >> 1;
        let midkey = node.get_key(usize::try_from(mid).unwrap());
        if midkey < key {
            left = mid + 1;
        } else if midkey > key {
            right = mid - 1;
        } else {
            return usize::try_from(mid + 1).unwrap();
        }
    }
    usize::try_from(left).unwrap()
}
