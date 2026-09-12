"""Report only complete ABBA groups from frozen, cool Android windows."""
import hashlib
import json
from pathlib import Path
import statistics

ROOT=Path(__file__).resolve().parents[3]
SESSION=ROOT/'build/s22-ready-memo-20260912'

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def summarize(rows):
    calls=sum(r['calls'] for r in rows)
    lookup=sum(r['lookup_ns'] for r in rows)
    return dict(phases=[r['phase'] for r in rows],
        presents_per_second=sum(r['frames']-1 for r in rows)*1e9/sum(r['last_ns']-r['first_ns'] for r in rows),
        lookup_ns_per_call=lookup/calls,
        lookup_ms_per_frame=lookup/sum(r['frames'] for r in rows)/1e6,
        hit_percent=100*sum(r['hits'] for r in rows)/calls,
        calls=calls,frames=sum(r['frames'] for r in rows))

def main():
    groups=[]; windows=[]
    for file in sorted(SESSION.glob('*/*/measurement.json')):
        proof=json.loads(file.read_text())
        for path,digest in proof['hashes'].items():
            if sha(Path(path))!=digest: raise RuntimeError('measurement input drift: '+path)
        windows.append(dict(file=str(file),sha256=sha(file),eligible=proof['eligible_for_comparison']))
        if not proof['eligible_for_comparison']: continue
        thermal=json.loads(Path(proof['thermal_file']).read_text())
        assert thermal['samples'] and all(x['status']==0 for x in thermal['samples'])
        marker=json.loads((file.parent/'marker.json').read_text())
        assert not marker['warmup'] and marker['last_tick']>=marker['board_tick']+1800
        rows=proof['rows']; byphase={r['phase']:r for r in rows}
        assert len(byphase)==len(rows)
        for first in sorted(p for p in byphase if p%4==0):
            if not all(first+i in byphase for i in range(4)): continue
            four=[byphase[first+i] for i in range(4)]
            assert [r['mode'] for r in four]==[0,1,1,0]
            assert all(r['valid'] and r['frames']==540 for r in four)
            before=summarize([four[0],four[3]])
            after=summarize(four[1:3])
            groups.append(dict(measurement=str(file),before=before,after=after,
                fps_percent=100*(after['presents_per_second']/before['presents_per_second']-1),
                lookup_time_reduction_percent=100*(1-after['lookup_ns_per_call']/before['lookup_ns_per_call']),
                lookup_saved_ms_per_frame=before['lookup_ms_per_frame']-after['lookup_ms_per_frame']))
    report=dict(candidate_adopted=False,diagnostic_only=True,groups=groups,windows=windows,
                fps_gain_established=False,
                limitations=['Same-process ABBA reduces but does not remove mobile frequency/thermal variation.',
                             'Both modes include diagnostic timing/counter overhead; this is not a release FPS benchmark.',
                             'Only complete cool settled-board groups are admitted; warmup/partial groups are excluded.'])
    if groups:
        report['median_fps_percent']=statistics.median(g['fps_percent'] for g in groups)
        report['median_lookup_time_reduction_percent']=statistics.median(g['lookup_time_reduction_percent'] for g in groups)
        report['median_lookup_saved_ms_per_frame']=statistics.median(g['lookup_saved_ms_per_frame'] for g in groups)
    (SESSION/'comparison.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({k:v for k,v in report.items() if k!='windows'},indent=2))

if __name__=='__main__': main()
