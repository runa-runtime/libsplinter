# TypeScript Bindings

These have (mostly) evolved by-hand in chunks as I was able to take breaks
from the C api and try to come up with something universal-ish that was also
not "robotic" to use. Generated APIs can be a little ignorant of their own 
inconvenience and Splinter is a unique bird.

## Using

Make sure splinter is built first (the libsplinter.so link needs to resolve).

You can then use Deno / Bun as you normally would with either built-in test
runner against the appropriately named tests.

## PRs Welcome

If you want to add more parts of the C API, I'd be happy to have some of it, 
but I caution against the urge to just implement everything. Splinter's
functionality is best exercised *by using it in different orthogonal  ways* 
and we don't want to suggest workflows beyond what's necessary. In other 
words, there's no "wrong" way to do things with Splinter, outside of the 
documented gotchas. It's supposed to be breadboard.

If you end up going near the eventfd broker, please consider creating an issue
to talk about your work first if you plan to also share it, because you would
probably be invited to create a companion class we ship in the repo. 

The default bindings themselves probably need some more things included, but 
not too much more, at this point.

