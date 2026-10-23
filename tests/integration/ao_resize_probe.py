"""Opt-in hidden-window resize stress probe for the board AO harness.

The probe is injected into the generated QA copy of ``aurora_bridge.c`` only.
It never changes production sources.  Requests happen after the W01 live event,
at a closed frame boundary immediately before the normal Aurora event pump;
the next ``aurora_update``/``aurora_begin_frame`` therefore owns the ordinary
SDL resize and swapchain synchronization path.
"""

from __future__ import annotations

import json
import re
from typing import Any

from tests.integration.ao_resource_probe import V2_PROVENANCE


RESIZE_ENV = "MP6_QA_RESIZE_AO"
MARKER = "MP6_AO_RESIZE_QA"
TARGET_SIZES = ((1281, 721), (960, 540), (1281, 721))
TARGET_OFFSETS = (60, 180, 300)

_BRIDGE_ANCHOR = "void VIWaitForRetrace(void)\n{"
_EVENT_PUMP_ANCHOR = "    mp6_dispatch_aurora_events(aurora_update());"


RESIZE_DRIVER = r'''
/* MP6_AO_RESIZE_QA: QA-only hidden-window stress.  This function is called
 * after the previous frame has closed and immediately before the ordinary
 * aurora_update()/aurora_begin_frame() boundary.  It never opens, focuses,
 * raises, or saves a user window. */
static void qa_resize_ao_tick(void)
{
    static unsigned long armedSeq;
    static long startTick = -1;
    static long requestTick;
    static int nextStep;
    static int pendingStep = -1;
    static const long offsets[] = {60, 180, 300};
    static const int widths[] = {1281, 960, 1281};
    static const int heights[] = {721, 540, 721};
    const char *enabled = getenv("MP6_QA_RESIZE_AO");
    SDL_Window *window;
    int logicalWidth, logicalHeight, pixelWidth, pixelHeight;

    if (!enabled || enabled[0] == '\0' || enabled[0] == '0') return;

    if (startTick < 0) {
        const unsigned long seq = mp6_event_last_seq("w01.live");
        if (!seq || seq == armedSeq || !mp6_event_matches("w01.live", NULL)) return;
        armedSeq = seq;
        startTick = mp6_tick_count;
        printf("[AO-RESIZE] {\"phase\":\"armed\",\"tick\":%ld,\"seq\":%lu}\n",
               mp6_tick_count, seq);
        fflush(stdout);
    }

    window = mp6_aspect_window();
    if (!window || !(SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN)) {
        fprintf(stderr, "[FATAL] AO resize QA requires its own hidden window\n");
        exit(3);
    }

    /* A request is applied by SDL/Aurora on the following event/frame
     * boundary.  Observe it here, before the next request, after that
     * boundary has completed.  Pixel dimensions are the AO census identity;
     * logical dimensions are retained to make DPI behavior explicit. */
    if (pendingStep >= 0) {
        if (!SDL_GetWindowSize(window, &logicalWidth, &logicalHeight) ||
            !SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight)) {
            fprintf(stderr, "[FATAL] AO resize QA could not read applied window size: %s\n",
                    SDL_GetError());
            exit(3);
        }
        if (pixelWidth == widths[pendingStep] && pixelHeight == heights[pendingStep]) {
            printf("[AO-RESIZE] {\"phase\":\"applied\",\"step\":%d,\"tick\":%ld,"
                   "\"requested\":[%d,%d],\"logical\":[%d,%d],\"pixels\":[%d,%d],"
                   "\"hidden\":1}\n",
                   pendingStep, mp6_tick_count, widths[pendingStep], heights[pendingStep],
                   logicalWidth, logicalHeight, pixelWidth, pixelHeight);
            fflush(stdout);
            pendingStep = -1;
        } else if (mp6_tick_count - requestTick > 30) {
            fprintf(stderr, "[FATAL] AO resize QA did not settle step=%d requested=%dx%d "
                    "pixels=%dx%d\n", pendingStep, widths[pendingStep], heights[pendingStep],
                    pixelWidth, pixelHeight);
            exit(3);
        }
    }

    if (nextStep < 3 && mp6_tick_count - startTick >= offsets[nextStep]) {
        if (pendingStep >= 0) {
            fprintf(stderr, "[FATAL] AO resize QA step=%d was not applied before step=%d\n",
                    pendingStep, nextStep);
            exit(3);
        }
        /* Normal startup policy constrains a window to 4:3.  This opt-in
         * fixture deliberately frees only this hidden QA window so the
         * requested 16:9 sizes reach the normal resize/swapchain path. */
        if (!SDL_SetWindowAspectRatio(window, 0.0f, 0.0f) ||
            !SDL_SetWindowSize(window, widths[nextStep], heights[nextStep])) {
            fprintf(stderr, "[FATAL] AO resize QA request step=%d failed: %s\n",
                    nextStep, SDL_GetError());
            exit(3);
        }
        printf("[AO-RESIZE] {\"phase\":\"request\",\"step\":%d,\"tick\":%ld,"
               "\"requested\":[%d,%d],\"hidden\":1}\n",
               nextStep, mp6_tick_count, widths[nextStep], heights[nextStep]);
        fflush(stdout);
        pendingStep = nextStep++;
        requestTick = mp6_tick_count;
    }
}
'''


