"""Opt-in, QA-only transforms for actual FI input/output matrix attribution.

Wire all three transforms: hsfdraw, its included foliage header, and frame_interp.
This module does not write sources or run the game. Every seam is exact-count
checked, so source drift aborts QA generation rather than producing partial proof.

MP6_QA_AO_FI_MATRIX=1 enables stderr JSONL prefixed [AO-FI-MATRIX]. Set
MP6_QA_AO_FI_MATRIX_FROM/TO to an inclusive VI-tick window (at most 120 ticks).
LIMIT defaults to 256 records, hard capped at 1024. Defaults are ticks 0..120.
The preceding tick is captured for predecessor evidence but not logged.
Only b01_m002 is attributed, by authored root, never by assumed runtime slot.
All its objects receive hints so a selected flower key can identify a different
previous object. Only flower_a/fa_c2/a_ff1/a_ha1/a_ha2 are normally emitted.
Group-domain counts are legacy model/camera/generation census diagnostics and do
not drive row admission: auxiliary sub is 0x8000..0xBFFF, whereas immediate
main draws use 0xFFFF. Additional rows are admitted only for a valid authored
per-object group matched with fi_key_same_group in the current or predecessor
stream. The row limit is global across real and replay records.

The probe reads captured/replayed bytes; it never changes keys, pairing, matrices,
camera state, render commands, or admission decisions. Diagnostics cost time.
JSON formatting goes to a private fixed 4 MiB buffer, never stderr while the
sampling window is active. A whole overflowing row is rolled back and capture
stops. Committed rows flush after TO (when the next probe hook is reached), or
at normal process exit. Abnormal termination can lose the buffered diagnostics.
An [AO-FI-MATRIX-STATUS] JSON summary reports flush reason, bytes, and drops.
Replay records are emitted only after a replay actually reaches end_frame, not
from builds subsequently rejected by the deadline gate. No off/on-FI comparison
can be inferred when FI capture is disabled: there is then no retained stream.
"""

from __future__ import annotations


