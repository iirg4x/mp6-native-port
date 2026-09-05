# Decomp Dependency

The Mario Party 6 decompilation checked out at
`../../external_refs/repos/marioparty6` (tracking `main` in
[`iirg4x/marioparty6`](https://github.com/iirg4x/marioparty6), not the original
upstream repository) is the source of truth for all recovered
game code. This port does not vendor, fork, or modify that source; it
consumes it read-only.

## How it's consumed

For now, the decomp repo is referenced by a path-based include: a relative
path out of this repository and into the sibling checkout. This is
deliberately low-ceremony while the port and the decomp repo live side by
side on the same machine. If this repository is ever pushed to a remote or
shared across machines, that path-based reference should be replaced with a
proper git submodule pinned to a specific decomp commit, since a relative
path pointing outside this repository will not resolve for anyone else who
clones it.

## Currently tracking

```
8f9c3c010da32352908b637e7d6c46e8d99989e7
```

This must match the output of the following command, run from the Port
repository root with the dependency checked out at the pinned revision:

```sh
git -C ../../external_refs/repos/marioparty6 rev-parse HEAD
```

i.e. the commit `HEAD` was pointing at in
`../../external_refs/repos/marioparty6` as of this document's last update.
Update this value whenever the port is resynced against a newer decomp
commit.

The port patch queue was rebased onto this exact `main` revision on
2026-09-02. The prior documented pin was
`4a6761094935be3588ca2b1eda0a71a0988f8efb`; it predates later recovered
board and REL source that the current port build consumes. The setup tool
reads this value at run time, so keeping the pin and patch queue synchronized
is a build-integrity requirement rather than informational bookkeeping.

Several board owners recovered at this revision still declare address-named
`.sdata2` constants whose storage comes from the original DOL in the matching
decomp build. The native link has no such binary input, so
`platform/os/board_constants.c` supplies bit-exact C definitions verified
against the GP6E01 `main.dol` hash recorded by the decomp project. This is a
data-ownership bridge, not a placeholder implementation; it should shrink as
upstream moves those constants into recovered C.
