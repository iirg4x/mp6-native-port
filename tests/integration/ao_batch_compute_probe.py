"""Admission and runtime evidence for the opt-in XeGTAO submission experiment."""

from tests.integration.ao_fi_pixel_policy import validate_ao_fi_pixel_policy


MARKER = '[MP6-AO] XeGTAO compute submission:'
BATCHED_ROW = MARKER + ' batched'


def batch_compute_error(args):
    if not getattr(args, 'xegtao_batch_compute', False):
        return None
    if (args.implementation != 'xegtao' or args.ao != 2 or
            args.xegtao_resolution != 'full' or args.xegtao_depth_storage != 'r32float'):
        return '--xegtao-batch-compute requires explicit XeGTAO AO2 Full/R32'
    if getattr(args, 'xegtao_init_failure', False) or getattr(args, 'ao_cycle', None) is not None:
        return '--xegtao-batch-compute cannot combine fallback or live policy cycling'
    # Those capture modes have their own source-bound comparison contracts.
    # Do not silently extend their acceptance scope with a scheduling change.
    if getattr(args, 'ao_fi_pixels', None) or getattr(args, 'deferred_capture', None):
        return '--xegtao-batch-compute is not admitted by the fixed/natural phase capture modes'
    return None


def validate_batch_compute(log, requested, quality, denoise, exit_code, reason):
    rows = [line.strip() for line in log.splitlines() if MARKER in line]
    if not requested:
        if rows:
            raise RuntimeError('unexpected XeGTAO batched submission without explicit request')
        return None
    if rows != [BATCHED_ROW]:
        raise RuntimeError('missing, duplicate or malformed XeGTAO batched submission marker')
    policy = validate_ao_fi_pixel_policy(
        log, 'xegtao', quality, denoise, exit_code, reason)
    return dict(requested=True, actual='batched', records=1, effective_policy=policy,
                scope='Compute submission attestation and conserved AO jobs; not output parity or performance acceptance')
