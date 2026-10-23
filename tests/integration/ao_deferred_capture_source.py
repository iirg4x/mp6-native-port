"""QA-only all-present capture wiring. Never force interpolation alpha.

The backend queues GPU copies during the burst; readback and file/JSON output
are deferred until after it. Copy/metadata overhead is still real.
"""
from __future__ import annotations


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"deferred capture seam expected once: {old[:90]!r}")
    return text.replace(old, new)


DEFERRED_METADATA = r'''
/* QA_DEFERRED_METADATA: per-present immutable submitted matrix snapshots. */
static QaAoFiPixelPending qaDeferredMetadata[128];
static long qaDeferredCaptureTicks[128];
static unsigned qaDeferredMetadataCount;
static int qaDeferredMetadataFailed;
static long qaDeferredEmitCaptureTick;
static void qa_deferred_real_snapshot(const FiStream *cur, const FiStream *prev)
{
    qa_ao_fi_pixel_pending_reset();
    if (!cur || !prev || !cur->walkOk || !prev->walkOk ||
        !qa_ao_fi_pixel_selected_tick(cur->qaViTick) ||
        cur->qaHintOverflow || prev->qaHintOverflow) return;
    for (unsigned j=0;j<cur->qaHintCount;j++) {
        const QaAoFiHint *h=&cur->qaHints[j];
        if (h->index>=cur->posCount || h->index>=cur->keyCount) continue;
        if (!qa_ao_fi_select(h,&cur->posKey[h->index],cur->posKey,cur->keyCount,
                            prev->posKey,prev->keyCount)) continue;
        QaAoFiReplay r={0};
        r.index=h->index; r.submitted=1; r.hasOutput=1;
        memcpy(r.output,cur->data+cur->posOff[h->index],48);
        if (!qa_ao_fi_pixel_append(cur,prev,&r)) {
            qa_ao_fi_pixel_pending_reset(); return;
        }
    }
    if (!qaAoFiPixelPending.rowCount) return;
    qaAoFiPixelPending.valid=1;
    qaAoFiPixelPending.interp=0;
    qaAoFiPixelPending.matrixCount=qaAoFiPixelPending.rowCount;
    qaAoFiPixelPending.viTick=cur->qaViTick;
    qaAoFiPixelPending.previousViTick=prev->qaViTick;
    qaAoFiPixelPending.seal=cur->tick;
    qaAoFiPixelPending.previousSeal=prev->tick;
    qaAoFiPixelPending.alpha=0.0;
}
int mp6_qa_deferred_metadata_store(unsigned seq, unsigned presentIndex, int replay)
{
    if (seq>=128 || seq!=qaDeferredMetadataCount || !qaAoFiPixelPending.valid ||
        (replay!=0)!= (qaAoFiPixelPending.interp!=0)) {
        qaDeferredMetadataFailed=1; return 0;
    }
    qaDeferredMetadata[seq]=qaAoFiPixelPending;
    qaDeferredMetadata[seq].presentIndex=presentIndex;
    qaDeferredCaptureTicks[seq]=mp6_tick_count;
    qaDeferredMetadataCount++;
    qa_ao_fi_pixel_pending_reset();
    return 1;
}
void mp6_qa_deferred_metadata_flush(void)
{
    for (unsigned j=0;j<qaDeferredMetadataCount;j++) {
        qaAoFiPixelPending=qaDeferredMetadata[j];
        qaDeferredEmitCaptureTick=qaDeferredCaptureTicks[j];
        qa_ao_fi_pixel_emit(j,qaAoFiPixelPending.presentIndex);
    }
    fprintf(stderr,"[AO-DEFERRED-METADATA] count=%u error=%d alpha=natural\n",
            qaDeferredMetadataCount,qaDeferredMetadataFailed);
}
'''


