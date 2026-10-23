"""CPU-only validation for the platform-default XeGTAO board-QA mode."""

from __future__ import annotations

import re


DEFAULT_SELECTION_MARKER = (
    "[MP6-AO] requested implementation: XeGTAO (platform default)"
)
DEFAULT_REFERENCE_MARKER = (
    "[MP6-AO] XeGTAO reference: full-resolution direct uint output, High, "
    "2 spatial denoise passes"
)
DEFAULT_DEPTH_STORAGE_MARKER = (
    "[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float"
)

_SHUTDOWN_COUNTER_PREFIX = "[MP6-AO] encoded="
_SHUTDOWN_COUNTER_RE = re.compile(
    r"^\[MP6-AO\] encoded=(\d+) composited=(\d+) foliage=(\d+) xegtao=(\d+)\s*$"
)

# These are deliberately narrow markers.  Generic game warnings are not an
# AO-default failure, but a validation error, fatal, crash, or XeGTAO fallback
# means the requested default path was not cleanly exercised.
DEFAULT_BAD_MARKERS = (
    "[MP6-CRASH]",
    "[FATAL]",
    "[AURORA FATAL:",
    "[AURORA ERROR",
    "GPUValidationError",
    "uncaptured error",
    "[MP6-AO] XeGTAO unavailable:",
    "[MP6-AO] XeGTAO experimental:",
    "[MP6-AO] invalid ",
    "using legacy",
    "fallback",
)


def default_ao_error(implementation: str, ao: int, quality: str,
                     denoise: int, resolution: str, depth_storage: str) -> str | None:
    """Return an admission error for a non-reference default invocation."""

    if implementation != "default":
        return None
    if ao <= 0:
        return "--implementation default requires --ao > 0"
    expected = {
        "--xegtao-quality": (quality, "high"),
        "--xegtao-denoise": (denoise, 2),
        "--xegtao-resolution": (resolution, "full"),
        "--xegtao-depth-storage": (depth_storage, "r32float"),
    }
    for option, (actual, reference) in expected.items():
        if actual != reference:
            return (f"--implementation default requires {option}={reference}; "
                    f"received {actual!r}")
    return None


def _marker_count(log: str, marker: str) -> int:
    return log.count(marker)


def _shutdown_counters(log: str) -> list[tuple[int, int, int, int]]:
    records: list[tuple[int, int, int, int]] = []
    for line in log.splitlines():
        if not line.startswith(_SHUTDOWN_COUNTER_PREFIX):
            continue
        match = _SHUTDOWN_COUNTER_RE.fullmatch(line)
        if match is None:
            raise ValueError("malformed XeGTAO shutdown counter record")
        records.append(tuple(int(value) for value in match.groups()))
    return records


def validate_default_ao(log: str, exit_code: int = 0,
                        reason: str = "exited") -> dict[str, object]:
    """Require clean platform-default High/2/full/R32 XeGTAO evidence."""

    if not isinstance(log, str):
        raise TypeError("log must be text")
    if exit_code != 0 or reason != "exited":
        raise RuntimeError(
            f"platform-default XeGTAO run did not exit cleanly: "
            f"exit={exit_code} reason={reason}"
        )
    for marker, description in (
        (DEFAULT_SELECTION_MARKER, "platform-default selection"),
        (DEFAULT_REFERENCE_MARKER, "reference High/2/full initialization"),
        (DEFAULT_DEPTH_STORAGE_MARKER, "R32 depth-storage initialization"),
    ):
        if _marker_count(log, marker) != 1:
            raise RuntimeError(
                f"platform-default XeGTAO run requires exactly one {description} log"
            )
    bad = next((marker for marker in DEFAULT_BAD_MARKERS
                if marker.casefold() in log.casefold()), None)
    if bad is not None:
        raise RuntimeError(
            f"platform-default XeGTAO run reported an error or fallback marker: {bad}"
        )

    records = _shutdown_counters(log)
    if len(records) != 1:
        raise RuntimeError(
            "platform-default XeGTAO run requires exactly one shutdown counter record"
        )
    encoded, composited, foliage, xegtao = records[0]
    if encoded <= 0 or composited <= 0 or xegtao <= 0:
        raise RuntimeError(
            "platform-default XeGTAO run produced no positive encoded/composited/XeGTAO work"
        )
    if encoded != composited or encoded != xegtao:
        raise RuntimeError(
            f"platform-default XeGTAO counters disagree: encoded={encoded} "
            f"composited={composited} xegtao={xegtao}"
        )
    return {
        "implementation": "default",
        "selection": "platform-default",
        "reference": "High2-full-r32",
        "markers": {
            "selection": DEFAULT_SELECTION_MARKER,
            "reference": DEFAULT_REFERENCE_MARKER,
            "depth_storage": DEFAULT_DEPTH_STORAGE_MARKER,
        },
        "counters": {
            "encoded": encoded,
            "composited": composited,
            "foliage": foliage,
            "xegtao": xegtao,
        },
        "counter_records": len(records),
        "exit_code": exit_code,
        "reason": reason,
    }


# Descriptive aliases for callers/tests that use the feature name first.
validate_platform_default_ao = validate_default_ao
platform_default_ao_error = default_ao_error
