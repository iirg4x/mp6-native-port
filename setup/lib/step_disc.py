"""setup/lib/step_disc.py -- step 4: the user's disc.

Validates + extracts the user's own Mario Party 6 (USA / GP6E01) disc into
<decomp>/orig/GP6E01/{sys,files} -- the exact layout tools/build.py and the
decomp's own dtk tooling expect (docs/BUILDING.md, getting_started.md's
"Extract Entire Disc"). Accepts a disc image (.iso/.rvz/.gcm/.ciso/...,
opened through nod -- see lib/nod_ffi.py) or an already-extracted folder.

The disc image itself is NEVER copied into any repository -- only the
(gitignored) decomp orig/ tree receives the extracted bytes, and nothing
here ever writes back to the source path.

After extraction, this step also runs the decomp's OWN `dtk dol split`
(via its existing configure.py + ninja -- no functionality re-implemented)
just far enough to materialize build/GP6E01/include/*.inc -- small data
blobs (font bitmaps, the boot-warning screens, ...) that several game/ and
REL source files #include directly, and which tools/build.py's own
DECOMP_INC_DATA search path assumes already exist. This does NOT require
MWCC/binutils/sjiswrap (those are for the decomp's OWN byte-matching gate,
never something the native port needs) -- only the `dtk` binary, which the
decomp's tooling fetches itself from its GitHub release the first time it's
needed, the same way it always has for every decomp contributor.
"""
import os

from . import common, nod_ffi

REL_DIR = "dll"  # config/GP6E01/config.yml's modules all live under files/dll/*.rel


def _looks_extracted(decomp_dir):
    orig = os.path.join(decomp_dir, "orig", "GP6E01")
    fst = os.path.join(orig, "sys", "fst.bin")
    dol = os.path.join(orig, "sys", "main.dol")
    rel_dir = os.path.join(orig, "files", REL_DIR)
    if not (os.path.isfile(fst) and os.path.isfile(dol)):
        return False
    if not os.path.isdir(rel_dir):
        return False
    return any(f.lower().endswith(".rel") for f in os.listdir(rel_dir))


def _progress_cb():
    printer = common.ProgressPrinter("  extracting", interval=1.0)

    def cb(done, total, current):
        printer.update(done, total, current)

    return cb


def ensure_disc(decomp_dir, disc_arg=None, include_movies=False, force=False, assume_yes=False):
    orig_root = os.path.join(decomp_dir, "orig", "GP6E01")

    if not force and _looks_extracted(decomp_dir):
        rel_count = len([f for f in os.listdir(os.path.join(orig_root, "files", REL_DIR))
                          if f.lower().endswith(".rel")])
        common.ok(f"disc already extracted at {orig_root} ({rel_count} .rel modules, sys/main.dol present) "
                  "-- skipping (use --force-disc to re-extract)")
        return orig_root

    if not disc_arg:
        common.banner("Your Mario Party 6 disc")
        print("  This tool needs your own, legally-owned Mario Party 6 (USA) disc to")
        print("  supply the game's assets (models/textures/audio/text) -- nothing here")
        print("  downloads or includes any of that. It is never copied into this repo;")
        print("  it is only read from, and the extracted copy lives in the (gitignored)")
        print("  decomp checkout's orig/ folder.")
        print()
        print("  Accepted: a disc image (.iso / .rvz / .gcm / .ciso / .gcz / .wia / ...)")
        print("            or an already-extracted folder (e.g. Dolphin's 'Extract Entire Disc').")
        disc_arg = common.prompt("Path to your Mario Party 6 disc", assume_yes=assume_yes)
        if not disc_arg:
            raise common.SetupError(
                "no disc path given",
                hint="re-run with --disc \"<path to your MP6 (USA) .iso/.rvz/... or extracted folder>\"",
            )

    disc_arg = disc_arg.strip().strip('"')
    if not os.path.exists(disc_arg):
        raise common.SetupError(f"disc path does not exist: {disc_arg}")

    common.ensure_dir(orig_root)
    common.ensure_dir(os.path.join(orig_root, "sys"))
    common.ensure_dir(os.path.join(orig_root, "files"))

    if os.path.isdir(disc_arg):
        common.info(f"treating {disc_arg} as an already-extracted disc folder")
        summary = nod_ffi.extract_disc_folder(
            disc_arg, orig_root, include_movies=include_movies, progress=_progress_cb()
        )
    else:
        nod_dll = nod_ffi.find_nod_dll(os.path.join(common.TOOLCHAIN_DIR, "nod"))
        if not os.path.exists(nod_dll):
            raise common.SetupError(
                f"nod.dll not found at {nod_dll}",
                hint="run this tool's toolchain step first (it fetches nod via tools/fetch_nod.py)",
            )
        common.info(f"opening disc image via nod: {disc_arg}")
        free = common.free_space_bytes(orig_root)
        if free < 900 * 1024 * 1024:
            common.warn(f"only {common.human_size(free)} free near {orig_root}; a full extraction "
                        "needs roughly 750MB-1.1GB")
        summary = nod_ffi.extract_disc_image(
            disc_arg, orig_root, nod_dll, include_movies=include_movies, progress=_progress_cb()
        )

    common.ok(f"extracted {summary['files']} files ({common.human_size(summary['bytes'])}), "
              f"game ID {summary['game_id']}")
    if summary.get("skipped_dirs"):
        common.info(f"skipped (not needed to build/boot the menu slice): {', '.join(summary['skipped_dirs'])} "
                    "-- pass --include-movies to also extract these")

    if not _looks_extracted(decomp_dir):
        raise common.SetupError(
            "extraction finished but the result doesn't look complete "
            f"(expected sys/main.dol, sys/fst.bin, files/{REL_DIR}/*.rel under {orig_root})"
        )
    return orig_root


def run_decomp_split(decomp_dir):
    """Runs just enough of the decomp's OWN build (its configure.py + a
    narrow ninja target) to materialize build/GP6E01/include/*.inc and the
    other dtk-split-derived state tools/build.py's compile step reads.
    Deliberately targets ONE ninja output (the split step's sentinel file)
    instead of the default `ninja` target, which would chase the full
    MWCC-matching gate build (docs on that: HANDOFF.md's "137 files OK"
    gate) -- something this port has never needed and won't attempt here.
    """
    ninja = common.which("ninja")
    if not ninja:
        raise common.SetupError("ninja not found on PATH", hint="see the prerequisite check output above")

    common.info(f"configuring the decomp build ({decomp_dir}) ...")
    common.run([common_python(), "configure.py"], cwd=decomp_dir)

    split_target = os.path.join("build", "GP6E01", "config.json")
    common.info(f"running `ninja {split_target}` -- dtk's disc-split step only "
                "(fetches the small `dtk` CLI from its GitHub release on first use; "
                "no MWCC/binutils/sjiswrap needed for this)")
    common.run([ninja, split_target], cwd=decomp_dir)

    inc_dir = os.path.join(decomp_dir, "build", "GP6E01", "include")
    if not os.path.isdir(inc_dir) or not os.listdir(inc_dir):
        raise common.SetupError(
            f"expected {inc_dir} to be populated after the split step, but it's missing/empty",
            hint="check the ninja output above for a dtk error (often a build.sha1/config.yml hash "
                 "mismatch -- make sure the disc that was extracted really is an unmodified GP6E01)",
        )
    common.ok(f"decomp split-derived assets ready: {inc_dir} ({len(os.listdir(inc_dir))} files)")


def common_python():
    import sys
    return sys.executable
