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
    return os.path.isdir(os.path.join(path, ".git"))


def ensure_decomp(url=None, ref_override=None, assume_yes=False):
    url = url or os.environ.get("MP6_DECOMP_URL") or DEFAULT_DECOMP_URL
    pin = ref_override or read_pinned_commit()
    dest = common.DECOMP_DIR

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
            common.ok(f"decomp checkout already at the pinned commit ({pin[:12]}) -- skipping clone/fetch")
            return dest
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
    common.ok(f"decomp checkout ready at {dest} @ {pin[:12]}")
    return dest
