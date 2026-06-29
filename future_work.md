# Future work / open problems

This file tracks research directions and problems that are not yet solved.

## Dynamic ring-based reservation for RC24 shared-buffer interleave

### Goal

Apply the `llrice`-style `PagePool` + `RingArray` reservation idea to the RC24
24-bit carryless Subbotin range coder with N=4 interleaved streams sharing a
single contiguous buffer.

In the `llrice` BitStream/Rice coder this works cleanly:

- `PagePool` is a single vector of pages; `acquire_page()` appends for the writer,
  `get_next_read_page()` consumes from the front for the reader.
- Each stream has a small `RingArray` of reserved page indices.
- Writer and reader are symmetric: both start from an empty state, every
  `put_bits` on the encoder has a matching `get/read` on the decoder, and
  reservations happen in the same global order on both sides.

### Why RC24 breaks the symmetry

Subbotin carryless RC24 has an asymmetric init/flush:

- **Encoder flush** writes the final 3 bytes of a stream **after** the main symbol
  loop has finished.
- **Decoder init** must read those same 3 bytes **before** the main symbol loop to
  seed its 24-bit `code`.

So the decoder cannot start at the beginning of the shared buffer and consume
the first 12 reserved slots as init bytes — those would be the *first* renorm
bytes of the streams, not the *last* flush bytes.

### Attempted variants

1. **Naïve global `next_free++` reservation**
   - Encoder and decoder reserve a fresh global slot for every renorm/init/flush
     byte.
   - Fails because `init` and `flush` reserve at different times. The decoder's
     init consumes the early renorm bytes, not the final flush bytes.

2. **Per-stream `RingArray<size_t,8>` with reserve-ahead refill**
   - Each stream keeps a ring of reserved global positions. `reserve()` refills
     from a common allocator; `emit()`/`consume()` take the oldest reserved slot.
   - Fails for the same reason: encoder `flush` writes into slots that were
     reserved during earlier renorms (the tail of the ring), while decoder `init`
     consumes the slots the decoder reserved before the main loop — not the same
     physical slots.

3. **Two-sided buffer (renorm from front, flush from back)**
   - Renorm bytes reserve upward from the front, flush bytes reserve downward from
     the back.
   - Same logical mismatch; decoder `init` still cannot know which back-side slots
     belong to which stream without extra side information.

### Current fallback

`test_rc24_reserve_il.c` currently uses a fixed-slot layout:

```
shared[k * N + stream_id]
```

This is a data-independent reservation scheme: both sides know in advance which
global slot belongs to byte `k` of stream `s`. `init` reads the last three slots
of each stream; `flush` writes them. It proves the shared-buffer concept but does
not realize the dynamic reservation idea.

### Constraints for a clean solution

- Single shared buffer, no per-stream compressed sizes, no file offsets.
- Decoder must locate each stream's flush bytes using only the same symbol order
  and model information the encoder used.
- N=4 interleaved streams.

### Hypothesis to revisit

Pre-reserve a dedicated "flush tail" per stream at the very start, and keep it
at the tail of each stream's reservation queue until the end. The difficulty
is that a plain FIFO ring naturally consumes from the oldest reserved slot, so
"reserve now, consume last" is not expressible. A small per-stream deferred
queue or a two-stage allocator (renorm arena + per-stream flush arena) may be
needed.

### Reference implementation

See `llrice.worktrees/agents-naval-bug/bitstream.hpp`, `ring.hpp`, `pool.hpp`
for the working Rice/BitStream design that this problem tries to port to RC24.
