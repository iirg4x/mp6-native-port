"""setup/lib/common.py -- shared plumbing for the local setup tool.

Everything here is plain-stdlib, cross-platform (Windows/Linux/macOS) Python
3.8+. No third-party dependencies -- the whole point of this tool is to be
runnable on a machine that has nothing but a stock Python interpreter, so it
must not require `pip install` of anything before it can even start reporting
what's missing.

Path layout (mirrors tools/build.py's own resolution -- this file does NOT
change that resolution, only replicates it so the setup tool and build.py
always agree on where things live):

    <root>/port/mp6-native[-setup]/setup/lib/common.py   (this file)
    <root>/port/mp6-native[-setup]/                       NATIVE_ROOT
    <root>/port/                                          PORT_ROOT
    <root>/port/toolchain/                                TOOLCHAIN_DIR
    <root>/external_refs/repos/marioparty6/                DECOMP_DIR
    <root>/external_refs/repos/aurora/                     AURORA_DIR
"""
import ctypes
import os
import shutil
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Path constants
# ---------------------------------------------------------------------------

LIB_DIR = os.path.dirname(os.path.abspath(__file__))          # .../setup/lib
SETUP_DIR = os.path.dirname(LIB_DIR)                           # .../setup
NATIVE_ROOT = os.path.dirname(SETUP_DIR)                       # .../mp6-native(-setup)
PORT_ROOT = os.path.dirname(NATIVE_ROOT)                       # .../port
WORKSPACE_ROOT = os.path.dirname(PORT_ROOT)                    # the outer project root

TOOLCHAIN_DIR = os.path.join(PORT_ROOT, "toolchain")
DECOMP_DIR = os.path.normpath(os.path.join(PORT_ROOT, "..", "external_refs", "repos", "marioparty6"))
AURORA_DIR = os.path.normpath(os.path.join(PORT_ROOT, "..", "external_refs", "repos", "aurora"))

IS_WINDOWS = os.name == "nt"


class SetupError(Exception):
    """A user-facing failure. `hint` (if set) is printed as actionable advice."""

    def __init__(self, message, hint=None):
        super().__init__(message)
        self.message = message
        self.hint = hint


# ---------------------------------------------------------------------------
# Console output. ANSI colors are best-effort only -- every message is also
# unambiguous in plain text (tagged [OK]/[FAIL]/[WARN]/...) so a dumb
# terminal (or a captured log file) is just as readable.
# ---------------------------------------------------------------------------

_COLOR_ENABLED = False


def _enable_windows_vt100():
    """Best-effort: turn on ANSI escape processing in the current Windows
    console (ENABLE_VIRTUAL_TERMINAL_PROCESSING). Silently no-ops if it
    can't -- plain text still works everywhere."""
    if not IS_WINDOWS:
        return True
    try:
        kernel32 = ctypes.windll.kernel32
        h = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if not kernel32.GetConsoleMode(h, ctypes.byref(mode)):
            return False
        new_mode = mode.value | 0x0004  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
        return bool(kernel32.SetConsoleMode(h, new_mode))
    except Exception:
        return False


def init_console():
    global _COLOR_ENABLED
    if os.environ.get("NO_COLOR"):
        _COLOR_ENABLED = False
        return
    ok = _enable_windows_vt100() if IS_WINDOWS else sys.stdout.isatty()
    _COLOR_ENABLED = bool(ok)


def _c(code, text):
    if not _COLOR_ENABLED:
        return text
    return f"\x1b[{code}m{text}\x1b[0m"


def banner(title):
    line = "=" * 78
    print(f"\n{line}\n{title}\n{line}")


def step(n, total, title):
    print(f"\n{_c('1;36', f'[STEP {n}/{total}]')} {title}")
    print("-" * 78)


def info(msg):
    print(f"  {msg}")


def ok(msg):
    print(f"  {_c('1;32', '[OK]')} {msg}")


