"""Pure parsing and balanced ABBA grouping; no device or filesystem mutations."""
import re
PATTERN = re.compile(r'\[MP6-BUNDLE-ABBA\] phase=(\d+) mode=(\d+) valid=(\d+) frames=(\d+) draws=(\d+) hits=(\d+) builds=(\d+) encode_ns=(\d+) first_ns=(\d+) last_ns=(\d+)')
KEYS = ('phase','mode','valid','frames','draws','hits','builds','encode_ns','first_ns','last_ns')
def parse(text):
    result = []
    seen = set()
    for match in PATTERN.finditer(text):
        row = dict(zip(KEYS,map(int,match.groups())))
        if row['phase'] in seen: raise ValueError('duplicate phase; renderer may have restarted')
        seen.add(row['phase'])
        if row['mode'] != int(row['phase']%4 in (1,2)): raise ValueError('invalid ABBA order')
        if row['hits'] + row['builds'] > row['draws']: raise ValueError('invalid draw accounting')
        if not row['mode'] and (row['hits'] or row['builds']): raise ValueError('baseline used bundles')
        result.append(row)
    return result

def groups(rows, minimum_phase, maximum_phase):
    usable = {r['phase']:r for r in rows if minimum_phase <= r['phase'] <= maximum_phase and
              r['valid'] and r['frames'] == 216 and r['draws'] > 0 and r['last_ns'] > r['first_ns'] > 0}
    result = []
    for base in sorted(usable):
        if base%4 or not all(base+i in usable for i in range(4)): continue
        phases = [usable[base+i] for i in range(4)]
        if not all(phases[i]['last_ns'] < phases[i+1]['first_ns'] for i in range(3)): continue
        def aggregate(selected):
            intervals = sum(p['frames']-1 for p in selected)
            duration = sum(p['last_ns']-p['first_ns'] for p in selected)
            frames = sum(p['frames'] for p in selected)
            draws = sum(p['draws'] for p in selected)
            return dict(presents_per_second=intervals*1e9/duration,
                encode_ms_per_frame=sum(p['encode_ns'] for p in selected)/frames/1e6,
                draws_per_frame=draws/frames,
                hit_percent=100*sum(p['hits'] for p in selected)/draws)
        baseline = aggregate([phases[0],phases[3]])
        candidate = aggregate([phases[1],phases[2]])
        result.append(dict(first_phase=base,baseline=baseline,candidate=candidate,
            presents_percent_change=100*(candidate['presents_per_second']/baseline['presents_per_second']-1),
            encode_percent_change=100*(candidate['encode_ms_per_frame']/baseline['encode_ms_per_frame']-1)))
    return result
