"""Instrument only isolated benchmark sources, never production math."""
import re

def instrument(source):
    start=source.index('void PSMTXIdentity(')
    end=source.index('/* ---------------------------------------------------------------------',start)
    region=source[start:end]
    pattern=re.compile(r'((void|u32|f32) (PS(?:MTX|VEC)\w+)\([^{}]+\)) \{\s*(return )?(C_\w+\([^;]+\);)\s*\}')
    matches=list(pattern.finditer(region))
    assert len(matches)==19, [m[3] for m in matches]
    names=[]
    def replace(match):
        index=len(names); names.append(match[3])
        call=match[5]
        result=''
        if match[2]!='void':
            call=match[2]+' result = '+call
            result='return result;'
        return match[1]+''' {
    unsigned long long begin=0;
    if (mp6_math_active && (++mp6_math_calls[INDEX] & 63u)==0) {
        begin=(unsigned long long)mp6_tick_phase_now();
        ++mp6_math_samples[INDEX];
    }
    CALL
    if (begin) mp6_math_ns[INDEX]+=(unsigned long long)mp6_tick_phase_now()-begin;
    RESULT
}'''.replace('INDEX',str(index)).replace('CALL',call).replace('RESULT',result)
    region=pattern.sub(replace,region)
    prefix='''#include <stdio.h>
static int mp6_math_active;
static unsigned long long mp6_math_calls[19],mp6_math_samples[19],mp6_math_ns[19];
static void mp6_math_report(void) {
    static const char* names[19]={NAMES};
    for (unsigned i=0;i<19;++i) fprintf(stderr,
        "[ENGINE-MATH] %s calls=%llu samples=%llu sampled_ns=%llu\\n",
        names[i],mp6_math_calls[i],mp6_math_samples[i],mp6_math_ns[i]);
}
'''.replace('NAMES',','.join('"'+name+'"' for name in names))
    source=prefix+source[:start]+region+source[end:]
    marker='    if (qaBoardAge>=1800 && qaBoardAge<5800) {'
    assert source.count(marker)==1
    source=source.replace(marker,'    mp6_math_active = qaBoardAge>=1800 && qaBoardAge<5800;\n'+marker)
    marker='        if (++count==4000) { fprintf(stderr,'
    assert source.count(marker)==1
    return source.replace(marker,'        if (++count==4000) { mp6_math_report(); fprintf(stderr,')