def warn(msg):
    print(f"  {_c('1;33', '[WARN]')} {msg}")


def error(msg):
    print(f"  {_c('1;31', '[FAIL]')} {msg}")


def hint(msg):
    print(f"  {_c('2', '  -> ' + msg)}")


def confirm(prompt, default=True, assume_yes=False):
    if assume_yes:
        return True
    suffix = " [Y/n] " if default else " [y/N] "
    try:
        reply = input(f"  {prompt}{suffix}").strip().lower()
    except EOFError:
        return default
    if not reply:
        return default
    return reply in ("y", "yes")


def prompt(text, default=None, assume_yes=False):
    """A plain text prompt (not yes/no). If assume_yes is set and there's a
    default, returns the default without blocking on stdin (keeps
    non-interactive/scripted runs from hanging)."""
    suffix = f" [{default}]" if default else ""
    if assume_yes and default is not None:
        return default
    try:
        reply = input(f"  {text}{suffix}: ").strip()
    except EOFError:
        reply = ""
    return reply or (default or "")


# ---------------------------------------------------------------------------
# Subprocess helpers
# ---------------------------------------------------------------------------

def which(name):
    return shutil.which(name)


def run(cmd, cwd=None, env=None, check=True, stream=True, quiet=False):
    """Runs `cmd` (a list -- never a shell string, so spaces-in-paths are
    never a quoting concern), streaming stdout/stderr live so long steps
    (clones, compiles) show progress instead of going silent. Returns the
    process return code. Raises SetupError if check and the command fails.
    """
    printable = " ".join(f'"{c}"' if isinstance(c, str) and " " in c else str(c) for c in cmd)
    if not quiet:
        print(f"  $ {printable}")
    full_env = None
    if env is not None:
        full_env = dict(os.environ)
        full_env.update(env)
    if stream:
        proc = subprocess.Popen(
            cmd, cwd=cwd, env=full_env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1, universal_newlines=True,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            print(f"    {line.rstrip()}")
        proc.wait()
        rc = proc.returncode
    else:
        proc = subprocess.run(cmd, cwd=cwd, env=full_env, capture_output=True, text=True)
        if proc.stdout:
            print(proc.stdout)
        if proc.stderr:
            print(proc.stderr, file=sys.stderr)
        rc = proc.returncode
    if check and rc != 0:
        raise SetupError(f"command failed (exit {rc}): {printable}")
    return rc


def run_capture(cmd, cwd=None, check=False):
    """Runs `cmd`, returns (returncode, stdout_text). Never prints -- for
    quiet probes (`git rev-parse HEAD`, `--version` checks, ...)."""
    try:
        proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    except FileNotFoundError:
        return 127, ""
    if check and proc.returncode != 0:
        raise SetupError(f"command failed (exit {proc.returncode}): {' '.join(cmd)}\n{proc.stderr}")
    return proc.returncode, (proc.stdout or "").strip()


# ---------------------------------------------------------------------------
# Misc
# ---------------------------------------------------------------------------

def human_size(n):
    n = float(n)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024.0:
            return f"{n:3.1f}{unit}"
        n /= 1024.0
    return f"{n:.1f}PB"


class ProgressPrinter:
    """Throttled single-line-ish progress printer (prints a new line at most
    every `interval` seconds, so a fast copy loop doesn't flood the console)."""

    def __init__(self, label, interval=0.5):
        self.label = label
        self.interval = interval
        self._last = 0.0

    def update(self, done, total, current=""):
        now = time.time()
        if now - self._last < self.interval and done != total:
            return
        self._last = now
        pct = (100.0 * done / total) if total else 0.0
        cur = f" ({current})" if current else ""
        print(f"  {self.label}: {pct:5.1f}%{cur}", end="\r" if done != total else "\n")
        sys.stdout.flush()


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)
    return path


def free_space_bytes(path):
    while not os.path.exists(path):
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    usage = shutil.disk_usage(path)
    return usage.free
