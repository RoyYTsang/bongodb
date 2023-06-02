
import os

BLOCK_SIZE = 2**12

class DiskBlock:
    __slots__ = ["addr", "buf"]

    def __init__(self, addr):
        self.addr = addr
        self.buf = bytearray(BLOCK_SIZE)

class _LRUNode:
    """next goes toward the front, i.e. the most recently used."""
    __slots__ = ["prev", "next", "block"]
    def __init__(self):
        self.block, self.prev, self.next = None, None, None

class BlockCache:
    """
    To disable the cache, set cache size to 0.
    """
    def __init__(self, f, max_size):
        self.max_size = max_size
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
        elif len(self.cache) < self.max_size:
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
        if self.max_size == 0:
            return self._read_block(addr)
        node = self._access(addr)
        if node.block is None:
            node.block = self._read_block(addr)
        return node.block

    def write(self, block):
        if self.max_size == 0:
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
        iotrace.write()
        self.f.seek(block.addr)
        nbytes = self.f.write(block.buf)
        os.fsync(self.f.fileno())
        assert nbytes == len(block.buf)

    def _read_block(self, addr):
        iotrace.read()
        block = DiskBlock(addr)
        self.f.seek(addr)
        self.f.readinto(block.buf)
        return block

class _IOTrace:
    def __init__(self):
        self._stats = []
        self.scope()
    def scope(self):
        self._stats.append([0, 0])
    def pop(self):
        if len(self._stats) == 1:
            raise ValueError("Can't pop the top level scope")
        return self._stats.pop()
    def get(self):
        self._check_scope()
        return tuple(self._stats[-1])
    def read(self):
        self._check_scope()
        self._stats[-1][0] += 1
    def write(self):
        self._check_scope()
        self._stats[-1][1] += 1
    def _check_scope(self):
        if len(self._stats) == 0:
            raise ValueError("No active scope in IOTrace")

iotrace = _IOTrace()
