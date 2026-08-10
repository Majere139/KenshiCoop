# KenshiLib deps — required pin and patches

`third_party/KenshiLib_deps/` is fetched, not committed (see the root README), so
nothing here is enforced automatically. A clean clone of
[KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
at its default branch **will not build** — you need the pin and the two patches
below.

## Pin

```bash
git -C third_party/KenshiLib_deps checkout e75769b
```

`e75769b` (2026-05-02, "KenshiLib 0.3.0") is the only revision that satisfies
everything `src/plugin/game/EngineInternal.h` includes:

| deps revision | date | `CameraClass.h` | `ZoneManager.h` | flat `CombatClass.h` |
|---|---|---|---|---|
| `a63131d` and earlier | ≤2026-03-26 | ✗ | ✗ | ✓ |
| **`960a7c0` / `e75769b`** | 2026-05-02 | ✓ | ✓ | ✓ |
| `b566d74` (0.4.0) | 2026-06-24 | ✓ | ✓ | ✗ moved to `kenshi/combat/` |

Newer is not better here: 0.4.0 moved `CombatClass.h` and `CombatTechniqueData.h`
into `kenshi/combat/` **without updating their own quoted includes**, so
`combat/CombatClass.h` does `#include "Enums.h"` and resolves against the wrong
directory. Building on 0.4.0 additionally needs the include path widened and the
`EngineInternal.h` include rewritten; pinning to 0.3.0 needs neither.

## Patches

Apply from the repo root after checking out the pin:

```bash
git -C third_party/KenshiLib_deps apply ../kenshilib_patches/0001-dedupe-BuildingDesignation.patch
git -C third_party/KenshiLib_deps apply ../kenshilib_patches/0002-complete-CraftingItem.patch
```

Both fix defects in KenshiLib's own headers, not in anything this repo controls.
They are present in the canonical
[KenshiReclaimer/KenshiLib](https://github.com/KenshiReclaimer/KenshiLib) source
too, so they are upstream bugs rather than mirror artifacts.

**0001 — `BuildingDesignation` defined twice.** `kenshi/Platoon.h` and
`kenshi/Building/Building.h` each define the enum independently (Platoon.h's copy
is even marked `// TODO move?`), and neither includes the other, so `#pragma once`
cannot help. `EngineInternal.h` needs both headers, which is an unavoidable C2011.
The two definitions are byte-identical, so the patch just wraps each in a shared
`KENSHILIB_BUILDINGDESIGNATION_DEFINED` guard — order-independent, no semantic
change.

**0002 — `std::deque<CraftingItem>` over an incomplete type.**
`kenshi/Building/CraftingBuilding.h` forward-declares `class CraftingItem` and
then declares a by-value `std::deque<CraftingItem>` member. The VC10 STL
instantiates `deque` at the member declaration and needs a complete type; the
class is never defined anywhere in KenshiLib. The patch gives it an opaque
definition. This cannot shift any mirrored member offset: MSVC's `deque` holds a
map pointer plus three `size_t` and never embeds `T`, so `sizeof(deque<T>)` is the
same for every `T`. Nothing in this repo reads the deque's elements.

## Why this isn't upstream's problem to have noticed

`third_party/KenshiLib_deps/` is gitignored, so a working tree that predates the
breakage keeps building indefinitely while a fresh clone cannot. Upstream v0.49
(2026-08-05) still uses the flat `kenshi/CombatClass.h` path that only exists at
or before 0.3.0, which is consistent with a long-lived local checkout.
