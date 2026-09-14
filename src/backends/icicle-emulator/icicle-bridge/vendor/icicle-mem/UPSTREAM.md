# Icicle memory dependency

Source: https://github.com/icicle-emu/icicle-emu
Revision: 3292602fd4857b53ed653dec69398efe4cdb435e
Directory: icicle-mem
License: MIT OR Apache-2.0 (both license files retained)

The source is retained locally so Cargo builds the same memory fix on
every machine without modifying its shared Git checkout. Cargo.toml
expands the upstream workspace dependencies without changing versions.

Local changes add an explicit physical-page reclamation method, hashable
physical indices, and reset bytes/permissions when reusing a freed slot.
The caller supplies removed-page candidates and any additional virtual
address spaces it retains. Active aliases, zero pages and executed pages
are protected. Full memory snapshots retain their own copy-on-write page
data. Reclamation is opt-in; ordinary upstream unmap behavior is unchanged.
