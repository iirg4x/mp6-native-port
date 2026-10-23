"""Make the existing QA foreground view reach the native XeGTAO composite.

Only the isolated generated AO translation unit is changed. Normal runs keep
the original shader bytes; this diagnostic shows premultiplied foreground RGB
instead of the AO composite and is not a quality/performance capture.
"""

ANCHOR = '''        auto code = half ? std::string(mp6::xegtao::HalfCompositeShader) :
                           mp6::xegtao::composite_shader(canonicalDepth);'''
NEEDLE = 'return vec4f(foreground*(1.0-visibility),visibility);'
MARKER = '[AO-FOREGROUND] implementation=xegtao mode=premultiplied-rgb'


def instrument_upstream_foreground(source: str) -> str:
    if 'QA_UPSTREAM_FOREGROUND' in source:
        raise ValueError('upstream foreground probe already installed')
    if source.count(ANCHOR) != 1:
        raise ValueError('upstream composite shader selection changed')
    replacement = ANCHOR + r'''
        // QA_UPSTREAM_FOREGROUND: generated diagnostic, not production shading.
        if (std::getenv("MP6_QA_FOREGROUND_AO")) {
            const std::string needle = "return vec4f(foreground*(1.0-visibility),visibility);";
            const auto at = code.find(needle);
            if (at == std::string::npos || code.find(needle, at + needle.size()) != std::string::npos) {
                std::fprintf(stderr,"[AO-FOREGROUND-ERROR] composite contract changed\n");
                std::abort();
            }
            code.replace(at, needle.size(), "return vec4f(foreground,0);");
            std::fprintf(stderr,"[AO-FOREGROUND] implementation=xegtao mode=premultiplied-rgb\n");
        }'''
    return source.replace(ANCHOR, replacement, 1)
