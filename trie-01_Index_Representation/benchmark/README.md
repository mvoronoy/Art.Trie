# Overview

Benchmark test for a fair comparison of two containers representing a set of outgoing Trie edges over an alphabet of 128 symbols (0..127):
- Bitmap-indexed container: several 64-bit words indicate which symbols exist, with a compact array storing only present values. POPCNT/std::popcount computes the index of a child.
- Ordered std::array: stores (symbol, value) pairs sorted by symbol, using binary search for lookup and ordered insertion/removal.
The benchmark measures insert, delete, and lookup separately, at multiple occupancies. Include warm-up, repeated timed batches, identical operation traces, and a checksum so the compiler cannot eliminate the work.

# How to compile

For my purposes I have used `clang++` v18.1.8 as following:
```bash
    clang++  -DNDEBUG -std=c++20 -O3 -m64 -march=native -flto ./bix.cpp
```
Where:

- -O3: Enables aggressive optimizations for maximum execution speed. It turns on vectorization, loop unrolling, and inline functions, which can increase binary size but yields the fastest code.

-  -m64: Forces Clang to target a 64-bit architecture, ensuring the binary uses 64-bit registers and addressing.

- -march=native: Tells the compiler to auto-detect and optimize specifically for the local CPU's architecture.

- -flto: Enables Link-Time Optimization. It optimizes across different source files during the linking phase rather than just compiling each file individually, removing redundant code and improving inlining.

You can try to compile benchmark with your preferred compiler that supporting C++20 standard. 

# Results

## Hardware:
Processor:  13th Gen Intel(R) Core(TM) i7-13800H

memory: 15GiB

## Output:

```
Lookup queries/sample = 1000
Median nanoseconds per operation
occ | bitmap ins | vector ins | bitmap del | vector del | bitmap find | vector find
-----+------------+------------+------------+-------------+-------------+------------
  32 |      392.18|    2252.16|     352.65|    1486.29|      20.63|     140.80|
  64 |     1533.66|    9036.38|    1484.93|    7135.33|      32.62|     283.58|
 128 |     6402.82|  141895.64|    6229.47|  101883.60|      97.61|    1413.47|
 256 |    28255.23|  640219.42|   26834.65|  469980.42|     110.10|    1560.39|
```
