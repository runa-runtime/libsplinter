---
title: "splinter_event_bus_wait"
parent: "API Reference"
date: 2026-06-30
updated: 2026-08-17
---

## `splinter_event_bus_wait` Splinter API Reference

The purpose of `splinter_event_bus_wait` is to block until the global epoch changes or a timeout expires, using `poll(2)` + `read(2)` on the fd from `splinter_event_bus_open`.

### Forward Declaration & Use

`int splinter_event_bus_wait(int fd, uint64_t timeout_ms)` `<splinter.h>`

```
int fd = splinter_event_bus_open();
if (splinter_event_bus_wait(fd, 1000) == 0) {
    uint64_t dirty[SPLINTER_EVENT_BUS_MASK_WORDS];
    splinter_event_bus_get_dirty(dirty, SPLINTER_EVENT_BUS_MASK_WORDS);
}
```

### Return & Rationale

**Return Behavior:**
Returns 0 if a change was detected, or -1 on timeout or error. `timeout_ms` of 0 is non-blocking and `UINT64_MAX` waits forever. On return the eventfd counter has been drained.

**Errno Behavior:**
*None.*

**Rationale (Or None):**
After a successful wait the caller should call `splinter_event_bus_get_dirty` (or `splinter_event_bus_take_dirty`) to find which slot stripes changed, scanning only what moved instead of sweeping the whole store. Prefer this over a busy spin whenever you can afford to block.

The eventfd is created non-blocking, and `O_NONBLOCK` is a status flag on the open file description shared by every fd handed out for a given bus. So if two waiters `poll` concurrently and one drains the counter first, the loser's `read` fails with `EAGAIN` and this returns -1 instead of blocking indefinitely. Treat -1 as "nothing for me right now" and re-arm; with multiple waiters it is an expected outcome rather than an error.

### See Also

**Relevant Symbols (Or None):**
[splinter_event_bus_open](splinter_event_bus_open.md), [splinter_event_bus_get_dirty](splinter_event_bus_get_dirty.md), [splinter_event_bus_take_dirty](splinter_event_bus_take_dirty.md), [splinter_poll](splinter_poll.md)
