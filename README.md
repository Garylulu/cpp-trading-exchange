# cpp-trading-exchange

A small trading exchange written in C++26, built step by step in the Build Fellowship
workshop *C++ Systems for Finance: Build a Trading Exchange from Scratch*.

What it has so far:

- two **memory pools** that hand out pre-allocated objects without touching the heap,
- an **order** type and two **order books** (a priority queue version and a tick array version),
- a multithreaded **exchange**: client threads send orders to a single matching engine thread,
- a **market data feed** that lets a client joining mid-day rebuild its own copy of the book.

There is no order matching yet: orders rest in the book until they are cancelled.

## Quick start

### 1. Install the tools

You need **Bazel** (the build system) and a compiler with C++26 support. The project has been
built and tested with **GCC 15.2** on Ubuntu 26.04. The code uses newer features such as
`= delete("reason")`, so older compilers will not build it.

The repo pins its Bazel version in [`.bazelversion`](.bazelversion) (currently 9.1.1). Install
**Bazelisk**, the official Bazel launcher: it reads that file and downloads the right Bazel for you.

**macOS (Homebrew)**

```bash
# Install Homebrew first if you don't have it: https://brew.sh
brew install bazelisk     # provides the `bazel` command
```

If Apple's built-in clang is too old for C++26, install a newer compiler with `brew install llvm`
or `brew install gcc`.

**Linux**

