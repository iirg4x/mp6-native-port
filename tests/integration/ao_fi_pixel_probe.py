"""Opt-in, QA-only FI phase forcing and submitted-pixel attribution.

This module is deliberately applied after ``ao_fi_matrix_probe``.  The matrix
probe owns stream identity, authored-object selection, and replay submission
tracking; this layer only chooses a bounded alpha phase and hands the already
submitted bytes plus metadata to the frame-dump boundary.  It never changes
production admission or pairing. Fixed diagnostic phases deliberately change
the replay positions; absent or malformed configuration leaves normal capture
and interpolation behavior untouched.
"""

from __future__ import annotations


def _replace(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found != count:
        raise ValueError(
            f"AO FI pixel probe seam expected {count}, found {found}: {old[:90]!r}"
        )
    return text.replace(old, new)


# Kept as a separate fragment so the focused test can compile the strict
# parser/phase arithmetic without pulling in the whole frame-interp TU.
PIXEL_PHASE_HELPER = r'''
/* QA_AO_FI_PIXEL_PHASE: strict, bounded alpha phase selection. */
static int qaAoFiPixelConfig=-1;
static long qaAoFiPixelFrom, qaAoFiPixelTo, qaAoFiPixelStride;
static int qa_ao_fi_pixel_uint(const char **cursor, long *out)
{
    const char *p=*cursor;
    unsigned long value=0;
    if (*p<'0' || *p>'9') return 0;
    while (*p>='0' && *p<='9') {
        unsigned digit=(unsigned)(*p-'0');
        if (value>(ULONG_MAX-digit)/10u) return 0;
        value=value*10u+digit;
        p++;
    }
    if (value>(unsigned long)LONG_MAX) return 0;
    *out=(long)value; *cursor=p;
    return 1;
}
static int qa_ao_fi_pixel_configure(void)
{
    const char *value=getenv("MP6_QA_AO_FI_PIXELS");
    const char *p;
    long from,to,stride;
    if (qaAoFiPixelConfig>=0) return qaAoFiPixelConfig;
    qaAoFiPixelConfig=0;
    if (!value || !*value) return 0;
    p=value;
    if (!qa_ao_fi_pixel_uint(&p,&from) || *p++!=':' ||
        !qa_ao_fi_pixel_uint(&p,&to) || *p++!=':' ||
        !qa_ao_fi_pixel_uint(&p,&stride) || *p!='\0' || stride<=0 || stride>120 ||
        to<from || to-from>120 || to==LONG_MAX) {
        fprintf(stderr,"[AO-FI-PIXEL-ERROR] MP6_QA_AO_FI_PIXELS='%s' is invalid; disabled\n",
                value);
        return 0;
    }
    qaAoFiPixelFrom=from; qaAoFiPixelTo=to; qaAoFiPixelStride=stride;
    qaAoFiPixelConfig=1;
    return 1;
}
static int qa_ao_fi_pixel_selected_tick(long tick)
{
    long delta;
    if (!qa_ao_fi_pixel_configure() || tick<qaAoFiPixelFrom ||
        tick>qaAoFiPixelTo) return 0;
    delta=tick-qaAoFiPixelFrom;
    return delta%qaAoFiPixelStride==0;
}
static double qa_ao_fi_pixel_phase(long tick)
{
    long slot;
    if (!qa_ao_fi_pixel_selected_tick(tick)) return 0.0;
    slot=(tick-qaAoFiPixelFrom)/qaAoFiPixelStride;
    return (double)(slot%4)*0.25;
}
'''


PIXEL_TYPES = r'''
/* QA_AO_FI_PIXEL: bounded submitted matrix metadata shared with frame_dump. */
typedef struct {
    uint32_t index;
    int object, hasPrevious;
    char name[64];
    FiPosKey key, previousKey;
    uint32_t currentBits[12], previousBits[12], outputBits[12];
    uint32_t cameraViewBits[12], cameraBits[13];
} QaAoFiPixelRow;
typedef struct {
    int valid, captured, interp;
    unsigned presentIndex, rowCount, matrixCount;
    unsigned hintOverflow, previousHintOverflow;
    long viTick, previousViTick, seal, previousSeal;
    double alpha;
    QaAoFiPixelRow rows[QA_AO_FI_HINT_MAX];
} QaAoFiPixelPending;
static QaAoFiPixelPending qaAoFiPixelPending;
static long qaAoFiPixelCapturedViTick=LONG_MIN;
static int qa_ao_fi_pixel_configure(void);
'''


PIXEL_HELPERS = PIXEL_PHASE_HELPER + r'''
static void qa_ao_fi_pixel_pending_reset(void)
{
    memset(&qaAoFiPixelPending,0,sizeof(qaAoFiPixelPending));
}
static void qa_ao_fi_pixel_copy_bits(uint32_t *out, const uint8_t *bytes)
{
    unsigned n;
    if (!bytes) return;
    for (n=0;n<12;n++) out[n]=rd_be32(bytes+4*n);
}
static void qa_ao_fi_pixel_copy_float_bits(uint32_t *out, const float *values,
                                           unsigned count)
{
    unsigned n;
    for (n=0;n<count;n++) memcpy(&out[n],&values[n],sizeof(uint32_t));
}
static int32_t qa_ao_fi_pixel_previous_index(const FiStream *cur,
                                             const FiStream *prev,
                                             uint32_t index)
{
    if (!cur || !prev || !prev->walkOk || index>=cur->posCount) return -1;
    int32_t j=cur->prevPos[index];
    if (j>=0 && (uint32_t)j<prev->keyCount &&
        fi_key_equal(&cur->posKey[index],&prev->posKey[j])) return j;
    return -1;
}
static int qa_ao_fi_pixel_duplicate(uint32_t index)
{
    unsigned j;
    for (j=0;j<qaAoFiPixelPending.rowCount;j++)
        if (qaAoFiPixelPending.rows[j].index==index) return 1;
    return 0;
}
static int qa_ao_fi_pixel_append(const FiStream *cur, const FiStream *prev,
                                const QaAoFiReplay *replay)
{
    const QaAoFiHint *hint;
    const FiPosKey *key, *previousKey;
    QaAoFiPixelRow *row;
    int32_t previousIndex;
    if (!replay->submitted || !replay->hasOutput) return 1;
    if (replay->index>=cur->posCount || replay->index>=cur->keyCount ||
        qa_ao_fi_pixel_duplicate(replay->index)) return 0;
    hint=qa_ao_fi_find(cur,(int32_t)replay->index);
    key=&cur->posKey[replay->index];
    if (!hint || !key->valid || hint->model!=key->model ||
        hint->object!=(int)(key->sub&0x3fffu) || !qa_ao_fi_select(
            hint,key,cur->posKey,cur->keyCount,
            prev && prev->walkOk ? prev->posKey : NULL,
            prev && prev->walkOk ? prev->keyCount : 0)) return 0;
    if (qaAoFiPixelPending.rowCount>=QA_AO_FI_HINT_MAX) return 0;
    previousIndex=qa_ao_fi_pixel_previous_index(cur,prev,replay->index);
    if (previousIndex<0 || (uint32_t)previousIndex>=prev->posCount) return 0;
    row=&qaAoFiPixelPending.rows[qaAoFiPixelPending.rowCount++];
    memset(row,0,sizeof(*row));
    row->index=replay->index; row->object=hint->object; row->key=*key;
    snprintf(row->name,sizeof(row->name),"%s",hint->name);
    qa_ao_fi_pixel_copy_bits(row->currentBits,
                             cur->data+cur->posOff[replay->index]);
    qa_ao_fi_pixel_copy_bits(row->outputBits,replay->output);
    qa_ao_fi_pixel_copy_float_bits(row->cameraViewBits,hint->cameraView,12);
    qa_ao_fi_pixel_copy_float_bits(row->cameraBits,hint->camera,13);
    if (previousIndex>=0) {
        previousKey=&prev->posKey[previousIndex];
        row->hasPrevious=1; row->previousKey=*previousKey;
        qa_ao_fi_pixel_copy_bits(row->previousBits,
                                 prev->data+prev->posOff[previousIndex]);
    }
    return 1;
}
static void qa_ao_fi_pixel_snapshot(const FiStream *cur, const FiStream *prev,
                                    double alpha, int interp)
{
    unsigned j;
    qa_ao_fi_pixel_pending_reset();
    if (!cur || !prev || !interp || !cur->walkOk || !prev->walkOk ||
        cur->qaViTick==qaAoFiPixelCapturedViTick ||
        !qa_ao_fi_pixel_selected_tick(cur->qaViTick) ||
        cur->qaHintOverflow || prev->qaHintOverflow ||
        qaAoFiReplayCount>=QA_AO_FI_HINT_MAX) return;
    for (j=0;j<qaAoFiReplayCount;j++)
        if (!qa_ao_fi_pixel_append(cur,prev,&qaAoFiReplay[j])) {
            qa_ao_fi_pixel_pending_reset();
            return;
        }
    if (!qaAoFiPixelPending.rowCount) {
        qa_ao_fi_pixel_pending_reset();
        return;
    }
    qaAoFiPixelPending.valid=1; qaAoFiPixelPending.interp=1;
    qaAoFiPixelPending.matrixCount=qaAoFiPixelPending.rowCount;
    qaAoFiPixelPending.hintOverflow=cur->qaHintOverflow;
    qaAoFiPixelPending.previousHintOverflow=prev->qaHintOverflow;
    qaAoFiPixelPending.viTick=cur->qaViTick;
    qaAoFiPixelPending.previousViTick=prev->qaViTick;
    qaAoFiPixelPending.seal=cur->tick;
    qaAoFiPixelPending.previousSeal=prev->tick;
    qaAoFiPixelPending.alpha=alpha;
}
static void qa_ao_fi_pixel_override_alpha(const FiStream *cur, double *alpha)
{
    if (cur && alpha && qa_ao_fi_pixel_selected_tick(cur->qaViTick))
        *alpha=qa_ao_fi_pixel_phase(cur->qaViTick);
}
int qa_ao_fi_pixel_present_begin(int replayFrame, unsigned presentIndex)
{
    if (!replayFrame || !qaAoFiPixelPending.valid ||
        qaAoFiPixelPending.captured || !qaAoFiPixelPending.interp ||
        !qaAoFiPixelPending.rowCount ||
        qaAoFiPixelCapturedViTick==qaAoFiPixelPending.viTick) return 0;
    qaAoFiPixelPending.presentIndex=presentIndex;
    return 1;
}
int qa_ao_fi_pixel_present_mode(void) { return qa_ao_fi_pixel_configure(); }
static void qa_ao_fi_pixel_json_string(const char *value)
{
    const unsigned char *p=(const unsigned char *)value;
    fputc('"',stderr);
    if (p) for (;*p;p++) {
        if (*p=='"' || *p=='\\') { fputc('\\',stderr); fputc(*p,stderr); }
        else if (*p<32 || *p>=127) fprintf(stderr,"\\u%04x",*p);
        else fputc(*p,stderr);
    }
    fputc('"',stderr);
}
static void qa_ao_fi_pixel_json_u32s(const uint32_t *values, unsigned count,
                                     int present)
{
    unsigned n;
    if (!present) { fputs("null",stderr); return; }
    fputc('[',stderr);
    for (n=0;n<count;n++) fprintf(stderr,"%s%u",n?",":"",values[n]);
    fputc(']',stderr);
}
static void qa_ao_fi_pixel_json_key(const FiPosKey *key)
{
    if (!key) { fputs("null",stderr); return; }
    fprintf(stderr,"{\"valid\":%u,\"camera\":%d,\"model\":%d,\"generation\":%u,"
        "\"sub\":%u,\"ordinal\":%u}",(unsigned)key->valid,(int)key->camera,
        (int)key->model,key->generation,(unsigned)key->sub,(unsigned)key->ordinal);
}
static void qa_ao_fi_pixel_emit(unsigned seq, unsigned presentIndex)
{
    unsigned j;
    fprintf(stderr,"[AO-FI-PIXEL] {\"seq\":%u,\"presentIndex\":%u,"
        "\"captureViTick\":%ld,\"viTick\":%ld,\"previousViTick\":%ld,"
        "\"seal\":%ld,\"previousSeal\":%ld,\"alpha\":%.9g,"
        "\"interpolationEnabled\":%d,\"matrixCount\":%u,"
        "\"hintOverflow\":%u,\"previousHintOverflow\":%u,\"matrices\":[",
        seq,presentIndex,mp6_tick_count,
        qaAoFiPixelPending.viTick,qaAoFiPixelPending.previousViTick,
        qaAoFiPixelPending.seal,qaAoFiPixelPending.previousSeal,
        qaAoFiPixelPending.alpha,qaAoFiPixelPending.interp,
        qaAoFiPixelPending.matrixCount,qaAoFiPixelPending.hintOverflow,
        qaAoFiPixelPending.previousHintOverflow);
    for (j=0;j<qaAoFiPixelPending.rowCount;j++) {
        const QaAoFiPixelRow *row=&qaAoFiPixelPending.rows[j];
        fprintf(stderr,"%s{\"submitted\":true,\"index\":%u,"
            "\"object\":%d,\"name\":",j?",":"",row->index,row->object);
        qa_ao_fi_pixel_json_string(row->name);
        fputs(",\"key\":",stderr); qa_ao_fi_pixel_json_key(&row->key);
        fputs(",\"previousKey\":",stderr);
        qa_ao_fi_pixel_json_key(row->hasPrevious?&row->previousKey:NULL);
        fputs(",\"currentBits\":",stderr);
        qa_ao_fi_pixel_json_u32s(row->currentBits,12,1);
        fputs(",\"previousBits\":",stderr);
        qa_ao_fi_pixel_json_u32s(row->previousBits,12,row->hasPrevious);
        fputs(",\"outputBits\":",stderr);
        qa_ao_fi_pixel_json_u32s(row->outputBits,12,1);
        fputs(",\"cameraViewBits\":",stderr);
        qa_ao_fi_pixel_json_u32s(row->cameraViewBits,12,1);
        fputs(",\"cameraBits\":",stderr);
        qa_ao_fi_pixel_json_u32s(row->cameraBits,13,1);
        fputc('}',stderr);
    }
    fputs("]}\n",stderr);
}
void qa_ao_fi_pixel_present_commit(unsigned seq, unsigned presentIndex,
                                   int csvWritten)
{
    if (!csvWritten || !qaAoFiPixelPending.valid ||
        qaAoFiPixelPending.captured ||
        qaAoFiPixelPending.presentIndex!=presentIndex) return;
    qaAoFiPixelCapturedViTick=qaAoFiPixelPending.viTick;
    qaAoFiPixelPending.captured=1;
    qa_ao_fi_pixel_emit(seq,presentIndex);
}
'''


PIXEL_DUMP_DECLS = r'''
/* QA_AO_FI_PIXEL_DUMP: frame_dump handoff for one successful replay capture. */
extern int qa_ao_fi_pixel_present_begin(int, unsigned);
extern int qa_ao_fi_pixel_present_mode(void);
extern void qa_ao_fi_pixel_present_commit(unsigned, unsigned, int);
'''


def instrument_frame_interp(text: str) -> str:
    """Add bounded alpha phases and pending pixel metadata after matrix QA."""
    if "QA_AO_FI_PIXEL:" in text:
        raise ValueError("AO FI pixel probe already installed")
    if "QA_AO_FI_MATRIX:" not in text:
        raise ValueError("AO FI pixel probe requires the AO FI matrix probe")
    text = _replace(
        text,
        "static long qaAoFiFrom, qaAoFiTo, qaAoFiLimit, qaAoFiEmitted;",
        "static long qaAoFiFrom, qaAoFiTo, qaAoFiLimit, qaAoFiEmitted;\n"
        + PIXEL_TYPES,
    )
    text = _replace(
        text,
        "static void fi_set_active(int on)",
        PIXEL_HELPERS + "\nstatic void fi_set_active(int on)",
    )
    # Pixel evidence must survive the separate matrix-log row/byte cap. Keep
    # that log capped while collecting bounded per-replay hints to TO+1 (the
    # host tick is one ahead of the current sealed stream during idle replay).
    text = _replace(text, "    return qaAoFiConfig && !qaAoFiBufferClosed",
        "    if (qa_ao_fi_pixel_configure()) return qaAoFiConfig &&\n"
        "        mp6_tick_count>=qaAoFiFrom-1 && mp6_tick_count<=qaAoFiTo+1;\n"
        "    return qaAoFiConfig && !qaAoFiBufferClosed")
    text = _replace(text, "    if (!qa_ao_fi_row_begin()) return;",
        "    if (qaAoFiBufferClosed || qaAoFiBufferOverflow || qaAoFiEmitted>=qaAoFiLimit) return;\n"
        "    if (!qa_ao_fi_row_begin()) return;")
    text = _replace(
        text,
        "    len = fi_build_replay(cur, prev, interp, alpha);",
        "    qa_ao_fi_pixel_override_alpha(cur,&alpha);\n"
        "    len = fi_build_replay(cur, prev, interp, alpha);",
    )
    text = _replace(
        text,
        "    qa_ao_fi_emit_replay(cur,prev,alpha,interp);\n    /* MP6_FRAME_DUMP",
        "    qa_ao_fi_emit_replay(cur,prev,alpha,interp);\n"
        "    qa_ao_fi_pixel_snapshot(cur,prev,alpha,interp);\n"
        "    /* MP6_FRAME_DUMP",
    )
    text = _replace(
        text,
        "    { extern void mp6_frame_dump_present(int replayFrame); mp6_frame_dump_present(1); }\n",
        "    { extern void mp6_frame_dump_present(int replayFrame); mp6_frame_dump_present(1); }\n"
        "    qa_ao_fi_pixel_pending_reset();\n",
    )
    return text


def instrument_frame_dump(text: str) -> str:
    """Attach pixel metadata only after a replay image and CSV row succeed."""
    if "QA_AO_FI_PIXEL_DUMP" in text:
        raise ValueError("AO FI pixel frame-dump probe already installed")
    text = _replace(text, "extern long mp6_tick_count;\n",
                    "extern long mp6_tick_count;\n" + PIXEL_DUMP_DECLS)
    text = _replace(text, "    FILE *f;\n",
                    "    FILE *f;\n"
                    "    int qaAoFiPixelCapture=0, qaAoFiPixelCsvOk=0;\n")
    text = _replace(
        text,
        "    s_fdPresentIndex++;\n    if (!s_fdArmed || s_fdDone) return;",
        "    s_fdPresentIndex++;\n"
        "    qaAoFiPixelCapture=qa_ao_fi_pixel_present_begin(\n"
        "        replayFrame,s_fdPresentIndex);\n"
        "    if (qa_ao_fi_pixel_present_mode() && !qaAoFiPixelCapture) return;\n"
        "    if (!s_fdArmed || s_fdDone) return;",
    )
    text = _replace(
        text,
        "    s_fdWritten++;\n",
        "    if (qaAoFiPixelCapture && s_fdIndex != NULL)\n"
        "        qaAoFiPixelCsvOk=(fflush(s_fdIndex)==0 && !ferror(s_fdIndex));\n"
        "    if (qaAoFiPixelCapture)\n"
        "        qa_ao_fi_pixel_present_commit((unsigned)s_fdWritten,\n"
        "            s_fdPresentIndex,qaAoFiPixelCsvOk);\n\n"
        "    s_fdWritten++;\n",
    )
    return text
