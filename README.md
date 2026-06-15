# doom-saturn-core

Shared source for the two Sega Saturn Doom ports:

- **DoomSRL** — Saturn Ring Library (SRL / C++23) platform layer.
- **DoomJo** — Jo Engine (C) platform layer.

This repo holds the SDK-agnostic code that both ports compile verbatim:

- the Doom game sources (doomgeneric / Chocolate-Doom, lightly patched), and
- `r_parallel.c` / `r_parallel.h` — the dual-SH2 (master + slave) column
  renderer. Pure hardware-level C: command queue, executors, sync protocol,
  cache-coherency. Its only platform touch-points are `slSlaveFunc` (SGL, in
  both ports), a `jo_print` debug-overlay shim (implemented per port), and a
  direct CCR cache-purge register write.

Each port adds this repo as a git submodule at `core/` and compiles
`core/*.c` plus its own platform layer (`dg_saturn`, `main`, `i_sound_saturn`,
`w_file_saturn`, `syscalls`) and build system.

Patched-once, both ports benefit — e.g. the ~1-2min freeze fix
(`rp_sgl_workptr_reset` in `r_parallel.c`, which resets the SGL transient
work pointer GBR+72 that `slSlaveFunc` leaks each frame).