def _replace_once(source: str, anchor: str, replacement: str) -> str:
    count = source.count(anchor)
    if count != 1:
        raise ValueError(f"resize probe anchor changed: {anchor!r} count={count}")
    return source.replace(anchor, replacement, 1)


def resize_probe(source: str) -> str:
    """Inject the disabled-by-default resize driver into bridge source."""

    if not isinstance(source, str):
        raise TypeError("source must be text")
    if MARKER in source:
        raise ValueError("resize probe already instrumented")
    source = _replace_once(
        source,
        _BRIDGE_ANCHOR,
        "static void qa_resize_ao_tick(void);\n\n" + _BRIDGE_ANCHOR,
    )
    source = _replace_once(
        source,
        _EVENT_PUMP_ANCHOR,
        "    qa_resize_ao_tick();\n" + _EVENT_PUMP_ANCHOR,
    )
    return source + RESIZE_DRIVER


def resize_ao_error(enabled: bool, window_size: str, ao: int,
                    implementation: str) -> str | None:
    """Return a parser error for an invalid resize-AO invocation."""

    if not enabled:
        return None
    if window_size != "1281x721":
        return "--resize-ao requires --window-size 1281x721"
    if ao <= 0:
        return "--resize-ao requires --ao > 0"
    if implementation != "xegtao":
        return "--resize-ao requires --implementation xegtao"
    return None


