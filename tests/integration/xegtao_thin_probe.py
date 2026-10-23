"""Pure source adapter for an isolated upstream thin-falloff experiment.

This never edits production shaders or performs compilation on import. A
compiled zero-compensation control must match baseline pixels before nonzero
variants are interpreted. These variants are not shipping quality policies.
"""
import math
from numbers import Real
import re


def build_probe_bridge(source: str, compensation: float) -> str:
    """Select only XeGTAO's existing fixed-constant thin-falloff branch."""
    if (isinstance(compensation, bool) or not isinstance(compensation, Real)
            or not 0 <= compensation <= .7 or not math.isfinite(compensation)):
        raise ValueError('compensation must be a finite number in 0..0.7')
    anchors = (
        '#define XE_GTAO_USE_DEFAULT_CONSTANTS 1',
        '#define XE_GTAO_DEFAULT_THIN_OBJECT_HEURISTIC 0',
        '#include "../../../third_party/xegtao/XeGTAO.h"',
        '#include "../../../third_party/xegtao/XeGTAO.hlsli"',
    )
    for anchor in anchors:
        if source.count(anchor) != 1:
            raise ValueError('expected exactly one bridge anchor: ' + anchor)
    for macro in ('XE_GTAO_USE_DEFAULT_CONSTANTS', 'XE_GTAO_DEFAULT_THIN_OBJECT_HEURISTIC'):
        if len(re.findall(r'^\s*#\s*(?:define|undef)\s+' + macro + r'\b', source, re.M)) != 1:
            raise ValueError('ambiguous bridge macro: ' + macro)
    if re.search(r'^\s*#\s*(?:define|undef)\s+XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION\b', source, re.M):
        raise ValueError('bridge already overrides thin compensation')
    for filename in ('XeGTAO.h', 'XeGTAO.hlsli'):
        if len(re.findall(r'^\s*#\s*include\s*["<][^">]*' + re.escape(filename) + r'[">]', source, re.M)) != 1:
            raise ValueError('ambiguous upstream include: ' + filename)
    positions = [source.index(anchor) for anchor in anchors]
    if positions != sorted(positions):
        raise ValueError('unexpected bridge macro/include order')
    if anchors[2] + '\n' + anchors[3] not in source:
        raise ValueError('expected adjacent upstream includes')
    value = format(float(compensation), '.9g')
    if '.' not in value and 'e' not in value:
        value += '.0'
    result = source.replace(anchors[1], anchors[1][:-1] + '1')
    result = result.replace(anchors[2], '#include "XeGTAO.h"\n'
        '#undef XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION\n'
        '#define XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION (' + value + 'f)')
    return result.replace(anchors[3], '#include "XeGTAO.hlsli"')
