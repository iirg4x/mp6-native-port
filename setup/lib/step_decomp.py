"""setup/lib/step_decomp.py -- step 3: the decomp checkout.

Clones (or resyncs) the Mario Party 6 decompilation repo to the exact
commit pinned in docs/DECOMP_DEPENDENCY.md, at the sibling path
tools/build.py's own DECOMP constant expects
(PORT_ROOT/../external_refs/repos/marioparty6 -- see common.py's docstring).
The pin is parsed out of the doc at RUN TIME (never hardcoded here), so this
step always tracks whatever docs/DECOMP_DEPENDENCY.md says today.
"""
import os
import re

from . import common

DEFAULT_DECOMP_URL = "https://github.com/iirg4x/marioparty6.git"
PIN_HEADING_RE = re.compile(r"##\s*Currently tracking", re.IGNORECASE)
SHA_RE = re.compile(r"\b([0-9a-f]{40})\b")


def read_pinned_commit():
    doc = os.path.join(common.NATIVE_ROOT, "docs", "DECOMP_DEPENDENCY.md")
    if not os.path.exists(doc):
        raise common.SetupError(f"{doc} is missing -- can't determine which decomp commit to pin to")
    with open(doc, "r", encoding="utf-8") as f:
        text = f.read()
    heading = PIN_HEADING_RE.search(text)
    search_from = heading.end() if heading else 0
    m = SHA_RE.search(text, search_from)
    if not m:
        raise common.SetupError(f"couldn't find a 40-character commit hash in {doc}")
    return m.group(1)


def _is_git_repo(path):
    # Linked worktrees carry a .git *file* pointing at the primary repo's
    # worktrees metadata, while ordinary clones carry a .git directory.
    # Both are valid read-only dependency checkouts.
    return os.path.exists(os.path.join(path, ".git"))


def decomp_checkout_problem(path, expected_commit):
    """Return a reproducibility failure for a deviated build-input tree."""
    if not _is_git_repo(path):
        return f"{path} is not a git checkout"
    rc, head = common.run_capture(["git", "-C", path, "rev-parse", "HEAD"])
    if rc != 0:
        return f"could not read the decomp checkout revision at {path}"
    if head != expected_commit:
        return f"decomp checkout is at {head!r}, expected {expected_commit!r}"
    rc, tracked = common.run_capture(
        ["git", "-C", path, "status", "--porcelain=v1", "--untracked-files=no"]
    )
    if rc != 0:
        return f"could not inspect tracked decomp changes at {path}"
    # Untracked files outside build-input roots (scratch/, build/, local
    # analysis outputs) cannot affect this port and are intentionally ignored.
    rc, untracked_inputs = common.run_capture(
        ["git", "-C", path, "ls-files", "--others", "--exclude-standard", "--",
         "src", "include", "libs"]
    )
    if rc != 0:
        return f"could not inspect untracked decomp build inputs at {path}"
    changes = [line for line in (tracked + "\n" + untracked_inputs).splitlines() if line.strip()]
    if changes:
        shown = ", ".join(changes[:5])
        suffix = f" (+{len(changes) - 5} more)" if len(changes) > 5 else ""
        return f"decomp checkout has modified/untracked build inputs: {shown}{suffix}"
    return None


def validate_decomp_checkout(path, expected_commit, allow_dirty=False):
    problem = decomp_checkout_problem(path, expected_commit)
    if problem and not allow_dirty:
        raise common.SetupError(
            problem,
            hint="restore/stash the decomp changes, or explicitly opt into a non-reproducible "
                 "development build with MP6_ALLOW_DIRTY_DECOMP=1",
        )
    if problem:
        common.warn(f"decomp integrity override active: {problem}")
    return True


def ensure_decomp(url=None, ref_override=None, assume_yes=False):
    url = url or os.environ.get("MP6_DECOMP_URL") or DEFAULT_DECOMP_URL
    pin = ref_override or read_pinned_commit()
    dest = common.DECOMP_DIR
    allow_dirty = os.environ.get("MP6_ALLOW_DIRTY_DECOMP") == "1"

    common.info(f"decomp dependency: {url}")
    common.info(f"pinned commit (from docs/DECOMP_DEPENDENCY.md unless overridden): {pin}")
    common.info(f"target checkout: {dest}")

    if os.path.isdir(dest) and os.listdir(dest):
        if not _is_git_repo(dest):
            raise common.SetupError(
                f"{dest} already exists and is non-empty, but isn't a git repository",
                hint="remove or rename that directory and re-run, or point --decomp-dir "
                     "logic (see setup/README.md) elsewhere",
            )
        rc, head = common.run_capture(["git", "-C", dest, "rev-parse", "HEAD"])
        if rc == 0 and head == pin:
            validate_decomp_checkout(dest, pin, allow_dirty=allow_dirty)
            common.ok(f"decomp checkout already at the pinned commit ({pin[:12]}) -- skipping clone/fetch")
            return dest
        # Never attempt a checkout over local modifications. That both risks
        # clobbering work and can leave a mixed tree when only some paths
        # conflict with the target revision.
        rc, dirty = common.run_capture(
            ["git", "-C", dest, "status", "--porcelain=v1", "--untracked-files=no"]
        )
        if rc != 0 or dirty:
            raise common.SetupError(
                f"existing decomp checkout is not at {pin[:12]} and has tracked modifications",
                hint="restore/stash that checkout before setup changes its revision",
            )
        common.info(f"existing checkout is at {head[:12] if head else '?'}, fetching + checking out {pin[:12]}")
        common.run(["git", "-C", dest, "remote", "set-url", "origin", url], check=False)
        common.run(["git", "-c", "core.longpaths=true", "-C", dest, "fetch", "--tags", "origin"])
        common.run(["git", "-c", "core.longpaths=true", "-C", dest, "checkout", "--detach", pin])
    else:
        common.ensure_dir(os.path.dirname(dest))
        common.info(f"cloning decomp (this fetches SOURCE CODE only -- no game assets travel over the "
                    f"network; assets come from your own disc in the next step)")
        # core.longpaths=true: some source trees + a deeply-nested destination
        # path can exceed Win32's classic MAX_PATH; this tells Git for
        # Windows to use the \\?\ long-path APIs instead of failing with
        # "Filename too long" partway through an otherwise-fine clone.
        common.run(["git", "-c", "core.longpaths=true", "clone", url, dest])
        common.run(["git", "-c", "core.longpaths=true", "-C", dest, "checkout", "--detach", pin])

    rc, head = common.run_capture(["git", "-C", dest, "rev-parse", "HEAD"])
    if rc != 0 or head != pin:
        raise common.SetupError(
            f"decomp checkout at {dest} is at {head!r}, expected pinned commit {pin!r}",
            hint="the pin in docs/DECOMP_DEPENDENCY.md may not exist on the configured remote; "
                 "check --decomp-url",
        )
    validate_decomp_checkout(dest, pin, allow_dirty=allow_dirty)
    common.ok(f"decomp checkout ready at {dest} @ {pin[:12]}")
    return dest