def _strict_json(payload: str, marker: str) -> dict[str, Any]:
    def reject_duplicate(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate field in {marker}: {key}")
            result[key] = value
        return result

    try:
        value = json.loads(payload, object_pairs_hook=reject_duplicate)
    except (TypeError, json.JSONDecodeError) as exc:
        raise ValueError(f"malformed {marker} JSON") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{marker} JSON must be an object")
    return value


def parse_resize_log(log: str) -> list[dict[str, Any]]:
    """Parse the complete ordered resize trace, rejecting malformed rows."""

    if not isinstance(log, str):
        raise TypeError("log must be text")
    rows: list[dict[str, Any]] = []
    for line in log.splitlines():
        if not line.startswith("[AO-RESIZE]"):
            continue
        rows.append(_strict_json(line[len("[AO-RESIZE]"):].strip(), "[AO-RESIZE]"))
    return rows


def _int_pair(value: Any, field: str) -> tuple[int, int]:
    if (not isinstance(value, list) or len(value) != 2 or
            any(type(item) is not int or item <= 0 for item in value)):
        raise ValueError(f"invalid resize {field}")
    return int(value[0]), int(value[1])


def _resource_sizes(resources: dict[str, Any]) -> list[tuple[int, int]]:
    if not isinstance(resources, dict) or resources.get("schema_version") != 2:
        raise ValueError("resize-AO requires v2 AO resource census")
    if resources.get("provenance") != V2_PROVENANCE:
        raise ValueError("resize-AO requires v2 shared-scratch AO provenance")
    record = resources.get("record")
    changes = resources.get("changes")
    if not isinstance(record, dict) or not isinstance(changes, list) or not changes:
        raise ValueError("resize-AO requires nonempty v2 AO resource changes")
    result = []
    for change in changes:
        if not isinstance(change, dict):
            raise ValueError("malformed v2 AO resource change")
        width, height = change.get("target_width"), change.get("target_height")
        if type(width) is not int or type(height) is not int or width <= 0 or height <= 0:
            raise ValueError("invalid v2 AO resource target dimensions")
        result.append((width, height))
    final = (record.get("target_width"), record.get("target_height"))
    if (type(final[0]) is not int or type(final[1]) is not int or
            final[0] <= 0 or final[1] <= 0):
        raise ValueError("invalid final v2 AO resource target dimensions")
    if final != result[-1]:
        raise ValueError("final v2 AO target does not match the last resource change")
    return result


def _ordered_subsequence(values: list[tuple[int, int]], wanted: tuple[tuple[int, int], ...]) -> bool:
    index = 0
    for value in values:
        if index < len(wanted) and value == wanted[index]:
            index += 1
    return index == len(wanted)


def validate_resize_ao_evidence(log: str, resources: dict[str, Any],
                                require_fi: bool = False) -> dict[str, Any]:
    """Validate request/application order and actual v2 AO target changes."""

    rows = parse_resize_log(log)
    if not rows or rows[0].get("phase") != "armed":
        raise ValueError("resize-AO trace is missing its armed row")
    if set(rows[0]) != {"phase", "tick", "seq"}:
        raise ValueError("resize-AO armed row has unexpected fields")
    if (type(rows[0]["tick"]) is not int or rows[0]["tick"] < 0 or
            type(rows[0]["seq"]) is not int or rows[0]["seq"] <= 0):
        raise ValueError("invalid resize-AO armed row")

    expected_rows: list[dict[str, Any]] = []
    for step, (width, height) in enumerate(TARGET_SIZES):
        expected_rows.extend((
            {"phase": "request", "step": step, "requested": (width, height)},
            {"phase": "applied", "step": step, "requested": (width, height)},
        ))
    if len(rows) != 1 + len(expected_rows):
        raise ValueError("resize-AO trace is missing or duplicating request/applied rows")

    previous_tick = rows[0]["tick"]
    requests: list[dict[str, Any]] = []
    applied: list[dict[str, Any]] = []
    for row, expected in zip(rows[1:], expected_rows):
        if row.get("phase") != expected["phase"] or row.get("step") != expected["step"]:
            raise ValueError("resize-AO request/applied rows are out of order")
        if set(row) - {"phase", "step", "tick", "requested", "logical", "pixels", "hidden"}:
            raise ValueError("resize-AO row has unexpected fields")
        if type(row.get("tick")) is not int or row["tick"] <= previous_tick:
            raise ValueError("resize-AO ticks are not strictly increasing")
        previous_tick = row["tick"]
        if _int_pair(row.get("requested"), "requested") != expected["requested"]:
            raise ValueError("resize-AO requested dimensions do not match the fixed schedule")
        if row.get("hidden") != 1:
            raise ValueError("resize-AO row does not prove hidden window state")
        if expected["phase"] == "request":
            if set(row) != {"phase", "step", "tick", "requested", "hidden"}:
                raise ValueError("resize-AO request row has unexpected fields")
            requests.append(row)
        else:
            if set(row) != {"phase", "step", "tick", "requested", "logical", "pixels", "hidden"}:
                raise ValueError("resize-AO applied row has unexpected fields")
            _int_pair(row.get("logical"), "logical")
            if _int_pair(row.get("pixels"), "pixels") != expected["requested"]:
                raise ValueError("resize-AO applied pixels do not match the requested target")
            applied.append(row)

    resource_sizes = _resource_sizes(resources)
    if any(size not in TARGET_SIZES for size in resource_sizes):
        raise ValueError("v2 AO census contains an unexpected synthetic target size")
    if not _ordered_subsequence(resource_sizes, TARGET_SIZES):
        raise ValueError("v2 AO census did not observe the ordered resize target sequence")
    if resource_sizes[-1] != TARGET_SIZES[-1]:
        raise ValueError("v2 AO census did not settle on the final 1281x721 target")

    fi = {"required": bool(require_fi), "active": False}
    if require_fi:
        if "[MP6-FI] capture armed" not in log:
            raise ValueError("resize-AO run requested FI but never armed the FI path")
        bad_markers = ("[MP6-FI] CENSUS MISMATCH", "[MP6-FI] CENSUS SUM MISMATCH",
                       "walk=FAIL")
        bad = next((marker for marker in bad_markers if marker in log), None)
        if bad is not None:
            raise ValueError(f"resize-AO FI evidence contains failure marker: {bad}")
        fi["active"] = True

    return {"schedule": [list(size) for size in TARGET_SIZES],
            "requests": requests, "applied": applied,
            "resource_targets": [list(size) for size in resource_sizes],
            "fi": fi}


# Descriptive aliases for callers/tests that use the feature name rather than
# the implementation name.
parse_ao_resize_log = parse_resize_log
validate_ao_resize_evidence = validate_resize_ao_evidence
