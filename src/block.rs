use std::slice;

pub type Key = u64;
pub type ValueOrAddr = u64;

pub const BLOCK_SIZE: usize = 4096;
pub const MAX_NKEYS: usize = (BLOCK_SIZE - 16) / 16;
pub const INVALID_NKEYS: usize = 9999;

const OFFSET_KEYS_START_QWORD: usize = 1;
const OFFSET_VALUES_START_QWORD: usize = OFFSET_KEYS_START_QWORD + MAX_NKEYS;
// TODO const OFFSET_VALUES_END_QWORD: usize = OFFSET_VALUES_START_QWORD + MAX_NKEYS + 1;

pub struct DiskBlock {
    pub addr: ValueOrAddr,
    buf_qword: Box<[u64; BLOCK_SIZE / 8]>,
}

impl DiskBlock {
    pub fn new(addr: ValueOrAddr) -> Self {
        Self {
            addr,
            buf_qword: Box::new([0; BLOCK_SIZE / 8]),
        }
    }

    pub fn buf_bytes(&self) -> &[u8] {
        let ptr = self.buf_qword.as_ptr().cast();
        let len = 8 * self.buf_qword.len();
        unsafe { slice::from_raw_parts(ptr, len) }
    }

    pub fn buf_bytes_mut(&mut self) -> &mut [u8] {
        let ptr = self.buf_qword.as_mut_ptr().cast();
        let len = 8 * self.buf_qword.len();
        unsafe { slice::from_raw_parts_mut(ptr, len) }
    }
}

pub struct NodeBuf(DiskBlock);

impl NodeBuf {
    pub fn new(block: DiskBlock) -> Self {
        Self(block)
    }

    pub fn block(&self) -> &DiskBlock {
        &self.0
    }

    pub fn get_addr(&self) -> ValueOrAddr {
        self.block().addr
    }

    pub fn get_key(&self, i: usize) -> Key {
        assert!(i < MAX_NKEYS);
        let index = i + OFFSET_KEYS_START_QWORD;
        self.0.buf_qword[index]
    }

    pub fn set_key(&mut self, i: usize, key: Key) {
        assert!(i < MAX_NKEYS);
        let index = i + OFFSET_KEYS_START_QWORD;
        self.0.buf_qword[index] = key;
    }

    pub fn get_value(&self, i: usize) -> ValueOrAddr {
        assert!(i <= MAX_NKEYS);
        let index = i + OFFSET_VALUES_START_QWORD;
        self.0.buf_qword[index]
    }

    pub fn set_value(&mut self, i: usize, value: ValueOrAddr) {
        assert!(i <= MAX_NKEYS);
        let index = i + OFFSET_VALUES_START_QWORD;
        self.0.buf_qword[index] = value;
    }

    pub fn key_view(&mut self) -> KeyView {
        KeyView(self)
    }

    pub fn value_view(&mut self) -> ValueView {
        ValueView(self)
    }

    pub fn get_nkeys(&self) -> usize {
        let block_header = self.0.buf_qword[0];
        usize::try_from(block_header & 0xFFFF).unwrap()
    }

    pub fn set_nkeys(&mut self, nkeys: usize) {
        assert!(nkeys < usize::from(u16::MAX));
        let block_header = self.0.buf_qword[0];
        let cleared = block_header & !0xFFFF;
        let new_header = cleared | u64::try_from(nkeys).unwrap();
        self.0.buf_qword[0] = new_header;
    }

    pub fn is_leaf(&self) -> bool {
        let block_header: u64 = self.0.buf_qword[0];
        let bits = block_header & 0xFFFF0000;
        bits != 0
    }

    pub fn set_is_leaf(&mut self, is_leaf: bool) {
        let block_header = self.0.buf_qword[0];
        let cleared = block_header & !0xFFFF0000;
        let new_bits = u64::from(is_leaf) << 16;
        self.0.buf_qword[0] = cleared | new_bits;
    }

    pub fn into_free_block(&mut self, next_free: ValueOrAddr) {
        self.set_nkeys(INVALID_NKEYS);
        self.set_value(0, next_free);
    }

    pub fn get_next_free(&mut self) -> ValueOrAddr {
        self.get_value(0)
    }
}

pub trait BufView {
    type ElemType;
    fn get(&self, i: usize) -> Self::ElemType;
    fn set(&mut self, i: usize, elem: Self::ElemType);
}