def instrument_interp(text):
    """Applied after existing matrix and pixel transforms in a private build."""
    if "QA_DEFERRED_METADATA" in text:
        raise ValueError("deferred interpolation probe already installed")
    text = replace_once(text, "    qa_ao_fi_pixel_override_alpha(cur,&alpha);\n", "")
    text = replace_once(text, "        cur->qaViTick==qaAoFiPixelCapturedViTick ||\n", "")
    # Historical fixed-phase output is deliberately not reused as evidence.
    text = replace_once(text, '[AO-FI-PIXEL] {', '[AO-DEFERRED-FRAME] {')
    text = replace_once(text, 'seq,presentIndex,mp6_tick_count,',
                        'seq,presentIndex,qaDeferredEmitCaptureTick,')
    # Both historical matrix rows and pixel rows contain an alpha format.
    # Change only the latter, leaving the matrix probe's record format intact.
    emitter = 'static void qa_ao_fi_pixel_emit(unsigned seq, unsigned presentIndex)'
    before, separator, after = text.partition(emitter)
    if not separator:
        raise ValueError('missing deferred pixel emitter')
    text = before + separator + replace_once(after, '\\"alpha\\":%.9g,', '\\"alpha\\":%.17g,')
    # The emitter needs the saved capture tick declaration before its definition.
    metadata = DEFERRED_METADATA.replace('static long qaDeferredEmitCaptureTick;\n', '')
    text = replace_once(text, 'static void qa_ao_fi_pixel_emit(unsigned seq, unsigned presentIndex)',
                        'static long qaDeferredEmitCaptureTick;\n'
                        'static void qa_ao_fi_pixel_emit(unsigned seq, unsigned presentIndex)')
    text = replace_once(text, 'static void fi_set_active(int on)',
                        metadata + '\nstatic void fi_set_active(int on)')
    text = replace_once(text, '    qa_ao_fi_emit_real(s,s_prev>=0?&s_streams[s_prev]:NULL);',
                        '    qa_ao_fi_emit_real(s,s_prev>=0?&s_streams[s_prev]:NULL);\n'
                        '    qa_deferred_real_snapshot(s,s_prev>=0?&s_streams[s_prev]:NULL);')
    return text


FRONTEND = r'''
/* QA_DEFERRED_CAPTURE: no readback, disk IO, or metadata print inside burst. */
extern int mp6_qa_deferred_capture_enqueue(const char *, unsigned, unsigned, long,
    int, unsigned long long, unsigned, unsigned, unsigned, unsigned);
extern int mp6_qa_deferred_capture_finish(void);
extern int mp6_qa_deferred_metadata_store(unsigned, unsigned, int);
extern void mp6_qa_deferred_metadata_flush(void);
static int qaDeferredConfigured, qaDeferredFrom, qaDeferredTo, qaDeferredQueued;
static int qa_deferred_present(int replay)
{
    if (!qaDeferredConfigured) {
        const char *from=getenv("MP6_QA_DEFERRED_FROM");
        const char *to=getenv("MP6_QA_DEFERRED_TO");
        qaDeferredConfigured=1;
        if (!fd_enabled() || !from || !to || !mp6_parse_i32_strict(from,3000,1000000,&qaDeferredFrom) ||
            !mp6_parse_i32_strict(to,3000,1000000,&qaDeferredTo) ||
            qaDeferredTo<qaDeferredFrom || qaDeferredTo-qaDeferredFrom>119 ||
            s_fdWantCount<2 || s_fdWantCount>128 || s_fdStride!=1 || s_fdDelay!=0 ||
            s_fdDownscale!=1 || s_fdRectW<=0 || s_fdRectH<=0 || s_fdTrigger[0]) {
            fprintf(stderr,"[AO-DEFERRED-ERROR] invalid capture configuration\n");
            s_fdDone=1; return 1;
        }
    }
    if (s_fdDone || !s_fdArmed || mp6_tick_count<qaDeferredFrom) return 1;
    if (mp6_tick_count<=qaDeferredTo && qaDeferredQueued<s_fdWantCount) {
        if (!mp6_qa_deferred_metadata_store((unsigned)qaDeferredQueued,s_fdPresentIndex,replay) ||
            !mp6_qa_deferred_capture_enqueue(s_fdDir,(unsigned)qaDeferredQueued,
                s_fdPresentIndex,mp6_tick_count,replay,
                (unsigned long long)mp6_host_monotonic_ns(),
                (unsigned)s_fdRectX,(unsigned)s_fdRectY,(unsigned)s_fdRectW,(unsigned)s_fdRectH)) {
            fprintf(stderr,"[AO-DEFERRED-ERROR] capture/metadata admission failed\n");
            s_fdDone=1;
        } else ++qaDeferredQueued;
    }
    if (s_fdDone || qaDeferredQueued==s_fdWantCount || mp6_tick_count>qaDeferredTo) {
        int ok=mp6_qa_deferred_capture_finish();
        mp6_qa_deferred_metadata_flush();
        fprintf(stderr,"[AO-DEFERRED-FINISH] requested=%d queued=%d complete=%d\n",
                s_fdWantCount,qaDeferredQueued,ok && qaDeferredQueued==s_fdWantCount && !s_fdDone);
        s_fdDone=1;
    }
    return 1;
}
'''


def instrument_dump(text):
    """Apply to original frame_dump.c, replacing only the private build path."""
    if "QA_DEFERRED_CAPTURE" in text:
        raise ValueError("deferred frame dump already installed")
    text = replace_once(text, 'void mp6_frame_dump_present(int replayFrame)\n{',
                        FRONTEND + '\nvoid mp6_frame_dump_present(int replayFrame)\n{')
    return replace_once(text, '    s_fdPresentIndex++;\n',
                        '    s_fdPresentIndex++;\n'
                        '    if (qa_deferred_present(replayFrame)) return;\n')
