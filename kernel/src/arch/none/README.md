# the architecture that does nothing

this is not a port. nothing here will ever run.

it is an **instrument**, and it measures one thing: whether the boundary
drawn in 0.2.20 is real or is decoration. 0.2.20 claimed that everything
above `arch/` is machine-independent. that claim was checked by
`tools/checkarch.py`, which greps -- it can see an `#include` of an x86
header and a line of inline assembly, and it cannot see a portable file
that quietly *depends* on something only x86 provides.

so `make portable-check` compiles the whole portable kernel -- `mm/`,
`sched/`, `fs/`, `lib/`, most of `drivers/` -- against these headers,
which supply the five contracts and nothing else. every function is
empty. every constant is a plausible-looking lie.

if it compiles, nothing above the line needs a machine.
if it does not, the compiler names the leak and the line number.

## and then it says what a port would have to supply

compiling is only half of it. the objects are linked and the *undefined
symbols* collected, and what comes back is a list of every name the
portable kernel reaches for and cannot resolve on its own.

that list is not a diagnostic. it is the porting checklist, derived
mechanically rather than written by somebody trying to remember. an
architecture is finished when it defines everything on it, and that is a
question with an answer rather than a matter of opinion.

## why the values here are wrong on purpose

`cpu_id()` returns 0. `cpu_stack_pointer()` returns 0. `CPU_MAX` is 1.

none of these are attempts to be reasonable. they are the smallest thing
that satisfies the type, because the moment one of them looks like a
*plausible* answer somebody will start relying on it -- and this
directory has to stay useless in order to stay honest. a stub that
returned a believable stack pointer would be a stub that hides the file
which needed a real one.
