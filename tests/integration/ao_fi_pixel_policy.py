"""Pure runtime-policy validator for AO/FI pixel captures."""

from __future__ import annotations

import re


def validate_ao_fi_pixel_policy(log, implementation, quality, denoise,
                                exit_code=0, reason='exited'):
    """Bind replay-pixel comparisons to the actual native Full/R32 policy."""
    if (implementation not in ('default', 'xegtao') or
            quality not in ('low', 'medium', 'high', 'ultra') or
            type(denoise) is not int or denoise not in (1, 2, 3)):
        raise ValueError('unsupported FI pixel policy request')
    if implementation == 'default' and (quality != 'high' or denoise != 2):
        raise ValueError('default FI pixel policy must remain High/2')
    if exit_code != 0 or reason != 'exited':
        raise RuntimeError('FI pixel policy run did not exit cleanly')
    for marker in ('[MP6-CRASH]', '[FATAL]', '[AURORA FATAL:', '[AURORA ERROR',
                   'GPUValidationError', 'uncaptured error', 'XeGTAO unavailable:',
                   '[MP6-AO] invalid ', 'using legacy', 'fallback'):
        if marker.casefold() in log.casefold():
            raise RuntimeError('FI pixel policy run reported ' + marker)
    source = 'platform default' if implementation == 'default' else 'environment override'
    expected_selection = f'[MP6-AO] requested implementation: XeGTAO ({source})'
    selection = [line.strip() for line in log.splitlines()
                 if line.startswith('[MP6-AO] requested implementation:')]
    if selection != [expected_selection]:
        raise RuntimeError('FI pixel implementation selection mismatch')
    kind = 'reference' if quality == 'high' and denoise == 2 else 'experimental'
    suffix = '' if denoise == 1 else 'es'
    expected_policy = (f'[MP6-AO] XeGTAO {kind}: full-resolution direct uint output, '
                       f'{quality.title()}, {denoise} spatial denoise pass{suffix}')
    policy_rows = [line.strip() for line in log.splitlines()
                   if line.startswith(('[MP6-AO] XeGTAO reference:',
                                       '[MP6-AO] XeGTAO experimental:'))]
    if policy_rows != [expected_policy]:
        raise RuntimeError('FI pixel effective quality/denoise policy mismatch')
    storage = [line.strip() for line in log.splitlines()
               if line.startswith('[MP6-AO] XeGTAO depth storage:')]
    if storage != ['[MP6-AO] XeGTAO depth storage: requested=r32float actual=r32float']:
        raise RuntimeError('FI pixel effective depth storage mismatch')
    counters = [line.strip() for line in log.splitlines()
                if line.startswith('[MP6-AO] encoded=')]
    match = re.fullmatch(r'\[MP6-AO\] encoded=(\d+) composited=(\d+) foliage=(\d+) xegtao=(\d+)',
                         counters[0]) if len(counters) == 1 else None
    if not match:
        raise RuntimeError('FI pixel shutdown counters missing or malformed')
    encoded, composited, _, xe = map(int, match.groups())
    if encoded <= 0 or encoded != composited or encoded != xe:
        raise RuntimeError('FI pixel XeGTAO passes are not positive and conserved')
    return dict(implementation=implementation, quality=quality, denoise_passes=denoise,
                resolution='full', depth_storage='r32float', encoded_passes=encoded)
