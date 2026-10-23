"""Which logical CPUs the build tools may use.

MP6_CPU_EXCLUDE names logical processors the build must never run on, as a
comma-separated list of indices (for example "8,9" on a machine with a faulty
core). apply_cpu_exclusion() removes them from this process's affinity mask;
every compiler, linker and CMake/Ninja child started afterwards inherits that
mask. allowed_cpu_count() is the matching default for parallel job counts, so
an excluded or otherwise restricted machine never gets more jobs than it has
usable CPUs. Unset, nothing changes.
"""
import os
import sys


def _kernel32():
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.GetProcessAffinityMask.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    kernel32.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    return kernel32


def _windows_affinity_mask():
    """This process's affinity mask, or 0 when it cannot be read."""
    import ctypes
    kernel32 = _kernel32()
    current = ctypes.c_size_t()
    system = ctypes.c_size_t()
    if not kernel32.GetProcessAffinityMask(kernel32.GetCurrentProcess(),
                                           ctypes.byref(current), ctypes.byref(system)):
        return 0
    return current.value


def _excluded():
    raw = os.environ.get("MP6_CPU_EXCLUDE", "")
    out = set()
    for part in raw.replace(";", ",").split(","):
        part = part.strip()
        if not part:
            continue
        if not part.isdigit():
            raise SystemExit(f"[BUILD] MP6_CPU_EXCLUDE: '{part}' is not a CPU index")
        out.add(int(part))
    return out


def apply_cpu_exclusion():
    """Clear the MP6_CPU_EXCLUDE CPUs from this process's affinity mask.
    Returns the excluded set (empty when unset)."""
    excluded = _excluded()
    if not excluded:
        return excluded
    if sys.platform == "win32":
        import ctypes
        kernel32 = _kernel32()
        current = _windows_affinity_mask()
        if not current:
            raise SystemExit(f"[BUILD] MP6_CPU_EXCLUDE: GetProcessAffinityMask failed "
                             f"({ctypes.get_last_error()})")
        mask = current
        for cpu in excluded:
            mask &= ~(1 << cpu)
        if mask == 0:
            raise SystemExit("[BUILD] MP6_CPU_EXCLUDE removes every CPU this process may use")
        if mask != current and not kernel32.SetProcessAffinityMask(kernel32.GetCurrentProcess(), mask):
            raise SystemExit(f"[BUILD] MP6_CPU_EXCLUDE: SetProcessAffinityMask failed "
                             f"({ctypes.get_last_error()})")
    elif hasattr(os, "sched_setaffinity"):
        allowed = os.sched_getaffinity(0) - excluded
        if not allowed:
            raise SystemExit("[BUILD] MP6_CPU_EXCLUDE removes every CPU this process may use")
        os.sched_setaffinity(0, allowed)
    return excluded


def allowed_cpu_count():
    """CPUs this process may run on (affinity-aware), at least 1."""
    count = None
    if sys.platform == "win32":
        # os.process_cpu_count() ignores the affinity mask on Windows.
        mask = _windows_affinity_mask()
        if mask:
            count = bin(mask).count("1")
    elif hasattr(os, "process_cpu_count"):
        count = os.process_cpu_count()
    elif hasattr(os, "sched_getaffinity"):
        count = len(os.sched_getaffinity(0))
    return max(1, count or os.cpu_count() or 4)
