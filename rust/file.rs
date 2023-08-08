use std::fs;
use std::io::{self, prelude::*, Cursor, SeekFrom};

use crate::block::{DiskBlock, HeaderBuf, NodeBuf, ValueOrAddr, BLOCK_SIZE, MAX_NKEYS};

fn open_db(path: &str) -> io::Result<fs::File> {
    fs::File::options()
        .read(true)
        .write(true)
        .create(true)
        .open(path)
}

pub struct NodeFile<T = fs::File> {
    file_size: ValueOrAddr,
    file: T,
}

impl<T: Read + Write + Seek> NodeFile<T> {
    pub fn read_node(&mut self, addr: ValueOrAddr) -> NodeBuf {
        NodeBuf::new(self.read_block(addr))
    }

    pub fn write_node(&mut self, node: &NodeBuf) {
        self.write_block(node.block())
    }

    pub fn get_root(&mut self) -> NodeBuf {
        let header = self.get_header();
        let root_ptr = header.get_root_ptr();
        self.read_node(root_ptr)
    }

    pub fn set_root(&mut self, addr: ValueOrAddr) {
        let mut header = self.get_header();
        header.set_root_ptr(addr);
        self.write_block(header.block());
    }

    pub fn new_node(&mut self) -> NodeBuf {
        let mut header = self.get_header();
        let free_ptr = header.get_free_ptr();
        if free_ptr == 0 {
            let addr = self.new_block_addr();
            let block = DiskBlock::new(addr);
            NodeBuf::new(block)
        } else {
            let mut node = self.read_node(free_ptr);
            let next_free = node.get_next_free();
            header.set_free_ptr(next_free);
            self.write_block(&header.block());
            node
        }
    }

    pub fn free_node(&mut self, mut node: NodeBuf) {
        let mut header = self.get_header();
        let free_ptr = header.get_free_ptr();
        header.set_free_ptr(node.get_addr());
        node.into_free_block(free_ptr);
        self.write_block(header.block());
        self.write_node(&node);
    }

    pub fn dump_file(&mut self) {
        let root = self.get_root();
        self.dump_node(&root);
    }

    fn dump_node(&mut self, node: &NodeBuf) {
        let addr = node.get_addr();
        let ntype = if node.is_leaf() { "Leaf" } else { "Node" };
        let nkeys = node.get_nkeys();
        println!("{addr}: {ntype} nkeys={nkeys}");
        print!("keys  :");
        for i in 0..MAX_NKEYS { print!(" {}", node.get_key(i)); }
        println!();
        print!("values:");
        for i in 0..MAX_NKEYS { print!(" {}", node.get_value(i)); }
        println!();

        if !node.is_leaf() {
            for i in 0..=nkeys {
                let child_addr = node.get_value(i);
                let child = self.read_node(child_addr);
                self.dump_node(&child);
            }
        }
    }

    fn read_block(&mut self, addr: ValueOrAddr) -> DiskBlock {
        self.file.seek(SeekFrom::Start(addr)).unwrap();
        let mut new_block = DiskBlock::new(addr);
        let _bytes_read = self.file.read(new_block.buf_bytes_mut()).unwrap();
        new_block
    }

    fn write_block(&mut self, block: &DiskBlock) {
        self.file.seek(SeekFrom::Start(block.addr)).unwrap();
        self.file.write(block.buf_bytes()).unwrap();
    }

    fn new_block_addr(&mut self) -> ValueOrAddr {
        let old_addr = self.file_size;
        self.file_size += u64::try_from(BLOCK_SIZE).unwrap();
        old_addr
    }

    fn get_header(&mut self) -> HeaderBuf {
        let block = self.read_block(0);
        HeaderBuf::new(block)
    }

    fn init_file(&mut self) {
        let _ = self.new_block_addr();
        let root_addr = self.new_block_addr();

        // init header
        let mut header = HeaderBuf::new(DiskBlock::new(0));
        header.set_root_ptr(root_addr);
        header.set_free_ptr(0);
        self.write_block(header.block());

        // init root
        let mut root = self.read_node(root_addr);
        root.set_nkeys(0);
        root.set_is_leaf(true);
        self.write_node(&root);
    }
}

impl NodeFile<fs::File> {
    pub fn open(path: &str) -> Self {
        let file = open_db(path).unwrap();
        let meta = file.metadata().unwrap();
        let mut new_obj = Self {
            file_size: meta.len(),
            file,
        };
        new_obj.init_file();
        new_obj
    }
}

impl NodeFile<Cursor<Vec<u8>>> {
    fn _new_in_memory() -> Self {
        let file = Cursor::new(Vec::new());
        let mut new_obj = Self { file_size: 0, file };
        new_obj.init_file();
        new_obj
    }

    fn _get_bytes(&self, start: usize, end: usize) -> &[u8] {
        self.file.get_ref()[start..end].as_ref()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn foo() {
        let mut file = NodeFile::_new_in_memory();
        file.init_file();
        println!("{:?}", file._get_bytes(0, 32));
    }
}