pub struct KeyView<'a>(&'a mut NodeBuf);

impl<'a> BufView for KeyView<'a> {
    type ElemType = Key;
    fn get(&self, i: usize) -> Self::ElemType {
        self.0.get_key(i)
    }
    fn set(&mut self, i: usize, elem: Self::ElemType) {
        self.0.set_key(i, elem)
    }
}

pub struct ValueView<'a>(&'a mut NodeBuf);

impl<'a> BufView for ValueView<'a> {
    type ElemType = ValueOrAddr;
    fn get(&self, i: usize) -> Self::ElemType {
        self.0.get_value(i)
    }
    fn set(&mut self, i: usize, elem: Self::ElemType) {
        self.0.set_value(i, elem)
    }
}

pub struct HeaderBuf(DiskBlock);

impl HeaderBuf {
    pub fn new(block: DiskBlock) -> Self {
        Self(block)
    }

    pub fn block(&self) -> &DiskBlock {
        &self.0
    }

    pub fn get_root_ptr(&self) -> ValueOrAddr {
        self.0.buf_qword[0]
    }

    pub fn set_root_ptr(&mut self, v: ValueOrAddr) {
        self.0.buf_qword[0] = v;
    }

    pub fn get_free_ptr(&self) -> ValueOrAddr {
        self.0.buf_qword[1]
    }

    pub fn set_free_ptr(&mut self, v: ValueOrAddr) {
        self.0.buf_qword[1] = v;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn new_nodebuf() -> NodeBuf {
        let buf = Box::new([0; BLOCK_SIZE / 8]);
        let block = DiskBlock {
            addr: 0,
            buf_qword: buf,
        };
        NodeBuf::new(block)
    }

    #[test]
    fn nkeys() {
        let mut node = new_nodebuf();
        assert_eq!(node.get_nkeys(), 0);
        node.set_nkeys(42);
        assert_eq!(node.get_nkeys(), 42)
    }

    #[test]
    fn is_leaf() {
        let mut node = new_nodebuf();
        assert!(!node.is_leaf());
        node.set_is_leaf(true);
        assert!(node.is_leaf());
        node.set_is_leaf(false);
        assert!(!node.is_leaf());
    }

    #[test]
    fn keys_values() {
        let mut node = new_nodebuf();
        assert_eq!(node.get_key(0), 0);
        node.set_key(0, 10);
        assert_eq!(node.get_key(0), 10);
        node.set_key(MAX_NKEYS - 1, 20);
        assert_eq!(node.get_key(MAX_NKEYS - 1), 20, "test last key");
        assert_eq!(node.get_value(0), 0, "test first value");
        node.set_value(0, 30);
        assert_eq!(node.get_value(0), 30);
        assert_eq!(
            node.get_key(MAX_NKEYS - 1),
            20,
            "test last key again. make sure it wasn't affected."
        );
        node.set_value(MAX_NKEYS, 40);
        assert_eq!(node.get_value(MAX_NKEYS), 40, "test last value");
    }

    #[test]
    fn nkeys_is_leaf_combined() {
        let mut node = new_nodebuf();
        node.set_is_leaf(true);
        node.set_nkeys(10);
        node.set_is_leaf(false);
        node.set_key(0, 50);

        assert!(!node.is_leaf());
        assert_eq!(node.get_nkeys(), 10);
        assert_eq!(node.get_key(0), 50);

        node.set_nkeys(20);
        node.set_is_leaf(true);

        assert!(node.is_leaf());
        assert_eq!(node.get_nkeys(), 20);
        assert_eq!(node.get_key(0), 50);
    }

    #[test]
    fn block_bytes() {
        let mut block = DiskBlock::new(0);
        // currently, the test only works for little endian.
        let little_endian: Vec<u8> = vec![81, 23, 12, 0, 0, 0, 0, 0];
        let le_qwords: Vec<u64> = little_endian
            .clone()
            .into_iter()
            .map(|n| u64::from(n))
            .collect();
        block.buf_qword[0] = le_qwords[0] + le_qwords[1] * 256 + le_qwords[2] * 256 * 256;
        let byte_vec: Vec<u8> = block.buf_bytes().to_owned().into_iter().take(8).collect();
        assert_eq!(byte_vec, little_endian);
    }
}
