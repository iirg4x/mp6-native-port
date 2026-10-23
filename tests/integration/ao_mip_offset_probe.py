"""QA-only transform for the XeGTAO depth-mip sampling offset.

The transform is applied to a generated QA copy of ``ambient_occlusion.cpp``
by the board builder.  It deliberately owns no production source or runner
policy: the environment is parsed in the generated worker once, and the
constant is overridden only after the upstream preparation succeeds.
"""

from __future__ import annotations


ENV_NAME = "MP6_QA_AO_DEPTH_MIP_OFFSET"
MARKER = "MP6_QA_AO_DEPTH_MIP_OFFSET"
LOG_MARKER = "[AO-MIP-OFFSET]"
ERROR_MARKER = "[AO-MIP-OFFSET-ERROR]"

_NAMESPACE_ANCHOR = "namespace {\nusing namespace aurora;\n"
_PREPARATION_BEGIN = "    // AO_UPSTREAM_PREPARATION_BEGIN\n"
_PREPARATION_END = "    // AO_UPSTREAM_PREPARATION_END"
_INITIALIZE_ANCHOR = "                upstream = g_xegtao.initialize(ctx.device, g_xegtaoPolicy);\n"


# Keep this helper self-contained: ambient_occlusion.cpp already supplies the
# standard headers it needs, and the generated QA source must not add a
# release-facing include or API.  The lexical check rejects hexadecimal,
# whitespace, suffixes, NaN/Inf spellings, and trailing junk before strtof.
MIP_OFFSET_HELPER = r'''
/* MP6_QA_AO_DEPTH_MIP_OFFSET: QA-only depth-mip sampling override. */
struct Mp6QaAoDepthMipOffset {
    bool enabled;
    float value;
};

static bool mp6_qa_ao_mip_decimal(const char *text)
{
    if (!text || !text[0]) return false;
    const char *cursor = text;
    if (*cursor == '+' || *cursor == '-') ++cursor;
    bool mantissaDigit = false;
    while (*cursor >= '0' && *cursor <= '9') {
        mantissaDigit = true;
        ++cursor;
    }
    if (*cursor == '.') {
        ++cursor;
        while (*cursor >= '0' && *cursor <= '9') {
            mantissaDigit = true;
            ++cursor;
        }
    }
    if (!mantissaDigit) return false;
    if (*cursor == 'e' || *cursor == 'E') {
        ++cursor;
        if (*cursor == '+' || *cursor == '-') ++cursor;
        bool exponentDigit = false;
        while (*cursor >= '0' && *cursor <= '9') {
            exponentDigit = true;
            ++cursor;
        }
        if (!exponentDigit) return false;
    }
    return *cursor == '\0';
}

[[noreturn]] static void mp6_qa_ao_mip_invalid(const char *requested)
{
    std::fprintf(stderr, "[AO-MIP-OFFSET-ERROR] invalid requested=%s\n",
                 requested ? requested : "<null>");
    std::fflush(stderr);
    std::abort();
}

static const Mp6QaAoDepthMipOffset &mp6_qa_ao_mip_offset(void)
{
    static const Mp6QaAoDepthMipOffset config = [] {
        const char *requested = std::getenv("MP6_QA_AO_DEPTH_MIP_OFFSET");
        if (!requested) return Mp6QaAoDepthMipOffset{false, 0.0f};
        if (!requested[0]) mp6_qa_ao_mip_invalid(requested);
        if (!mp6_qa_ao_mip_decimal(requested)) mp6_qa_ao_mip_invalid(requested);
        char *end = nullptr;
        const float value = std::strtof(requested, &end);
        if (end == requested || *end != '\0' || !std::isfinite(value) ||
            value < 2.0f || value > 6.0f)
            mp6_qa_ao_mip_invalid(requested);
        return Mp6QaAoDepthMipOffset{true, value};
    }();
    return config;
}
'''


_PREPARATION_OVERRIDE = '''                if (upstream && qaAoDepthMipOffset.enabled) {
                    upstreamConstants->DepthMIPSamplingOffset = qaAoDepthMipOffset.value;
                    static bool qaAoDepthMipOffsetLogged = false;
                    if (!qaAoDepthMipOffsetLogged) {
                        std::fprintf(stderr, "[AO-MIP-OFFSET] requested=%.9g applied=%.9g\\n",
                                     qaAoDepthMipOffset.value,
                                     upstreamConstants->DepthMIPSamplingOffset);
                        std::fflush(stderr);
                        qaAoDepthMipOffsetLogged = true;
                    }
                }
'''


def _replace_once(source: str, anchor: str, replacement: str) -> str:
    count = source.count(anchor)
    if count != 1:
        raise ValueError(f"AO mip-offset probe anchor changed: {anchor!r} count={count}")
    return source.replace(anchor, replacement, 1)


def instrument_ao_mip_offset(source: str) -> str:
    """Inject the bounded mip-offset seam into one QA source copy.

    Every structural anchor is required exactly once and the initialization
    site must remain inside the upstream preparation span.  A second pass is
    rejected by the marker, preventing duplicate parsing, assignment, or log
    records in a generated source file.
    """

    if not isinstance(source, str):
        raise TypeError("source must be text")
    if MARKER in source:
        raise ValueError("AO mip-offset probe is already instrumented")
    for anchor in (_NAMESPACE_ANCHOR, _PREPARATION_BEGIN,
                   _PREPARATION_END, _INITIALIZE_ANCHOR):
        if source.count(anchor) != 1:
            raise ValueError(
                f"AO mip-offset probe anchor changed: {anchor!r} count={source.count(anchor)}"
            )
    begin = source.index(_PREPARATION_BEGIN)
    end = source.index(_PREPARATION_END, begin) + len(_PREPARATION_END)
    initialize = source.index(_INITIALIZE_ANCHOR)
    if not begin < initialize < end:
        raise ValueError("AO mip-offset initialization anchor escaped preparation span")

    transformed = _replace_once(
        source,
        _NAMESPACE_ANCHOR,
        _NAMESPACE_ANCHOR + MIP_OFFSET_HELPER + "\n",
    )
    transformed = _replace_once(
        transformed,
        _PREPARATION_BEGIN,
        _PREPARATION_BEGIN + "    const auto &qaAoDepthMipOffset = mp6_qa_ao_mip_offset();\n",
    )
    transformed = _replace_once(
        transformed,
        _INITIALIZE_ANCHOR,
        _INITIALIZE_ANCHOR + _PREPARATION_OVERRIDE,
    )
    return transformed


__all__ = [
    "ENV_NAME",
    "ERROR_MARKER",
    "LOG_MARKER",
    "MARKER",
    "MIP_OFFSET_HELPER",
    "instrument_ao_mip_offset",
]
