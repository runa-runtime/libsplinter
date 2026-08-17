---
title: "splinter_event_bus_stripe"
parent: "API Reference"
date: 2026-08-17
updated: 2026-08-17
---

## `splinter_event_bus_stripe` Splinter API Reference

The purpose of `splinter_event_bus_stripe` is to report how many physical slots each dirty-mask bit covers, so a watcher can turn a set bit back into a range of slots to rescan.

### Forward Declaration & Use

`size_t splinter_event_bus_stripe(void)` `<splinter.h>`

```
size_t stripe = splinter_event_bus_stripe();

/* bit b covers physical slots [b*stripe, min((b+1)*stripe, slots)) */
size_t first = b * stripe;
size_t last  = first + stripe;   /* clamp to the store's slot count */
```

### Return & Rationale

**Return Behavior:**
Returns `ceil(slots / SPLINTER_EVENT_BUS_BITS)` for the currently mapped store, or 1 if no store is mapped. Never returns 0. A return of 1 means the mask is exact: bit *b* is slot *b*, with no sharing.

**Errno Behavior:**
*None.*

**Rationale (Or None):**
The dirty mask is a fixed `SPLINTER_EVENT_BUS_BITS` wide regardless of store size, so any store larger than that has to share bits. Slots are divided into contiguous stripes of this width rather than folded modularly, which means a set bit maps to a neighbouring run of slots that can be rescanned as a sequential walk.

Geometry is static — slot count is fixed at `splinter_create` and cannot change on a live store — so the stripe width is computed once when the store is mapped rather than on every write. Reading it here is a plain load, cheap enough to call in a loop.

Prefer this over deriving the value yourself from `splinter_header_snapshot_t.slots`; it keeps the rounding in one place, and rounding the other way silently produces bit indices that fall outside the mask.

### See Also

**Relevant Symbols (Or None):**
[splinter_event_bus_get_dirty](splinter_event_bus_get_dirty.md), [splinter_event_bus_take_dirty](splinter_event_bus_take_dirty.md), [splinter_create](splinter_create.md)
