"""Strict parsing of the benchmark's completed asynchronous GPU window."""
import math
import re

def texture_upload_window(log):
    """Older reports lack this counter; absence is not a zero-byte result."""
    lines=[line for line in log.splitlines() if '[ENGINE-TEXTURES]' in line]
    if not lines:
        return None
    if len(lines)!=1:
        raise ValueError('expected one texture-upload sampling window')
    row=re.fullmatch(r'\[ENGINE-TEXTURES\] samples=(\d+) mean_upload_bytes=([\d.]+)',lines[0])
    if not row or int(row[1])!=4000:
        raise ValueError('malformed or incomplete texture-upload window')
    mean=float(row[2])
    if not math.isfinite(mean) or mean<0:
        raise ValueError('invalid texture-upload mean')
    return dict(samples=4000,mean_bytes=mean,
                scope='Samples of the last completed frame; asynchronous publication may repeat or skip frames. Not an exact total.')

def gpu_window(log):
    headers=re.findall(r'\[ENGINE-GPU\] status=(\d+) samples=(\d+) dropped=(\d+) span=([\d.]+) between=([\d.]+)',log)
    if len(headers)!=1:
        raise ValueError('expected one completed GPU window')
    status,samples,dropped=map(int,headers[0][:3])
    if status!=2 or samples!=120:
        raise ValueError(f'GPU timing window unavailable or incomplete: status={status}, samples={samples}')
    span,between=map(float,headers[0][3:])
    rows={}
    for name,average,maximum in re.findall(r'\[ENGINE-GPU-PASS\] (.+) avg=([\d.]+) max=([\d.]+)',log):
        if name in rows:
            raise ValueError(f'duplicate GPU pass: {name}')
        average,maximum=float(average),float(maximum)
        if not all(math.isfinite(v) and v>=0 for v in (average,maximum)) or maximum<average:
            raise ValueError(f'invalid GPU pass timing: {name}')
        rows[name]=dict(average_ms=average,max_ms=maximum)
    if not rows or not all(math.isfinite(v) and v>=0 for v in (span,between)) or between>span:
        raise ValueError('invalid GPU window')
    return dict(sample_count=samples,dropped_frames=dropped,span_average_ms=span,
                between_average_ms=between,passes=rows,
                scope='Final 120 completed GPU frames; not the 4000-frame CPU timing window')


def frame_profile(log):
    """CPU stages overlap across threads; callers must not sum them as frame time."""
    def fields(marker, expected):
        lines=re.findall(r'^\['+re.escape(marker)+r'\] (.*)$',log,re.M)
        if len(lines)!=1: raise ValueError(f'expected one {marker} record')
        pairs=re.findall(r'(\w+)=([\d.]+)',lines[0])
        result={key:float(value) for key,value in pairs}
        if len(pairs)!=len(result) or set(result)!=set(expected):
            raise ValueError(f'unexpected {marker} fields')
        if not all(math.isfinite(value) and value>=0 for value in result.values()):
            raise ValueError(f'invalid {marker} value')
        return result
    cpu=fields('FRAME-PROFILE',('frames','frameAdmission','stagingAdmission','encode','acquire','finish','submit','present','queueDelayMax'))
    if cpu.pop('frames')!=4000: raise ValueError('incomplete frame profile')
    work=fields('FRAME-WORK',('passes','gxDraws','sourceVertices','indices','drawElements','copies','copyBytes'))
    passes=re.findall(r'^\[FRAME-PASS\] (.*)$',log,re.M)
    if not passes: raise ValueError('missing final-frame pass inventory')
    front={}
    front_lines=re.findall(r'^\[FRAME-FRONT\] (.*)$',log,re.M)
    for line in front_lines:
        row=re.fullmatch(r'stage=(.+) inclusive=([\d.]+) exclusive=([\d.]+) calls=([\d.]+)',line)
        if not row: raise ValueError('invalid front-end profile row')
        name=row[1];inclusive,exclusive,calls=map(float,row.groups()[1:])
        if name in front or not all(math.isfinite(v) and v>=0 for v in (inclusive,exclusive,calls)):
            raise ValueError('invalid or duplicate front-end profile')
        if exclusive>inclusive+0.000002: raise ValueError('exclusive cost exceeds inclusive cost')
        front[name]=dict(inclusive_ms=inclusive,exclusive_ms=exclusive,calls_per_frame=calls)
    expected_front={'FIFO','Draw preparation','Pipeline config','Pipeline lookup',
                    'Texture resolve','Texture bindings','Uniform packing','Storage copy'}
    if front_lines and set(front) not in (expected_front,expected_front|{'Array bindings','Command recording'}):
        raise ValueError('incomplete front-end profile')
    cp={}
    cp_lines=re.findall(r'^\[FRAME-CP\] (.*)$',log,re.M)
    for line in cp_lines:
        row=re.fullmatch(r'register=(.+) writes=([\d.]+) repeats=([\d.]+)',line)
        if not row: raise ValueError('invalid CP census row')
        name=row[1];writes,repeats=map(float,row.groups()[1:])
        if name in cp or not all(math.isfinite(v) and v>=0 for v in (writes,repeats)) or repeats>writes:
            raise ValueError('invalid or duplicate CP census')
        cp[name]=dict(writes_per_frame=writes,repeated_writes_per_frame=repeats)
    if cp_lines and set(cp)!={'VCD low','VCD high','VAT A','VAT B','VAT C'}:
        raise ValueError('incomplete CP census')
    texture={}
    if '[FRAME-TEXTURE-LOADS]' in log:
        texture=fields('FRAME-TEXTURE-LOADS',('texture','identical','palette','identicalPalette'))
        if texture['identical']>texture['texture'] or texture['identicalPalette']>texture['palette']:
            raise ValueError('texture repeats exceed loads')
    return dict(cpu_average_ms=cpu,work_average_per_frame=work,final_frame_passes=passes,
        front_end_average=front,cp_register_census=cp,texture_load_census=texture,
        scope='4000 instrumented desktop frames. CPU stages overlap; admission includes waiting. Scene-pass count excludes custom AO passes and final presentation. Vertices are submitted references, not unique mesh vertices or hardware invocations.')
