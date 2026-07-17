#!/usr/bin/env python3
"""setup/setup.py -- local setup tool for the Mario Party 6 native port.

Turns "I own a Mario Party 6 disc" into a playable build ON THIS MACHINE.

WHY THIS EXISTS (read this before anything else):
A prebuilt mp6native.exe/APK can't be distributed -- it would contain the
recompiled game code (the decompilation), which isn't this project's to
redistribute. So instead of shipping a binary, this repository ships THIS
setup tool: you run it, point it at your OWN legally-owned disc, and it
fetches the decomp SOURCE + a standard compiler toolchain and builds the
playable port right here. The result:
  - This repository (and this tool) contain ZERO game code and ZERO game
    assets, before or after you run it.
  - Game CODE comes from the pinned decompilation repository, fetched as
    source over git (exactly the way any open-source dependency is) --
    see docs/DECOMP_DEPENDENCY.md.
  - Game ASSETS (models/textures/audio/text) come from YOUR OWN disc,
    read once to extract them, never uploaded anywhere, never copied into
    any repository.
  - The playable binary is compiled locally, on your machine, by a
    standard toolchain (zig) fetched from its official distributor.

Usage:
    python setup/setup.py --disc "D:\\path\\to\\Mario Party 6 (USA).iso"
    python setup/setup.py                      # prompts for the disc path
    python setup/setup.py --android            # also build the Android APK
    python setup/setup.py --help               # every flag

See setup/README.md for the full walkthrough and docs/SETUP_TOOL.md for the
design writeup (including exactly what this project's own end-to-end
verification did and didn't exercise).
"""
import argparse
import os
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import common, step_prereqs, step_toolchain, step_decomp, step_disc, step_aurora, step_build, step_android  # noqa: E402

TOTAL_CORE_STEPS = 6


LEGAL_BANNER = """
  This tool builds a playable native port of Mario Party 6 ON THIS MACHINE.

    * You must legally own a Mario Party 6 (USA / GP6E01) disc or its
      dump. This tool reads it once, locally, to extract game assets.
    * No Nintendo material -- no game code, no game assets -- is included
      in this repository or in this tool. Ever.
    * Game CODE is fetched as SOURCE from a public decompilation
      repository (a from-scratch reimplementation, standard practice in
      the decompilation community) and compiled here, by you, with a
      standard open-source compiler toolchain (zig, fetched from
      ziglang.org).
    * Game ASSETS come only from the disc you point this tool at. They
      are extracted to a local, git-ignored folder -- never uploaded,
      never committed, never redistributed by this tool.

  See docs/SETUP_TOOL.md for the full design rationale.
"""


