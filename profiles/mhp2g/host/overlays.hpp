#pragma once

#include "tutikumo_profile.hpp"

#include "psprecomp/runtime.hpp"

namespace tutikumo {

// Loads the overlay libraries and installs the dispatch-miss hook that
// recognises the overlay currently in a slot and registers its recompiled
// corpus. The libraries are read from TUTIKUMO_OVERLAY_DIR, or from overlays/
// next to the executable. With TUTIKUMO_DUMP_OVERLAYS set, an unknown overlay is
// written out instead so it can be recompiled.
void install_overlay_support(psprecomp::Runtime &runtime);

// Drops the corpus of any slot whose contents no longer match it. The guest
// flushes the instruction cache right after loading an overlay, which is when
// this is called; the next jump into the slot then installs the right corpus.
void revalidate_overlays(psprecomp::Runtime &runtime);

// The guest loaded code: a slot whose image matched no corpus is searched
// again at the next call into it.
void forget_unmatched_overlays();

} // namespace tutikumo