Either use Homebrew on Linux (https://brew.sh, then `brew install bazelisk`), or download the
Bazelisk binary directly. The direct download needs no `sudo`:

```bash
mkdir -p ~/.local/bin
curl -fsSL -o ~/.local/bin/bazel \
  https://github.com/bazelbuild/bazelisk/releases/download/v1.25.0/bazelisk-linux-amd64
chmod +x ~/.local/bin/bazel   # make sure ~/.local/bin is on your PATH
```

Also install a C++ compiler, e.g. `sudo apt install g++` on Ubuntu (GCC 15 or newer).

Check that it works:

```bash
bazel version   # the first run downloads Bazel 9.1.1
```

### 2. Build, test, and run

```bash
git clone <this repo> && cd cpp-trading-exchange-starter

bazel build //...                            # build everything
bazel test //...                             # run all tests
bazel run //exchange:exchange_main           # run the exchange; stop it with Ctrl+C
bazel run //exchange:exchange_main -- 5      # or run it for 5 seconds
```

See [BAZEL_BUILD.md](BAZEL_BUILD.md) for every target and build configuration
(`--config=opt`, `--config=dbg`, `--config=asan`).

## Project layout

```
common/
  mem_pool.h            memory pool, version 1 (flags stored with each object)
  mem_pool_o.h          memory pool, version 2, optimized (free list)
  order.h               a single order
  order_book.h          order book, version 1 (priority queues)
  order_book_ta.h       order book, version 2 (tick array + linked lists)
  locked_queue.h        thread-safe queue (mutex + condition variable)
  perf_utils.h          RDTSC cycle counter for latency measurement
  time_utils.h          current time as a string
  tests/                unit tests for the common utilities
exchange/
  matcher/matching_engine.h          engine thread that owns the order book
  market_data/market_data.h          feed messages, snapshots, and the publisher
  market_data/market_data_client.h   client that rebuilds the book from market data
  exchange_main.cpp                  the exchange program
  tests/market_data_test.cpp         market data tests, run against both order books
examples/               standalone demos (container benchmark, threads)
```

## Components

### Memory pools: `mem_pool.h` and `mem_pool_o.h`

A memory pool allocates all of its memory once, up front. `allocate(args...)` builds an object in
a free block and returns a pointer to it; `deallocate(ptr)` clears the object and marks the block
free again. Nothing is allocated from the heap after the constructor, which keeps latency
predictable. When the pool is full, `allocate` prints an error and returns `nullptr`.

Both versions have the same interface (`MemPool<T>`), so they can be swapped by changing one
`#include`.

| | `mem_pool.h` | `mem_pool_o.h` (optimized) |
|---|---|---|
| Storage | One vector of `{object, is_free}` blocks | A vector of objects, plus a separate linked list of free blocks |
| Finding a free block | Scan from the start for the first free flag: O(n) | Take the head of the free list: O(1) |
| Freeing a block | Set its flag | Push it onto the head of the free list: O(1) |
| Reuse order | Lowest free index first | Most recently freed first (likely still in cache) |

The free list in `mem_pool_o.h` is stored as **indices in a pre-allocated array**, not as a
`std::list`, since `std::list` would allocate on every insert. The same array also marks blocks
in use, so `deallocate` can catch a double free (checked by `assert` in debug builds).

Both pools keep a live, default-constructed `T` in every free block, so **`T` must have a default
constructor**.

### Orders: `order.h`

`Common::Order` holds one order: ticker, size, side (`BUY`/`SELL`), type (`LIMIT`/`MARKET`),
price, client id, and order id. Prices are stored as **whole cents** (`$100.00` is `10000`) so
they compare exactly, with no floating-point rounding. `toString()` prints e.g.

```
Order[id:567890 client:1234 AAPL BUY 100 LIMIT @ $100.00]
```

### Order books: `order_book.h` and `order_book_ta.h`

Both books hold the resting orders for one ticker in **price-time priority**: best price first,
and among equal prices the oldest order first. Both use `MemPool` from `mem_pool_o.h` for order
storage and share one interface:

- `add(order)` adds a limit order; it returns `false` for the wrong ticker, a market order (it
  can't rest without matching), zero size, a duplicate order id, or a full pool.
- `cancel(order_id)` removes an order; it returns `false` if no such order is in the book.
- `orders()` returns every order in priority order (used for market data snapshots).
- `toString()` prints the asks, then the bids, each best price first.

| | `order_book.h` (priority queue) | `order_book_ta.h` (tick array) |
|---|---|---|
| Structure | One `std::priority_queue` for bids, one for asks | One slot per price tick on each side; each slot holds a FIFO doubly linked list of orders |
| Construction | `OrderBook(ticker, capacity)` | `OrderBook(ticker, capacity, min_price, max_price, tick_size)` |
| `add` | O(log n) | O(1) |
| `cancel` | Lazy: the order is marked dead and only removed (and its memory freed) when it reaches the top of its queue | O(1): unlinked and freed immediately |
| Prices | Any price | Must lie in the range and on the tick grid |
| Memory | Grows with the number of orders | One slot per tick per side, used or not |

**Choosing between them:** the tick array is faster when prices stay within a known, reasonably
narrow range. The priority queue is the better fit when prices can be anywhere, or when a tick
array would need too many slots.

### Thread-safe queue: `locked_queue.h`

`Common::LockedQueue<T>` lets threads hand work to each other. A mutex protects the queue, and a
condition variable lets a consumer sleep until an item arrives. `pop(stop_token)` returns early
when a `std::jthread` is asked to stop, so threads shut down cleanly.

### The exchange: `matching_engine.h` and `exchange_main.cpp`

```
 client threads              inbound queue           engine thread                   market data
┌──────────────┐                                   ┌──────────────────┐
│ trader 1..3  │──add/cancel──▶ LockedQueue ─────▶│  MatchingEngine  │──ADD/CANCEL──▶ each subscriber's
└──────────────┘                     ▲             │  owns the book   │                feed queue
┌──────────────┐                     │             │  (no lock needed)│
│ md client    │──snapshot request───┘             └──────────────────┘──snapshot───▶ client's reply queue
└──────────────┘
```

- **`MatchingEngine<Book>`** is the only thread that touches the order book, so the book needs
  no lock. Clients send it requests (`add`, `cancel`, `requestSnapshot`) through a locked inbound
  queue. It works with either order book.
- **`exchange_main.cpp`** builds the engine, starts three trading client threads and one market
  data client thread, and runs until **Ctrl+C** (or for the number of seconds given). Traders
  keep up to five orders each, adding a new order or cancelling their oldest every 50 ms.
  Order ids come from a shared `std::atomic` counter so they are unique across threads.
- The book type is chosen at the top of `exchange_main.cpp`: it uses the tick array book by
  default; switch the `#include` and the `BOOK_ARGS` line to use the priority queue book.

### Market data: `market_data.h` and `market_data_client.h`

The exchange publishes every change to the book as a numbered update on the **incremental feed**:

```
#7 ADD Order[id:3 client:1 AAPL BUY 100 LIMIT @ $99.50]
#8 CANCEL id:3
```

It also serves **snapshots**: every order in the book, stamped with the number of the last update
it includes. A client joining mid-day catches up in four steps (`MarketDataClient`):

1. **Subscribe** to the feed and start buffering updates.
2. **Request a snapshot** (taken at update number S) and load it as the initial book.
3. **Replay** the buffered updates, discarding any numbered S or lower, since the snapshot already
   includes them.
4. **Go live**: apply each new update as it arrives.

The client rebuilds its book with the same `OrderBook` class as the exchange, so applying the same
changes gives the same book. It skips updates it has already seen and reports any gap in the
numbering.

## Test results

Tested on Ubuntu 26.04 (WSL2, x86-64) with GCC 15.2 and Bazel 9.1.1.

| Check | Result |
|---|---|
| `bazel build //...` | Builds cleanly with `-Wall -Wextra -Werror -Wpedantic` |
| `bazel test //...` | 4 of 4 test targets pass |
| `market_data_test` and `market_data_test_ta` | 6 of 6 tests pass against **each** order book |
| `bazel test --config=asan //exchange/tests/...` | Passes under AddressSanitizer |
| `exchange_main`, built with ThreadSanitizer | No data races reported |
| `exchange_main` (timed runs with each order book, and a Ctrl+C run) | The mid-day client's book matches the exchange's book exactly at shutdown |
| `common/tests/mp_test_1.cpp` (not a Bazel target) | Passes with both memory pools |

A typical run of the exchange:

```
Exchange running with 3 traders. Press Ctrl+C to stop.
[md client] snapshot at #72 with 12 orders, 12 updates buffered
[md client] live at #72
...
Market data client is at #183 and its book matches the exchange's.
```

### Performance

Measured with a quick throwaway benchmark (not in the repo), compiled with `-O2`; the numbers are
the middle of three runs. Treat them as rough comparisons on one machine.

| Workload | Version 1 | Version 2 |
|---|---|---|
| Memory pool: fill and empty a 20,000-block pool, 5 times | `mem_pool.h`: ~2,160 ns per allocate+deallocate | `mem_pool_o.h`: ~2.2 ns |
| Order book: add 100,000 orders over 400 price levels | `order_book.h`: ~49 ns per add | `order_book_ta.h`: ~32 ns per add |
| Order book: cancel all 100,000 in random order | `order_book.h`: ~211 ns per cancel | `order_book_ta.h`: ~88 ns per cancel |

### Observations

- **The free list is the biggest win.** `mem_pool.h` scans for a free block, so filling a large
  pool gets slower as it fills (O(n²) in total). `mem_pool_o.h` takes the head of its free list
  in constant time and was about 1,000 times faster on this workload.
- **The tick array book beat the priority queue book** on both adds and cancels: about 1.5 times
  faster on adds and 2.4 times on cancels. Cancels gain the most, because the priority queue
  can only remove its top element.
- **Lazy cancel has a side effect.** In `order_book.h`, a cancelled order that has not reached the
  top of its queue still holds its pool block. With heavy cancelling, the pool can report full
  while the book has room. `order_book_ta.h` frees the block immediately.
- **The snapshot recovery works as designed.** In the demo, the market data client received 12
  feed updates before its snapshot arrived; it discarded the ones the snapshot already included
  and ended with a book identical to the exchange's.
- **One known limitation.** Both memory pools require a default-constructible `T`, so
  `common/tests/mp_test_2.cpp` (whose `Person` has no default constructor) does not compile
  against them.
