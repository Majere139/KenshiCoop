# Replication pitfalls and gate design

> **Purpose.** Hard-won failure modes in KenshiCoop's item/world replication, and
> the testing habits that did or did not catch them. Each entry states a rule, the
> concrete bug that produced it, and the signature to grep for. This is not a
> changelog — per-protocol narrative lives in `resources/PROTOCOL_HISTORY.md`
> (which is untracked, hence this file).
>
> Most entries generalise past items: they are really about drawing conclusions
> from engine reads, and about writing gates that can fail.

---

## 1. Budget in milliseconds, not ticks

`mainLoop_hook` runs every engine tick and Kenshi's loop runs at roughly
**100–125 Hz**. Any budget expressed as "N consecutive reads" is therefore a
tolerance of `N × ~8–10 ms`, which is almost never what the author meant.

`WD_DEAD_READS_MAX = 3` was intended as "let the read agree with itself before
believing it". In a real session it retired a tracked ground item **29 ms after
the drop that created it**, and a second one 32 seconds before the peer actually
picked that item up. The author then had no handle to answer the peer's pickup
with, so its ground copy stayed on the floor next to the item the peer now held —
the duplicate players reported.

**Rule.** Pair every read-count budget with a real-time hold, and log the elapsed
duration in the line that acts on it, so the budget is auditable from a session
log instead of inferred. See `WD_DEAD_HOLD_MS` and the `ground-prune` line, which
now reports `(N consecutive reads over Xms, everLive=1)`.

## 2. Never conclude from a single engine read

The engine streams objects out and back. A cached `RootObject*` goes unreadable
for a frame, `isInInventory` flickers, a spatial query misses. Any of these is a
transient, and code that treats one of them as a verdict makes a permanent
mistake from a momentary one.

Three places did exactly this, all in the pickup path:

- `why=gone` erased a ground track on one unreadable read — the very read that
  `reconcileGroundGear` deliberately refuses to trust on its own.
- `why=untracked` gave up after a single attempt at the site-anchored spatial
  scan. That scan uses `getObjectsWithinSphere`, which this codebase repeatedly
  documents as unreliable **in towns** — so the one-shot fallback was weakest
  exactly where players hit it.
- The drop mirror fabricated an item the moment a top-level search missed (§4).

**Rule.** When an operation cannot be satisfied now, park it and retry against a
deadline rather than answering once. Unsatisfied identified pickups are now
`PendingPickup` entries retried each tick (named track first, site scan second)
until `WD_REHOME_MAX_MS`, then reported as `PICKUP-GAVEUP` rather than vanishing
quietly. Identity-less intents are still refused outright — without an instance
identity, "re-home the oldest same-sid copy" teleports an unrelated object.

## 3. Let the asymmetry of the cost pick the default

Forgetting a track that is still on the ground costs a **permanent duplicate**.
Keeping a track whose object is genuinely gone costs **a stale pointer until the
next read**. Those are not comparable, so the tie should not be broken evenly.

The same asymmetry runs the other way for destruction: the author may only
destroy its ground copy after it has *seen* the item in the target container,
because a wrong guess loses the only instance. Keeping a duplicate beats
destroying the last copy.

**Rule.** Write down which error is recoverable before choosing the threshold.

## 4. Fabrication must prove absence, not failure-to-find

`APPLY-HEALED` mints an item from the drop intent's provenance when the mirror
cannot find its own copy. That is safe only if the item is genuinely absent —
and "my search didn't find it" is a much weaker claim, because **search scope**
is part of the search.