def parse_args(argv):
    p = argparse.ArgumentParser(
        description="Local setup tool: disc + decomp source + toolchain -> a playable native build.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--disc", metavar="PATH",
                   help="path to your Mario Party 6 (USA) disc image (.iso/.rvz/.gcm/.ciso/...) "
                        "or an already-extracted folder. Prompted for if omitted and not already extracted.")
    p.add_argument("--include-movies", action="store_true",
                   help="also extract files/movie/ (336MB of FMVs, not needed to build or boot to the "
                        "menu slice this port currently covers). Default: skipped.")
    p.add_argument("--force-disc", action="store_true", help="re-extract the disc even if already done")
    p.add_argument("--decomp-url", metavar="URL", help="override the decomp git remote "
                   "(default: the fork URL, or $MP6_DECOMP_URL)")
    p.add_argument("--decomp-ref", metavar="SHA", help="override the pinned decomp commit "
                   "(default: parsed from docs/DECOMP_DEPENDENCY.md)")
    p.add_argument("--aurora-url", metavar="URL", help="override the Aurora git remote "
                   "(default: https://github.com/encounter/aurora.git, or $MP6_AURORA_URL)")
    p.add_argument("--build-aurora", action="store_true",
                   help="if Aurora isn't already built, attempt a from-scratch build (best-effort, "
                        "20-60+ minutes; default: print the manual recipe and stop)")
    p.add_argument("--headless-only", action="store_true", help="only build mp6native_headless.exe")
    p.add_argument("--windowed-only", action="store_true", help="only build mp6native.exe (windowed)")
    p.add_argument("--coro-fibers", action="store_true", help="passthrough to tools/build.py's A/B lever")
    p.add_argument("--dist-dir", metavar="PATH", help="where to assemble the runnable build "
                   "(default: <this checkout>/dist)")
    p.add_argument("-j", "--jobs", type=int, help="parallel compile jobs (default: build.py's own default)")
    p.add_argument("--android", action="store_true", help="also build the Android lane (SDK/NDK + APK)")
    p.add_argument("--skip-apk", action="store_true", help="with --android: build the .so rows but skip "
                   "the gradle APK assembly")
    p.add_argument("-y", "--yes", action="store_true", help="assume yes / don't block on prompts "
                   "(needed for non-interactive / scripted runs)")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    common.init_console()

    common.banner("Mario Party 6 -- local setup")
    print(LEGAL_BANNER)
    if not common.confirm("Continue?", default=True, assume_yes=args.yes):
        print("Aborted.")
        return 1

    total_steps = TOTAL_CORE_STEPS + (1 if args.android else 0)

    try:
        common.step(1, total_steps, "Checking prerequisites (git, python3, ninja, cmake)")
        if not step_prereqs.check(assume_yes=args.yes):
            return 1

        common.step(2, total_steps, "Toolchain (zig compiler + nod disc-image library)")
        step_toolchain.ensure_zig(assume_yes=args.yes)
        step_toolchain.ensure_nod(assume_yes=args.yes)

        common.step(3, total_steps, "Decomp source (game code, fetched as source, read-only)")
        decomp_dir = step_decomp.ensure_decomp(
            url=args.decomp_url, ref_override=args.decomp_ref, assume_yes=args.yes
        )

        common.step(4, total_steps, "Your disc (game assets, extracted locally, never redistributed)")
        step_disc.ensure_disc(
            decomp_dir, disc_arg=args.disc, include_movies=args.include_movies,
            force=args.force_disc, assume_yes=args.yes,
        )
        step_disc.run_decomp_split(decomp_dir)

        common.step(5, total_steps, "Aurora graphics backend (detect existing build, or build it)")
        step_aurora.ensure_aurora(auto_build=args.build_aurora, url=args.aurora_url, assume_yes=args.yes)

        common.step(6, total_steps, "Building mp6native + assembling dist/")
        windowed = not args.headless_only
        headless = not args.windowed_only
        step_build.build(common.NATIVE_ROOT, headless=headless, windowed=windowed,
                          jobs=args.jobs, coro_fibers=args.coro_fibers)
        dist_dir = step_build.assemble_dist(common.NATIVE_ROOT, dist_dir=args.dist_dir)

        if args.android:
            common.step(7, total_steps, "Android lane (SDK/NDK, .so rows, APK)")
            step_android.run_android_lane(common.NATIVE_ROOT, jobs=args.jobs, assume_yes=args.yes,
                                          skip_apk=args.skip_apk)

        common.banner("Done")
        exe = os.path.join(dist_dir, "mp6native.exe")
        headless_exe = os.path.join(dist_dir, "mp6native_headless.exe")
        if os.path.exists(exe):
            print(f"  Run this:  {exe}")
        if os.path.exists(headless_exe):
            print(f"  (headless/CI build also available: {headless_exe})")
        print(f"\n  Everything needed to play now lives under: {dist_dir}")
        print("  Settings persist to mp6_config.json next to the executable.")
        return 0

    except common.SetupError as exc:
        common.error(exc.message)
        if exc.hint:
            common.hint(exc.hint)
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted.")
        return 130
    except Exception:  # noqa: BLE001 -- top-level: never crash without context
        common.error("unexpected error:")
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    sys.exit(main())