def _replace(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found != count:
        raise ValueError(f"AO FI probe seam expected {count}, found {found}: {old[:90]!r}")
    return text.replace(old, new)


def _replace_in_region(
    text: str, start: str, end: str, old: str, new: str
) -> str:
    """Replace one seam inside one current-source function region.

    P2 split replay into a plan path and a full-builder path, so a reset line
    is intentionally present once in each function.  Keep each function's
    seam strict rather than relaxing the global count and accidentally
    instrumenting an unrelated reset.
    """
    if text.count(start) != 1:
        raise ValueError(
            f"AO FI probe seam expected 1, found {text.count(start)}: {start[:90]!r}"
        )
    if text.count(end) != 1:
        raise ValueError(
            f"AO FI probe seam expected 1, found {text.count(end)}: {end[:90]!r}"
        )
    begin = text.index(start)
    finish = text.index(end, begin + len(start))
    if finish <= begin:
        raise ValueError(f"AO FI probe seam has reversed region: {start[:90]!r}")
    region = text[begin:finish]
    return text[:begin] + _replace(region, old, new) + text[finish:]


HSF_HELPER = r'''
/* QA_AO_FI_MATRIX_HSF: generated diagnostics only. */
extern int qa_ao_fi_enabled(void);
extern void qa_ao_fi_hint_clear(void);
extern void qa_ao_fi_hint(int, int, const char *, const float *, const float *);
static void qa_ao_fi_draw_hint(const HU3D_DRAW_OBJ *draw)
{
    qa_ao_fi_hint_clear();
    if (!qa_ao_fi_enabled() || !draw || !draw->model || !draw->object ||
        (draw->model->attr & HU3D_ATTR_HOOKFUNC)) return;
    const HSF_DATA *hsf=draw->model->hsf;
    if (!hsf || !hsf->root || !hsf->root->name ||
        strcmp(hsf->root->name,"b01_m002") || Hu3DCameraNo<0 ||
        Hu3DCameraNo>=HU3D_CAM_MAX) return;
    const HU3D_CAMERA *c=&Hu3DCamera[Hu3DCameraNo];
    const float camera[13]={c->pos.x,c->pos.y,c->pos.z,
        c->target.x,c->target.y,c->target.z,c->up.x,c->up.y,c->up.z,
        c->fov,c->near,c->far,c->aspect};
    qa_ao_fi_hint((int)(draw->model-Hu3DData),
        (int)(draw->object-hsf->object),draw->object->name,
        &Hu3DCameraMtx[0][0],camera);
}
'''


def instrument_hsfdraw(text: str) -> str:
    """Transform the patched HSF source before its existing QA transforms."""
    if "QA_AO_FI_MATRIX_HSF" in text:
        raise ValueError("AO FI HSF probe already installed")
    text = _replace(text, '#include "mp6_ao_foliage_draw.h"',
                    HSF_HELPER + '\n#include "mp6_ao_foliage_draw.h"')
    return _replace(text, "GXLoadPosMtxImm(drawObj->matrix, GX_PNMTX0);",
                    "qa_ao_fi_draw_hint(drawObj);\n"
                    "    GXLoadPosMtxImm(drawObj->matrix, GX_PNMTX0);", 2)


def instrument_foliage(text: str) -> str:
    """The generated foliage header must accompany instrument_hsfdraw()."""
    if "QA_AO_FI_MATRIX_FOLIAGE" in text:
        raise ValueError("AO FI foliage probe already installed")
    return _replace(text, "    GXLoadPosMtxImm(draw->matrix,GX_PNMTX0);",
                    "    /* QA_AO_FI_MATRIX_FOLIAGE */\n"
                    "    qa_ao_fi_draw_hint(draw);\n"
                    "    GXLoadPosMtxImm(draw->matrix,GX_PNMTX0);")


BUFFER = r'''
/* QA-private text storage: never change stderr's process-wide buffering. */
#include <stdarg.h>
#include <limits.h>
#ifndef QA_AO_FI_BUFFER_CAP
#define QA_AO_FI_BUFFER_CAP (4u*1024u*1024u)
#endif
static char qaAoFiBuffer[QA_AO_FI_BUFFER_CAP];
static size_t qaAoFiBufferUsed, qaAoFiRowStart;
static unsigned qaAoFiBufferRows, qaAoFiDroppedRows;
static int qaAoFiRowOpen, qaAoFiBufferOverflow, qaAoFiBufferClosed;
static int qa_ao_fi_row_begin(void)
{
    if (qaAoFiBufferClosed || qaAoFiBufferOverflow || qaAoFiRowOpen) return 0;
    qaAoFiRowStart=qaAoFiBufferUsed; qaAoFiRowOpen=1;
    return 1;
}
static int qa_ao_fi_row_end(void)
{
    if (!qaAoFiRowOpen) return 0;
    qaAoFiRowOpen=0;
    if (qaAoFiBufferOverflow) {
        qaAoFiBufferUsed=qaAoFiRowStart;
        qaAoFiDroppedRows++;
        return 0;
    }
    qaAoFiBufferRows++;
    return 1;
}
static void qa_ao_fi_fprintf(FILE *unused, const char *format, ...)
{
    (void)unused;
    if (!qaAoFiRowOpen || qaAoFiBufferOverflow || qaAoFiBufferClosed) return;
    size_t available=sizeof(qaAoFiBuffer)-qaAoFiBufferUsed;
    if (!available) { qaAoFiBufferOverflow=1; return; }
    va_list args;
    va_start(args,format);
    int length=vsnprintf(qaAoFiBuffer+qaAoFiBufferUsed,available,format,args);
    va_end(args);
    if (length<0 || (size_t)length>=available) { qaAoFiBufferOverflow=1; return; }
    qaAoFiBufferUsed+=(size_t)length;
}
static void qa_ao_fi_fputs(const char *text, FILE *unused)
{
    (void)unused;
    if (!qaAoFiRowOpen || qaAoFiBufferOverflow || qaAoFiBufferClosed) return;
    size_t length=strlen(text);
    if (length>sizeof(qaAoFiBuffer)-qaAoFiBufferUsed) { qaAoFiBufferOverflow=1; return; }
    memcpy(qaAoFiBuffer+qaAoFiBufferUsed,text,length);
    qaAoFiBufferUsed+=length;
}
static void qa_ao_fi_fputc(int c, FILE *unused)
{
    (void)unused;
    if (!qaAoFiRowOpen || qaAoFiBufferOverflow || qaAoFiBufferClosed) return;
    if (qaAoFiBufferUsed==sizeof(qaAoFiBuffer)) { qaAoFiBufferOverflow=1; return; }
    qaAoFiBuffer[qaAoFiBufferUsed++]=(char)c;
}
static void qa_ao_fi_flush(const char *reason, long tick)
{
    if (qaAoFiBufferClosed) return;
    if (qaAoFiRowOpen) {
        qaAoFiBufferUsed=qaAoFiRowStart; qaAoFiRowOpen=0; qaAoFiDroppedRows++;
    }
    qaAoFiBufferClosed=1;
    size_t bytes=qaAoFiBufferUsed;
    size_t written=bytes?fwrite(qaAoFiBuffer,1,bytes,stderr):0;
    fprintf(stderr,"[AO-FI-MATRIX-STATUS] {\"flush\":\"%s\",\"viTick\":%ld,"
        "\"records\":%u,\"bytes\":%zu,\"writtenBytes\":%zu,\"droppedRows\":%u,"
        "\"overflow\":%d}\n",reason,tick,qaAoFiBufferRows,bytes,written,
        qaAoFiDroppedRows,qaAoFiBufferOverflow);
    qaAoFiBufferUsed=0;
}
'''


TYPES = r'''
/* QA_AO_FI_MATRIX: generated diagnostics only, bounded host-side metadata. */
#define QA_AO_FI_HINT_MAX 256u
typedef struct {
    int valid, model, object, selected;
    uint32_t index;
    char name[64];
    float cameraView[12], camera[13];
} QaAoFiHint;
typedef struct {
    uint32_t index, outputOffset;
    const char *reason;
    int hasOutput, submitted;
    uint8_t output[48];
} QaAoFiReplay;
static QaAoFiHint qaAoFiPending;
static QaAoFiReplay qaAoFiReplay[QA_AO_FI_HINT_MAX];
static unsigned qaAoFiReplayCount;
static int qaAoFiConfig=-1;
static long qaAoFiFrom, qaAoFiTo, qaAoFiLimit, qaAoFiEmitted;
void qa_ao_fi_hint_clear(void) { qaAoFiPending.valid=0; }
static void qa_ao_fi_shutdown(void) { qa_ao_fi_flush("shutdown",mp6_tick_count); }
static long qa_ao_fi_env_long(const char *name, long fallback)
{
    const char *s=getenv(name);
    char *end;
    if (!s || !*s) return fallback;
    long value=strtol(s,&end,10);
    return *end ? -1 : value;
}
int qa_ao_fi_enabled(void)
{
    if (qaAoFiConfig<0) {
        const char *e=getenv("MP6_QA_AO_FI_MATRIX");
        qaAoFiFrom=qa_ao_fi_env_long("MP6_QA_AO_FI_MATRIX_FROM",0);
        qaAoFiTo=qa_ao_fi_env_long("MP6_QA_AO_FI_MATRIX_TO",
            qaAoFiFrom>=0 && qaAoFiFrom<=LONG_MAX-120 ? qaAoFiFrom+120 : -1);
        qaAoFiLimit=qa_ao_fi_env_long("MP6_QA_AO_FI_MATRIX_LIMIT",256);
        qaAoFiConfig=e && !strcmp(e,"1") && qaAoFiFrom>=0 &&
            qaAoFiTo>=qaAoFiFrom && qaAoFiTo-qaAoFiFrom<=120 &&
            qaAoFiLimit>0 && qaAoFiLimit<=1024;
        if (qaAoFiConfig && atexit(qa_ao_fi_shutdown)!=0) qaAoFiConfig=0;
    }
    if (qaAoFiConfig && mp6_tick_count>qaAoFiTo)
        qa_ao_fi_flush("after-range",mp6_tick_count);
    return qaAoFiConfig && !qaAoFiBufferClosed && !qaAoFiBufferOverflow &&
        qaAoFiEmitted<qaAoFiLimit &&
        mp6_tick_count>=qaAoFiFrom-1 && mp6_tick_count<=qaAoFiTo;
}
void qa_ao_fi_hint(int model, int object, const char *name,
                   const float *view, const float *camera)
{
    memset(&qaAoFiPending,0,sizeof(qaAoFiPending));
    if (!qa_ao_fi_enabled() || !name || model<0 || object<0) return;
    QaAoFiHint *h=&qaAoFiPending;
    h->valid=1; h->model=model; h->object=object;
    snprintf(h->name,sizeof(h->name),"%s",name);
    h->selected=!strcmp(name,"flower_a") || !strcmp(name,"fa_c2") ||
        !strcmp(name,"a_ff1") || !strcmp(name,"a_ha1") || !strcmp(name,"a_ha2");
    memcpy(h->cameraView,view,sizeof(h->cameraView));
    memcpy(h->camera,camera,sizeof(h->camera));
}
'''


HELPERS = r'''
/* Row admission keeps the five named flowers and adds only live authored
 * object groups that carry an AO copy on either side of the pair.  Do not use
 * qa_ao_fi_group_domains here: those counters intentionally retain the legacy
 * whole-model census semantics for diagnostics. */
static int qa_ao_fi_is_authored_key(const FiPosKey *key)
{
    return key && key->valid && key->sub>=0x4000u && key->sub<0xC000u;
}
static int qa_ao_fi_is_aux_key(const FiPosKey *key)
{
    return qa_ao_fi_is_authored_key(key) &&
        key->sub>=0x8000u && key->sub<0xC000u;
}
static int qa_ao_fi_has_aux_group(const FiPosKey *key,
                                  const FiPosKey *keys, uint32_t count)
{
    if (!qa_ao_fi_is_authored_key(key)) return 0;
    for (uint32_t j=0;j<count;j++)
        if (qa_ao_fi_is_aux_key(&keys[j]) && fi_key_same_group(key,&keys[j]))
            return 1;
    return 0;
}
static int qa_ao_fi_hint_matches_group(const QaAoFiHint *hint,
                                       const FiPosKey *key)
{
    FiPosKey authored;
    if (!hint || !qa_ao_fi_is_authored_key(key) || hint->object<0 ||
        hint->object>=0x4000) return 0;
    authored=*key;
    authored.model=(int16_t)hint->model;
    authored.sub=(uint16_t)(0x4000u+(unsigned)hint->object);
    authored.ordinal=0;
    return fi_key_same_group(key,&authored);
}
static int qa_ao_fi_select(const QaAoFiHint *hint, const FiPosKey *key,
                           const FiPosKey *currentKeys, uint32_t currentCount,
                           const FiPosKey *previousKeys, uint32_t previousCount)
{
    if (!hint) return 0;
    if (hint->selected) return 1;
    if (!qa_ao_fi_hint_matches_group(hint,key)) return 0;
    return qa_ao_fi_has_aux_group(key,currentKeys,currentCount) ||
        qa_ao_fi_has_aux_group(key,previousKeys,previousCount);
}

static const QaAoFiHint *qa_ao_fi_find(const FiStream *s, int32_t index)
{
    if (s && index>=0) for (unsigned j=0;j<s->qaHintCount;j++)
        if (s->qaHints[j].index==(uint32_t)index) return &s->qaHints[j];
    return NULL;
}
static void qa_ao_fi_record(const FiStream *cur, const FiStream *prev,
                            uint32_t index,
                            const char *reason, const uint8_t *output, uint32_t outputOffset)
{
    if (!qa_ao_fi_enabled()) return;
    const QaAoFiHint *h=qa_ao_fi_find(cur,(int32_t)index);
    const FiPosKey *key=index<cur->keyCount ? &cur->posKey[index] : NULL;
    const FiPosKey *previousKeys=prev && prev->walkOk ? prev->posKey : NULL;
    uint32_t previousCount=prev && prev->walkOk ? prev->keyCount : 0;
    if (!key || !qa_ao_fi_select(h,key,cur->posKey,cur->keyCount,
                                 previousKeys,previousCount) ||
        qaAoFiReplayCount>=QA_AO_FI_HINT_MAX) return;
    QaAoFiReplay *r=&qaAoFiReplay[qaAoFiReplayCount++];
    r->index=index; r->reason=reason; r->hasOutput=output!=NULL;
    r->outputOffset=outputOffset; r->submitted=0;
    if (output) memcpy(r->output,output,48);
}
static void qa_ao_fi_submitted(uint32_t offset, uint32_t size)
{
    for (unsigned j=0;j<qaAoFiReplayCount;j++) {
        QaAoFiReplay *r=&qaAoFiReplay[j];
        if (r->hasOutput && r->outputOffset>=offset &&
            r->outputOffset-offset<=size && size-(r->outputOffset-offset)>=48)
            r->submitted=1;
    }
}
static void qa_ao_fi_group_domains(const FiPosKey *key, const FiPosKey *keys,
                                   uint32_t count, uint32_t *mainCount,
                                   uint32_t *auxCount)
{
    *mainCount=*auxCount=0;
    if (!key->valid) return;
    for (uint32_t j=0;j<count;j++) {
        const FiPosKey *other=&keys[j];
        if (!other->valid || other->model!=key->model ||
            other->camera!=key->camera || other->generation!=key->generation) continue;
        /* Immediate main is 0xFFFF, NOT auxiliary despite its high bit. */
        if (other->sub>=0x8000u && other->sub<0xC000u) (*auxCount)++;
        else (*mainCount)++;
    }
}
static void qa_ao_fi_json_string(const char *s)
{
    if (!s) { fputs("null",stderr); return; }
    fputc('"',stderr);
    for (;*s;s++) {
        unsigned char c=(unsigned char)*s;
        if (c=='"' || c=='\\') { fputc('\\',stderr); fputc(c,stderr); }
        else if (c<32 || c>=127) fprintf(stderr,"\\u%04x",c);
        else fputc(c,stderr);
    }
    fputc('"',stderr);
}
static void qa_ao_fi_json_floats(const float *v, unsigned count)
{
    if (!v) { fputs("null",stderr); return; }
    fputc('[',stderr);
    for (unsigned n=0;n<count;n++) {
        if (n) fputc(',',stderr);
        if (isfinite(v[n])) fprintf(stderr,"%.9g",(double)v[n]);
        else fputs("null",stderr);
    }
    fputc(']',stderr);
}
static void qa_ao_fi_json_matrix(const uint8_t *bytes)
{
    if (!bytes) { fputs("null",stderr); return; }
    float m[12];
    for (unsigned n=0;n<12;n++) m[n]=rd_bef32(bytes+4*n);
    qa_ao_fi_json_floats(m,12);
}
static void qa_ao_fi_json_bits(const uint8_t *bytes)
{
    if (!bytes) { fputs("null",stderr); return; }
    fputc('[',stderr);
    for (unsigned n=0;n<12;n++) fprintf(stderr,"%s%u",n?",":"",rd_be32(bytes+4*n));
    fputc(']',stderr);
}
static void qa_ao_fi_json_key(const FiPosKey *key)
{
    if (!key) { fputs("null",stderr); return; }
    fprintf(stderr,"{\"valid\":%u,\"camera\":%d,\"model\":%d,\"generation\":%u,"
        "\"sub\":%u,\"ordinal\":%u}",(unsigned)key->valid,(int)key->camera,
        (int)key->model,key->generation,(unsigned)key->sub,(unsigned)key->ordinal);
}
static void qa_ao_fi_emit_one(const FiStream *cur, const FiStream *prev,
                            uint32_t i, int replay, double alpha, int interp,
                            const char *rewriteReason, const uint8_t *output, int submitted)
{
    if (!qa_ao_fi_enabled() || cur->qaViTick<qaAoFiFrom ||
        cur->qaViTick>qaAoFiTo || !cur->walkOk || i>=cur->posCount) return;
    const QaAoFiHint *h=qa_ao_fi_find(cur,(int32_t)i);
    const FiPosKey *key=&cur->posKey[i];
    const FiPosKey *previousKeys=prev && prev->walkOk ? prev->posKey : NULL;
    uint32_t previousCount=prev && prev->walkOk ? prev->keyCount : 0;
    if (!qa_ao_fi_select(h,key,cur->posKey,cur->keyCount,
                         previousKeys,previousCount)) return;
    if (!qa_ao_fi_row_begin()) return;
    uint32_t gc=0,oc=0,gp=0,op=0;
    uint32_t mainCur=0,auxCur=0,mainPrev=0,auxPrev=0;
    int32_t candidate=-1, paired=cur->prevPos[i];
    fi_group_census(key,cur->posKey,cur->keyCount,&gc,&oc);
    qa_ao_fi_group_domains(key,cur->posKey,cur->keyCount,&mainCur,&auxCur);
    if (prev && prev->walkOk) {
        fi_group_census(key,prev->posKey,prev->keyCount,&gp,&op);
        qa_ao_fi_group_domains(key,prev->posKey,prev->keyCount,&mainPrev,&auxPrev);
        for (uint32_t j=0;j<prev->keyCount;j++)
            if (fi_key_equal(key,&prev->posKey[j])) { candidate=(int32_t)j; break; }
    }
    const char *pairReason=!prev || !prev->walkOk ? "no-predecessor" :
        !key->valid ? "no-key" : !gp ? "no-previous-group" :
        oc!=1 || op!=1 ? "nonunique-or-missing-key" : gc!=gp ? "group-count-change" :
        paired<0 ? "unmatched" : "paired";
    const QaAoFiHint *ph=qa_ao_fi_find(prev,candidate);
    const uint8_t *current=cur->data+cur->posOff[i];
    const uint8_t *previous=prev && prev->walkOk && candidate>=0 &&
        (uint32_t)candidate<prev->posCount ? prev->data+prev->posOff[candidate] : NULL;
    int foliage=0;
    for (uint32_t j=0;j<cur->aoCount && cur->ao[j].offset<=cur->posOff[i]-5;j++) {
        if (cur->ao[j].kind==3) foliage=1;
        else if (cur->ao[j].kind==4) foliage=0;
    }
    fprintf(stderr,"[AO-FI-MATRIX] {\"event\":\"%s\",\"assetRoot\":\"b01_m002\",\"viTick\":%ld,\"presentViTick\":%ld,"
        "\"seal\":%ld,\"previousSeal\":%ld,\"previousViTick\":%ld,\"alpha\":%.9g,"
        "\"interpolationEnabled\":%d,\"submitted\":%d,\"index\":%u,\"pairedIndex\":%d,\"candidateIndex\":%d,"
        "\"coveragePass\":%d,\"groupCurrent\":%u,\"groupPrevious\":%u,"
        "\"groupMainCurrent\":%u,\"groupAuxCurrent\":%u,\"groupMainPrevious\":%u,\"groupAuxPrevious\":%u,"
        "\"keyCurrentCount\":%u,\"keyPreviousCount\":%u,\"hintOverflow\":%u,"
        "\"previousHintOverflow\":%u,\"objectModel\":%d,\"contextMismatch\":%d,\"object\":%d,\"name\":",
        replay?"replay":"real",cur->qaViTick,mp6_tick_count,cur->tick,prev?prev->tick:-1L,
        prev?prev->qaViTick:-1L,alpha,interp,submitted,i,paired,candidate,foliage,gc,gp,
        mainCur,auxCur,mainPrev,auxPrev,oc,op,
        cur->qaHintOverflow,prev?prev->qaHintOverflow:0,h->model,
        key->valid && key->model!=h->model,h->object);
    qa_ao_fi_json_string(h->name);
    fprintf(stderr,",\"previousObject\":%d,\"previousName\":",ph?ph->object:-1);
    qa_ao_fi_json_string(ph?ph->name:NULL);
    fputs(",\"objectMismatch\":",stderr);
    fputs(ph?(h->object!=ph->object?"true":"false"):"null",stderr);
    fputs(",\"pairReason\":",stderr); qa_ao_fi_json_string(pairReason);
    fputs(",\"rewriteReason\":",stderr); qa_ao_fi_json_string(rewriteReason);
    fputs(",\"key\":",stderr); qa_ao_fi_json_key(key);
    fputs(",\"previousKey\":",stderr);
    qa_ao_fi_json_key(prev && candidate>=0?&prev->posKey[candidate]:NULL);
    fputs(",\"previous\":",stderr); qa_ao_fi_json_matrix(previous);
    fputs(",\"current\":",stderr); qa_ao_fi_json_matrix(current);
    fputs(",\"output\":",stderr); qa_ao_fi_json_matrix(output);
    fputs(",\"previousBits\":",stderr); qa_ao_fi_json_bits(previous);
    fputs(",\"currentBits\":",stderr); qa_ao_fi_json_bits(current);
    fputs(",\"outputBits\":",stderr); qa_ao_fi_json_bits(output);
    fputs(",\"cameraCurrent\":",stderr); qa_ao_fi_json_floats(h->camera,13);
    fputs(",\"cameraPrevious\":",stderr); qa_ao_fi_json_floats(ph?ph->camera:NULL,13);
    fputs(",\"cameraViewCurrent\":",stderr); qa_ao_fi_json_floats(h->cameraView,12);
    fputs(",\"cameraViewPrevious\":",stderr); qa_ao_fi_json_floats(ph?ph->cameraView:NULL,12);
    fputs("}\n",stderr);
    if (qa_ao_fi_row_end()) qaAoFiEmitted++;
}
static void qa_ao_fi_emit_real(const FiStream *cur, const FiStream *prev)
{
    if (!qa_ao_fi_enabled() || !cur->walkOk) return;
    for (unsigned j=0;j<cur->qaHintCount;j++) {
        uint32_t i=cur->qaHints[j].index;
        if (i<cur->posCount) qa_ao_fi_emit_one(cur,prev,i,0,0.0,0,"real-verbatim",
                                               cur->data+cur->posOff[i],1);
    }
}
static void qa_ao_fi_emit_replay(const FiStream *cur, const FiStream *prev,
                                double alpha, int interp)
{
    for (unsigned j=0;j<qaAoFiReplayCount;j++) {
        const QaAoFiReplay *r=&qaAoFiReplay[j];
        qa_ao_fi_emit_one(cur,prev,r->index,1,alpha,interp,r->reason,
                           r->submitted?r->output:NULL,r->submitted);
    }
}
'''


def instrument_frame_interp(text: str) -> str:
    """Add observation-only metadata to the production FI translation unit."""
    if "QA_AO_FI_MATRIX:" in text:
        raise ValueError("AO FI matrix probe already installed")
    text = _replace(text, "} FiPosKey;", "} FiPosKey;\n" + BUFFER + TYPES)
    text = _replace(text, "    long tick;           /* seal ordinal (diagnostics) */",
        "    QaAoFiHint qaHints[QA_AO_FI_HINT_MAX];\n"
        "    unsigned qaHintCount, qaHintOverflow;\n    long qaViTick;\n"
        "    long tick;           /* seal ordinal (diagnostics) */")
    text = _replace(text, "    s->keyCount = 0;", "    s->keyCount = 0;\n"
        "    s->qaHintCount=s->qaHintOverflow=0; s->qaViTick=mp6_tick_count;")
    text = _replace(text, "void mp6_fi_stream_note_pos_mtx(void)\n{",
        "void mp6_fi_stream_note_pos_mtx(void)\n{\n"
        "    int qaHasHint=qaAoFiPending.valid;\n    qaAoFiPending.valid=0;")
    text = _replace(text, "    s->posKey[s->keyCount++] = key;",
        "    if (qaHasHint && qa_ao_fi_enabled()) {\n"
        "        if (s->qaHintCount<QA_AO_FI_HINT_MAX) {\n"
        "            QaAoFiHint *h=&s->qaHints[s->qaHintCount++];\n"
        "            *h=qaAoFiPending; h->valid=1; h->index=s->keyCount;\n        }\n"
        "        else s->qaHintOverflow++;\n    }\n"
        "    s->posKey[s->keyCount++] = key;")
    # The helper is after key census definitions and before both frame hooks.
    buffered_helpers = (HELPERS.replace("fprintf(", "qa_ao_fi_fprintf(")
                        .replace("fputs(", "qa_ao_fi_fputs(")
                        .replace("fputc(", "qa_ao_fi_fputc("))
    text = _replace(text, "static void fi_set_active(int on)",
                    buffered_helpers + "\nstatic void fi_set_active(int on)")
    text = _replace(text, "    fi_pair_stream(s, s_prev >= 0 ? &s_streams[s_prev] : NULL);",
        "    fi_pair_stream(s, s_prev >= 0 ? &s_streams[s_prev] : NULL);\n"
        "    qa_ao_fi_emit_real(s,s_prev>=0?&s_streams[s_prev]:NULL);")
    # P2 has two replay reset sites: a cached plan replay and the fallback
    # full builder.  Reset the per-present diagnostic records at both sites so
    # plan-path rows describe this replay only; normal rendering is unchanged
    # because the generated helpers remain gated by qa_ao_fi_enabled().
    text = _replace_in_region(
        text,
        "static void fi_replay_plan_debug_reset(void)",
        "static uint32_t fi_replay_plan_replay(",
        "    s_dbgScaleHold = 0;",
        "    qaAoFiReplayCount=0;\n    s_dbgScaleHold = 0;",
    )
    text = _replace_in_region(
        text,
        "static uint32_t fi_build_replay(",
        "int mp6_fi_idle_present(",
        "    s_dbgScaleHold = 0;",
        "    qaAoFiReplayCount=0;\n    s_dbgScaleHold = 0;",
    )
    text = _replace(text, "        if (emitCopyClear) s_dbgCopyClear++;",
        "        if (drop && streamPosIdx!=UINT32_MAX)\n"
        "            qa_ao_fi_record(cur,prev,streamPosIdx,\"dropped-offscreen\",NULL,out+5);\n"
        "        if (emitCopyClear) s_dbgCopyClear++;")
    text = _replace(text, "        s_dbgPosSeen++;",
        "        const char *qaReason=!interp?\"interpolation-disabled\":\n"
        "            !paired?\"unpaired\":!camStable?\"camera-cut\":\"pending\";\n"
        "        s_dbgPosSeen++;")
    text = _replace(text, "            if (state == FI_PAIR_EQUAL) s_dbgVerbatimEq++;",
        "            if (state == FI_PAIR_EQUAL) { s_dbgVerbatimEq++; qaReason=\"byte-equal\"; }")
    text = _replace(text, "                    s_dbgSnapDecompose++;",
        "                    s_dbgSnapDecompose++; qaReason=\"decompose-refused\";")
    text = _replace(text, "                        s_dbgSnapGate++;",
        "                        s_dbgSnapGate++; qaReason=\"motion-gate\";")
    text = _replace(text, "                        rewrittenResult = 1;",
        "                        rewrittenResult = 1;\n"
        "                        qaReason=holdScale?\"rewritten-scale-held\":\"rewritten\";")
    text = _replace(text, "        if (!rewrittenResult && slot < 10) {",
        "        qa_ao_fi_record(cur,prev,streamPosIdx,qaReason,\n"
        "            rewrittenResult?payload:cur->data+c.offset+5,out+5);\n"
        "        if (!rewrittenResult && slot < 10) {")
    text = _replace(text,
        "            if (marker->offset > offset && !skipFoliage)\n"
        "                aurora_gx_submit_raw(s_replayBuf + offset, marker->offset - offset);",
        "            if (marker->offset > offset && !skipFoliage) {\n"
        "                aurora_gx_submit_raw(s_replayBuf + offset, marker->offset - offset);\n"
        "                qa_ao_fi_submitted(offset,marker->offset-offset);\n            }")
    text = _replace(text,
        "        if (len > offset && !skipFoliage) aurora_gx_submit_raw(s_replayBuf + offset, len - offset);",
        "        if (len > offset && !skipFoliage) {\n"
        "            aurora_gx_submit_raw(s_replayBuf + offset, len - offset);\n"
        "            qa_ao_fi_submitted(offset,len-offset);\n        }")
    text = _replace(text, "    aurora_end_frame();\n    fi_def_restore();\n    /* MP6_FRAME_DUMP",
        "    aurora_end_frame();\n    fi_def_restore();\n"
        "    qa_ao_fi_emit_replay(cur,prev,alpha,interp);\n    /* MP6_FRAME_DUMP")
    return text