`dropItemFromInventory` reads top level only, on purpose (see the `includeNested`
note in `PROTOCOL_HISTORY.md`: a bagged item belongs to a different `Inventory`
and must not be counted as the character's loose kit). But hoovering
a pile of loot into one character overflows the grid and Kenshi stows the tail in
the worn backpack. The mirror then declared the item missing and fabricated one,
leaving the real item in the bag **and** a minted duplicate on the ground. Four
of these appeared in a single player session, three consecutively.

**Rule.** Before fabricating, exhaust every place the item could legitimately be.
`relocateWeaponToGround` now falls back to `dropItemFromNestedContainer` and logs
`RELOCATE-NESTED` when that reach is what saved the object.

## 5. Engine constraints that shape what a test can even do

Discovered the hard way while trying to build fixtures; each one silently returns
zero rather than failing loudly.

| Constraint | Symptom |
|---|---|
| A character has two weapon slots, and **both its grid and its worn bag refuse a third weapon**. | `[mk] tryAddItem-fail sid='...' type=2` — a scenario trying to mint a burst of weapons adds nothing. Prefer an armour template. |
| A character's loose storage **is** its worn backpack, so a second container cannot be added. | `[mk] tryAddItem-fail ... type=46`. The same-template multi-bag case is unreachable on a character. |
| `getObjectsWithinSphere` is unreliable in towns. | Spatial discovery and spatial recovery both silently miss. Query-free paths (the `dropItem` detour, handle-based liveness) exist for this reason; do not build a sole recovery path on a spatial scan. |
| A town-dropped item often reports its transform as `(0,0,0)` the frame it grounds. | Mirrored copies land at world origin unless the sentinel is filtered. |

## 6. Gate on a conservation ledger, not on presence

"Does the peer have it?" passes on a duplicate, because the peer does have it —
and so does the ground. A count taken only at the end also passes on a
destroy-and-recreate loop.

The invariant that catches both directions at once is a **per-template ledger
evaluated on each side**: every instance dumped must exist exactly once, either
on the ground or in the picker's bag.

```
ground + bag == dumped        # per template, per client
```

A template summing **above** what was dumped is the duplicate; **below** it is the
item that never arrived. `inv_dump_all` prints this as `dist='1+0/1,1+0/1'` so a
failure is readable without opening the logs.

Two traps in measuring it:

- Count nested contents (`includeNested`), or a successful transfer **into a
  backpack** reads as a loss.
- Measure a **delta** against each side's own baseline. Probe templates are
  ordinary kit the receiving character may already carry, and an absolute count
  passes on the save's own contents with nothing having crossed the wire. An
  early version of the nested-bag gate did exactly that (host 7, join 5, PASS).

## 7. "Tolerated but reported" is how a bug survives green runs

`APPLY-HEALED` was deliberately logged-and-allowed, on the reasoning that a heal
means the publish hold was outrun and the backstop covered it. It covered nothing
— it was minting duplicates — and it did so through several passing runs because
no gate would fail on it.

**Rule.** If a signature means the product did something it should not have, fail
on it. If it is genuinely acceptable, say why in the gate, not in a comment. The
oracles now fail on `APPLY-HEALED`, `PICKUP-GAVEUP`, any surviving one-shot
`why=untracked`, and any `ground-prune` that retired a once-live track inside
`WD_DEAD_HOLD_MS`.

## 8. A fault-injection lever must model the *actual* fault

This is the subtlest lesson here, and it cost three A/B attempts.

The goal was a gate that fails before a fix and passes after. Two levers failed
to discriminate:

1. **The plain scenario** passed on both builds — the fault is timing-dependent
   and does not reproduce on demand.
2. **`KENSHICOOP_WD_FORGET_TRACK`** (discard the author's track permanently) also
   passed pre-fix, because a *permanently* lost track is covered by the
   site-anchored recovery, which `inv_regear_forget` already gates. The injected
   fault was **more severe** than the real one, so it exercised a different
   recovery path and said nothing about the bug under test.

What discriminated was modelling the real transient:
`KENSHICOOP_WD_TRANSIENT_DEAD=N` makes the first N pickup-time resolutions report
the object gone and later reads succeed — the engine streaming an object out and
back. Pre-fix, the host ledger reads `2+0/1` (two ground copies of a template
dumped once); post-fix it reads `1+0/1`.

**Rule.** If a lever makes the failure permanent when reality makes it momentary,
a passing A/B proves only that some *other* recovery path works. Match the
severity, not just the shape.

## 9. Log unconditionally for anything a player can see

W1 ground-item diagnostics sit behind `KENSHICOOP_INV_DUMP`, so a real session log
of "I dropped it here and it never appeared there" contained nothing attributable
about the W1 path at all. The W2 path, whose key lines are unconditional, was
diagnosable from the same log in minutes.

**Rule.** A state the player can observe deserves an unconditional line. Keep the
verbose per-item dumps behind the flag; put the verdicts (`DROP-CAP-SKIP`,
`SEND-DEFER`, `PICKUP-GAVEUP`, `APPLY-LOST`) in front of it.

## 10. Gate the workflow, not the unit

Every gear gate was a one-item round trip: drop one thing, pick it up, assert. The
player's actual workflow was to **dump a character's entire inventory and hoover
all of it up with one other character**. That difference is where the bugs lived —
a burst mints many tracks at once, and one receiving grid fills up and overflows
into the backpack. Neither condition existed in any gate, which is why the suite
stayed green across three reported-bug sessions.

**Rule.** Ask how the feature is actually driven, and make at least one gate drive
it that way. `inv_dump_all` exists for this; `inv_dump_all_transient` is its
fault-injected twin (§8).
