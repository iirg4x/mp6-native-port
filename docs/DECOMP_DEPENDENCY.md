# Decomp Dependency

The Mario Party 6 decompilation checked out at
`../../external_refs/repos/marioparty6` (tracking the `fork/main` branch —
the `iirg4x` fork — not upstream) is the source of truth for all recovered
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
b05ede1d53f5763539a4a33ab0505b4d7749b96d
```

This is the output of, run from this file's location:

```sh
git -C ../../external_refs/repos/marioparty6 rev-parse HEAD
```

i.e. the commit `HEAD` was pointing at in
`../../external_refs/repos/marioparty6` as of this document's last update.
Update this value whenever the port is resynced against a newer decomp
commit.
