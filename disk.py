
import os
import time
from dataclasses import dataclass, field

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
        iotrace.start_io()
        self.f.seek(block.addr)
        nbytes = self.f.write(block.buf)
        # os.fsync(self.f.fileno())
        iotrace.end_write()
        assert nbytes == len(block.buf)

    def _read_block(self, addr):
        iotrace.start_io()
        block = DiskBlock(addr)
        self.f.seek(addr)
        self.f.readinto(block.buf)
        iotrace.end_read()
        return block


@dataclass(slots=True)
class IOTraceStats:
    num_reads: int = 0
    num_writes: int = 0
    read_time: float = 0
    write_time: float = 0
    read_sumsqr: float = 0
    write_sumsqr: float = 0
    reads: list = field(default_factory=list)
    writes: list = field(default_factory=list)

class _IOTrace:
    def __init__(self):
        self._stats: list[IOTraceStats] = []
        self._start = None
        self.scope()

    def scope(self):
        self._stats.append(IOTraceStats())

    def pop(self):
        if len(self._stats) == 1:
            raise ValueError("Can't pop the top level scope")
        return self._stats.pop()

    def get(self):
        num_reads, num_writes = self.get_num()
        avg_read, avg_write = self.get_avg()
        stdev_read, stdev_write = self.get_stdev()
        return num_reads, avg_read, stdev_read, num_writes, avg_write, stdev_write

    def get_num(self):
        self._check_scope()
        stat = self._stats[-1]
        return stat.num_reads, stat.num_writes

    def get_avg(self):
        self._check_scope()
        stat = self._stats[-1]

        if stat.num_reads == 0:
            avg_read = 0
        else:
            avg_read = stat.read_time / stat.num_reads

        if stat.num_writes == 0:
            avg_write = 0
        else:
            avg_write = stat.write_time / stat.num_writes

        return avg_read, avg_write

    def get_stdev(self):
        self._check_scope()
        stat = self._stats[-1]

        if stat.num_reads == 0:
            stdev_read = 0
        else:
            stdev_read = _stdev_from_sums(stat.num_reads, stat.read_time, stat.read_sumsqr)

        if stat.num_writes == 0:
            stdev_write = 0
        else:
            stdev_write = _stdev_from_sums(stat.num_writes, stat.write_time, stat.write_sumsqr)

        return stdev_read, stdev_write

    def start_io(self):
        self._check_scope()
        assert self._start is None
        self._start = time.perf_counter()

    def _end_io(self):
        self._check_scope()
        assert self._start is not None
        t = time.perf_counter() - self._start
        self._start = None
        return t

    def end_read(self):
        t = self._end_io()
        stat = self._stats[-1]
        stat.num_reads += 1
        stat.read_time += t
        stat.read_sumsqr += t * t
        stat.reads.append(t)

    def end_write(self):
        t = self._end_io()
        stat = self._stats[-1]
        stat.num_writes += 1
        stat.write_time += t
        stat.write_sumsqr += t * t
        stat.writes.append(t)

    def get_data(self):
        self._check_scope()
        stat = self._stats[-1]
        return stat.reads, stat.writes

    def _check_scope(self):
        if len(self._stats) == 0:
            raise ValueError("No active scope in IOTrace")

def _stdev_from_sums(num_points, sum_points, sum_sqrs):
    avg = sum_points / num_points
    variance = avg**2 + (sum_sqrs - 2*avg*sum_points) / num_points
    return variance**0.5

iotrace = _IOTrace()
