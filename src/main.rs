use bongodb::btree::BTree;

fn gen_shuffled(n: usize) -> Vec<u64> {
    let mut v: Vec<usize> = (1..=n).into_iter().collect();
    for i in 0..n {
        // swap with a random index
        let j = (i+1).wrapping_mul(i+4).wrapping_mul(i);
        v.swap(i, j % n);
    }
    v
        .into_iter()
        .map(|x| u64::try_from(x).unwrap())
        .collect()
}

fn main() {
    let mut bt = BTree::new("rustbtree.index");
    for i in gen_shuffled(100_000).into_iter() {
        bt.insert(i, i * 10);
    }
    // bt.dump();
    println!("{:?}", bt.get(24));
}
