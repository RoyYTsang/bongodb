use crate::block::{Key, ValueOrAddr};
use crate::file::NodeFile;
use crate::insert::{insert, InsertResult};
use crate::lookup::lookup;

pub struct BTree {
    node_file: NodeFile,
}

impl BTree {
    pub fn new(path: &str) -> Self {
        Self {
            node_file: NodeFile::open(path),
        }
    }

    pub fn get(&mut self, key: Key) -> Option<ValueOrAddr> {
        lookup(&mut self.node_file, key)
    }

    pub fn insert(&mut self, key: Key, value: ValueOrAddr) {
        match insert(&mut self.node_file, key, value, false) {
            InsertResult::NewRoot(new_root_addr) => {
                self.node_file.set_root(new_root_addr);
            }
            InsertResult::SameRoot => (),
            InsertResult::DuplicateKey => (),
        }
    }

    pub fn dump(&mut self) {
        self.node_file.dump_file();
    }
}
