---
title: "splinter_event_bus_get_dirty"
parent: "API Reference"
date: 2026-06-30
updated: 2026-08-17
---

## `splinter_event_bus_get_dirty` Splinter API Reference

The purpose of `splinter_event_bus_get_dirty` is to copy a snapshot of the dirty-slot bitmask into caller-supplied storage so a watcher can enumerate only the slots that changed.

### Forward Declaration & Use

`void splinter_event_bus_get_dirty(uint64_t *out, size_t words)` `<splinter.h>`

```
uint64_t dirty[SPLINTER_EVENT_BUS_MASK_WORDS];
splinter_event_bus_get_dirty(dirty, SPLINTER_EVENT_BUS_MASK_WORDS);

size_t stripe = splinter_event_bus_stripe();
/* bit b = (w*64 + i) covers physical slots [b*stripe, (b+1)*stripe) */
```

### Return & Rationale

**Return Behavior:**
This function returns no value (void). `out` must hold at least `words` `uint64_t` values; `words` is capped at `SPLINTER_EVENT_BUS_MASK_WORDS`.

**Errno Behavior:**
*None.*

**Rationale (Or None):**
The mask is a fixed `SPLINTER_EVENT_BUS_BITS` wide (128 bytes) no matter how large the store is, because it is meant to stay cache-resident. Slots are mapped onto it in contiguous stripes of `ceil(slots / SPLINTER_EVENT_BUS_BITS)` slots each, so a set bit means at least one slot in that contiguous range was written since the bus was initialized. Call [splinter_event_bus_stripe](splinter_event_bus_stripe.md) for the width rather than recomputing it. For a store of `SPLINTER_EVENT_BUS_BITS` slots or fewer the stripe is 1 and the mapping is exact — one bit per slot.

Because a stripe is contiguous, rescanning one is a sequential walk over neighbouring slots rather than a scattered hop across the store.

This call does **not** clear the mask, so callers diff the snapshot against their own saved copy to find newly-dirtied stripes. A caller that does not diff will watch the mask saturate to all-ones over the life of the store and stop discriminating. If you are the only consumer, [splinter_event_bus_take_dirty](splinter_event_bus_take_dirty.md) removes that trap by snapshotting and clearing in one atomic step.

### See Also

**Relevant Symbols (Or None):**
[splinter_event_bus_take_dirty](splinter_event_bus_take_dirty.md), [splinter_event_bus_stripe](splinter_event_bus_stripe.md), [splinter_event_bus_wait](splinter_event_bus_wait.md), [splinter_event_bus_init](splinter_event_bus_init.md)
