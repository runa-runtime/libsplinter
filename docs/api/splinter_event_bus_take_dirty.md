---
title: "splinter_event_bus_take_dirty"
parent: "API Reference"
date: 2026-08-17
updated: 2026-08-17
---

## `splinter_event_bus_take_dirty` Splinter API Reference

The purpose of `splinter_event_bus_take_dirty` is to snapshot the dirty-slot bitmask and clear it in the same atomic step, so a single consumer sees every set bit exactly once and the mask can never saturate.

### Forward Declaration & Use

`void splinter_event_bus_take_dirty(uint64_t *out, size_t words)` `<splinter.h>`

```
int fd = splinter_event_bus_open();
uint64_t dirty[SPLINTER_EVENT_BUS_MASK_WORDS];
size_t stripe = splinter_event_bus_stripe();

while (splinter_event_bus_wait(fd, 1000) == 0) {
    splinter_event_bus_take_dirty(dirty, SPLINTER_EVENT_BUS_MASK_WORDS);
    /* every set bit here is new since the previous take */
    for (size_t b = 0; b < SPLINTER_EVENT_BUS_BITS; b++)
        if (dirty[b / 64] & (1ULL << (b % 64)))
            rescan_slots(b * stripe, stripe);
}
```

### Return & Rationale

**Return Behavior:**
This function returns no value (void). `out` must hold at least `words` `uint64_t` values; `words` is capped at `SPLINTER_EVENT_BUS_MASK_WORDS`. Bit geometry is identical to [splinter_event_bus_get_dirty](splinter_event_bus_get_dirty.md).

**Errno Behavior:**
*None.*

**Rationale (Or None):**
The mask is OR-only. A consumer using `splinter_event_bus_get_dirty` must diff each snapshot against its own saved copy, and one that forgets will watch the mask fill to all-ones and quietly stop telling it anything. Reading and clearing in one step removes that failure mode: every bit is reported once, and the mask always reflects only what has happened since the last take.

Clearing is done per word with an atomic read-and-clear rather than across the whole mask at once. That is deliberate and safe — a bit set between two words is simply reported on the next take, never lost.

**This call is destructive to other watchers.** Bits taken here are gone for everyone attached to the store, so a second watcher can miss changes you consumed. It is classified **MEDIUM** risk for that reason, not LOW. With two or more independent readers, use `splinter_event_bus_get_dirty` and have each reader diff its own copy.

### See Also

**Relevant Symbols (Or None):**
[splinter_event_bus_get_dirty](splinter_event_bus_get_dirty.md), [splinter_event_bus_stripe](splinter_event_bus_stripe.md), [splinter_event_bus_wait](splinter_event_bus_wait.md), [splinter_event_bus_init](splinter_event_bus_init.md)
