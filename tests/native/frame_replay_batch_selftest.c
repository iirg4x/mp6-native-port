#include <assert.h>
#include <time.h>
#include "replay-under-test.inc"

long mp6_tick_count;
static int camera_cut, model_resets;
int mp6_console_cvar_get(int id, int value) { (void)id; return value; }
int mp6_fi_model_camera_stable(int camera) { return camera != 1 && !camera_cut; }

/* These engine seams are retained by the Windows linker, but must never be
 * reached by the isolated replay builder. Fail the test if that changes. */
int mp6_enh_unlocked_fps(void) { abort(); }
int mp6_tick_interpolation_possible(void) { abort(); }
int mp6_fi_capture_camera_id(void) { abort(); }
int mp6_fi_capture_context_next(int *camera, int *model, uint32_t *generation,
                              uint16_t *sub, uint16_t *ordinal) { abort(); }
void mp6_ao_begin_frame(void) { abort(); }
void aurora_gx_set_drain_capture(void (*fn)(const void *, uint32_t, void *), void *user) { abort(); }
void aurora_gx_export_vtx_layout(uint8_t *desc, uint8_t *cnt, uint8_t *type) { abort(); }
void mp6_fi_model_reset(void) { ++model_resets; }
void mp6_fi_model_snapshot(void) { abort(); }
void mp6_fi_model_animlog(long tick) { abort(); }
uint64_t mp6_host_monotonic_ns(void) { abort(); }

/* Frozen pre-batched stream walker.  Keep this oracle intentionally boring:
 * it is the old one-command-at-a-time loop copied before the NOP-span change.
 * The test below compares the candidate's commands, position fingerprint,
 * validity and chunk-boundary census against this implementation. */
static void fi_walk_stream_reference(FiStream *s)
{
    FiVtxState vs;
    uint32_t pos = 0, chunkIdx = 0, aoIdx = 0;
    fi_pair_cache_invalidate();
    s->walked = 1;
    s->walkOk = 0;
    s->posCount = 0;
    s->commandCount = 0;
    if (s->aoCount > FI_AO_MAX) return;
    fi_seed_state(&vs, s);
    while (pos < s->size) {
        FiCmd c;
        int markerBoundary = aoIdx < s->aoCount && s->ao[aoIdx].offset == pos;
        while (aoIdx < s->aoCount && s->ao[aoIdx].offset == pos) ++aoIdx;
        if (aoIdx < s->aoCount && s->ao[aoIdx].offset < pos) return;
        uint32_t n = fi_walk_one(s->data, pos, s->size, &vs, &c);
        if (n == 0) return;
        if (fi_is_pos_load(&c)) {
            if (!fi_grow((void **)&s->posOff, &s->posCap, s->posCount + 1,
                         sizeof(uint32_t))) return;
            s->posOff[s->posCount++] = c.offset + 5;
        }
        if (!markerBoundary && c.op < 8 && s->commandCount &&
            s->commands[s->commandCount - 1].op < 8) {
            s->commands[s->commandCount - 1].size += n;
        } else {
            if (s->commandCount == UINT32_MAX ||
                !fi_grow((void **)&s->commands, &s->commandCap,
                         s->commandCount + 1, sizeof(FiCmd))) return;
            s->commands[s->commandCount++] = c;
        }
        pos += n;
        while (chunkIdx < s->chunkCount && s->chunkEnd[chunkIdx] <= pos) {
            s_diagChunkChecked++;
            if (s->chunkEnd[chunkIdx] == pos) s_diagChunkAligned++;
            else s_diagChunkMisaligned++;
            chunkIdx++;
        }
    }
    while (aoIdx < s->aoCount && s->ao[aoIdx].offset == pos) ++aoIdx;
    s->walkOk = (pos == s->size && s->posCount == s->keyCount &&
                 aoIdx == s->aoCount);
}

static void assert_same_cmd(const FiCmd *a, const FiCmd *b)
{
    assert(a->offset == b->offset);
    assert(a->size == b->size);
    assert(a->op == b->op);
    assert(a->auroraSub == b->auroraSub);
    assert(a->bpReg == b->bpReg);
    assert(a->xfAddr == b->xfAddr);
    assert(a->xfCount == b->xfCount);
}

static void assert_same_walk(const FiStream *candidate, const FiStream *reference,
                             const uint32_t candidateDiag[3],
                             const uint32_t referenceDiag[3])
{
    assert(candidate->walked == reference->walked);
    assert(candidate->walkOk == reference->walkOk);
    assert(candidate->posCount == reference->posCount);
    assert(candidate->commandCount == reference->commandCount);
    assert(candidate->aoCount == reference->aoCount);
    for (uint32_t i = 0; i < candidate->aoCount; ++i) {
        assert(candidate->ao[i].offset == reference->ao[i].offset);
        assert(candidate->ao[i].kind == reference->ao[i].kind);
        assert(candidate->ao[i].camera == reference->ao[i].camera);
    }
    for (uint32_t i = 0; i < candidate->posCount; ++i)
        assert(candidate->posOff[i] == reference->posOff[i]);
    for (uint32_t i = 0; i < candidate->commandCount; ++i)
        assert_same_cmd(&candidate->commands[i], &reference->commands[i]);
    for (unsigned i = 0; i < 3; ++i) assert(candidateDiag[i] == referenceDiag[i]);
}

static void oracle_u32(uint8_t *data, uint32_t *used, uint32_t value)
{
    data[(*used)++] = (uint8_t)(value >> 24);
    data[(*used)++] = (uint8_t)(value >> 16);
    data[(*used)++] = (uint8_t)(value >> 8);
    data[(*used)++] = (uint8_t)value;
}

static void oracle_pos_load(uint8_t *data, uint32_t *used, uint32_t address)
{
    data[(*used)++] = GX_LOAD_XF_REG;
    oracle_u32(data, used, (11u << 16) | address);
    memset(data + *used, 0, 48);
    *used += 48;
}

static void compare_walkers(const uint8_t *data, uint32_t size,
                            const FiAoMarker *markers, uint32_t markerCount,
                            const uint32_t *chunkEnds, uint32_t chunkCount,
                            uint32_t keyCount, int requireBatch)
{
    FiStream candidate = {0}, reference = {0};
    uint32_t candidateDiag[3], referenceDiag[3];
    unsigned candidateCalls, referenceCalls;

    candidate.data = (uint8_t *)data;
    candidate.size = size;
    candidate.aoCount = markerCount;
    memcpy(candidate.ao, markers, markerCount * sizeof(*markers));
    candidate.chunkEnd = (uint32_t *)chunkEnds;
    candidate.chunkCount = chunkCount;
    candidate.keyCount = keyCount;
    reference = candidate;
    reference.commands = NULL;
    reference.commandCap = 0;
    reference.posOff = NULL;
    reference.posCap = 0;

    s_diagChunkAligned = s_diagChunkChecked = s_diagChunkMisaligned = 0;
    candidateCalls = test_walk_calls;
    fi_walk_stream(&candidate);
    candidateCalls = test_walk_calls - candidateCalls;
    candidateDiag[0] = s_diagChunkAligned;
    candidateDiag[1] = s_diagChunkChecked;
    candidateDiag[2] = s_diagChunkMisaligned;

    s_diagChunkAligned = s_diagChunkChecked = s_diagChunkMisaligned = 0;
    referenceCalls = test_walk_calls;
    fi_walk_stream_reference(&reference);
    referenceCalls = test_walk_calls - referenceCalls;
    referenceDiag[0] = s_diagChunkAligned;
    referenceDiag[1] = s_diagChunkChecked;
    referenceDiag[2] = s_diagChunkMisaligned;

    assert_same_walk(&candidate, &reference, candidateDiag, referenceDiag);
#ifndef MP6_BENCHMARK_WALK
    if (requireBatch) assert(candidateCalls < referenceCalls);
#else
    (void)requireBatch;
    (void)candidateCalls;
    (void)referenceCalls;
#endif
    free(candidate.commands);
    free(candidate.posOff);
    free(reference.commands);
    free(reference.posOff);
}

static void walker_oracle_cases(void)
{
    uint8_t data[4096];
    FiAoMarker markers[FI_AO_MAX];
    uint32_t chunks[32];
    uint32_t used = 0, markerCount = 0, chunkCount = 0;

    memset(markers, 0, sizeof(markers));
    /* Exercise every short NOP span with a marker and a chunk end at every
     * nearby offset.  This includes markers just outside the stream and
     * chunk ends that are stale or beyond the stream. */
    for (unsigned length = 0; length <= 16; ++length) {
        for (unsigned markerOffset = 0; markerOffset <= length + 1; ++markerOffset) {
            for (unsigned chunkOffset = 0; chunkOffset <= length + 1; ++chunkOffset) {
                for (unsigned i = 0; i < length; ++i) data[i] = (uint8_t)(i & 7u);
                memset(markers, 0, sizeof(markers));
                markers[0].offset = markerOffset;
                chunks[0] = chunkOffset;
                chunks[1] = length;
                compare_walkers(data, length, markers, 1, chunks, 2, 0, 0);
            }
        }
    }
    /* The legacy diagnostic accepts captured chunk ends in their supplied
     * order; retain that corner behavior while batching. */
    for (unsigned i = 0; i < 8; ++i) data[i] = (uint8_t)i;
    chunks[0] = 5;
    chunks[1] = 3;
    chunks[2] = 6;
    chunks[3] = 8;
    compare_walkers(data, 8, markers, 0, chunks, 4, 0, 0);

    /* A zero boundary is stale until the first command completes.  The next
     * end (1) must still be aligned, so this catches the boundary-at-zero
     * interaction that a single long run would otherwise miss. */
    for (unsigned i = 0; i < 8; ++i) data[i] = (uint8_t)i;
    chunks[0] = 0;
    chunks[1] = 1;
    chunks[2] = 3;
    chunks[3] = 8;
    compare_walkers(data, 8, markers, 0, chunks, 4, 0, 1);

    used = 0;
    /* The varied byte values all have opcode 0 and must batch. Markers and
     * drain chunks deliberately split the run at observable boundaries. */
    markers[markerCount++].offset = 0;
    for (unsigned i = 0; i < 257; ++i) {
        if (i == 5) markers[markerCount++].offset = used;
        if (i == 128) {
            markers[markerCount++].offset = used;
            markers[markerCount++].offset = used; /* duplicate boundary */
        }
        data[used++] = (uint8_t)(i & 7u);
    }
    chunks[chunkCount++] = 0;       /* preserve the old boundary-at-zero case */
    chunks[chunkCount++] = 64;      /* inside the first raw-NOP run */
    chunks[chunkCount++] = 128;     /* coincident with duplicate AO markers */
    chunks[chunkCount++] = 257;     /* end of the first run */

    /* 0x48/0x4f are GX_CMD_INVL_VC opcodes, not bytes eligible for batching. */
    data[used++] = (uint8_t)GX_CMD_INVL_VC;
    data[used++] = (uint8_t)(GX_CMD_INVL_VC | 7u);
    data[used++] = GX_LOAD_BP_REG;
    data[used++] = 0x52;
    oracle_u32(data, &used, 0x52000000u);
    oracle_pos_load(data, &used, 0);
    for (unsigned i = 0; i < 91; ++i) data[used++] = (uint8_t)(7u - (i & 7u));
    markers[markerCount++].offset = used;
    markers[markerCount++].offset = used; /* duplicate end marker */
    chunks[chunkCount++] = used;
    compare_walkers(data, used, markers, markerCount, chunks, chunkCount, 1, 1);

    /* A marker inside a multi-byte command fails identically; it must not be
     * hidden by the NOP fast path. */
    used = 0;
    data[used++] = GX_LOAD_BP_REG;
    data[used++] = 0x52;
    oracle_u32(data, &used, 0x52000000u);
    memset(markers, 0, sizeof(markers));
    markers[0].offset = 2;
    chunks[0] = used;
    compare_walkers(data, used, markers, 1, chunks, 1, 0, 0);

    /* Out-of-order and beyond-end markers, plus a non-NOP invalid opcode. */
    used = 0;
    for (unsigned i = 0; i < 32; ++i) data[used++] = (uint8_t)(i & 7u);
    memset(markers, 0, sizeof(markers));
    markers[0].offset = 10;
    markers[1].offset = 9;
    chunks[0] = used;
    compare_walkers(data, used, markers, 2, chunks, 1, 0, 1);
    markers[0].offset = used + 1;
    markers[1].offset = used + 1;
    compare_walkers(data, used, markers, 2, chunks, 1, 0, 0);
    data[0] = 0xFF;
    memset(markers, 0, sizeof(markers));
    compare_walkers(data, used, markers, 0, chunks, 1, 0, 0);

    /* A raw-NOP span must stop before a truncated register command, not
     * consume it as padding or turn malformed framing into a valid walk. */
    for (unsigned tail = 0; tail < 2; ++tail) {
        for (unsigned i = 0; i < 32; ++i) data[i] = (uint8_t)(i & 7u);
        data[32] = tail ? GX_LOAD_XF_REG : GX_LOAD_BP_REG;
        chunks[0] = 33;
        compare_walkers(data, 33, markers, 0, chunks, 1, 0, 1);
    }
    puts("Old/new stream walker oracle passed");
}

#ifdef MP6_BENCHMARK_WALK
static void walker_benchmark(void)
{
    uint8_t data[32768];
    FiStream candidate = {0}, reference = {0};
    const unsigned repetitions = 50;
    const uint32_t size = (uint32_t)sizeof(data);

    for (uint32_t i = 0; i < size; ++i) data[i] = (uint8_t)(i & 7u);
    candidate.data = data;
    candidate.size = size;
    candidate.keyCount = 0;
    reference = candidate;
    reference.commands = NULL;
    reference.commandCap = 0;
    reference.posOff = NULL;
    reference.posCap = 0;

    for (unsigned trial = 0; trial < 20; ++trial) {
        clock_t begin;
        double candidateUs, referenceUs;
        if ((trial & 1u) == 0) {
            begin = clock();
            for (unsigned i = 0; i < repetitions; ++i) fi_walk_stream_reference(&reference);
            referenceUs = (double)(clock() - begin) * 1000000.0 /
                          CLOCKS_PER_SEC / repetitions;
            begin = clock();
            for (unsigned i = 0; i < repetitions; ++i) fi_walk_stream(&candidate);
            candidateUs = (double)(clock() - begin) * 1000000.0 /
                          CLOCKS_PER_SEC / repetitions;
        } else {
            begin = clock();
            for (unsigned i = 0; i < repetitions; ++i) fi_walk_stream(&candidate);
            candidateUs = (double)(clock() - begin) * 1000000.0 /
                          CLOCKS_PER_SEC / repetitions;
            begin = clock();
            for (unsigned i = 0; i < repetitions; ++i) fi_walk_stream_reference(&reference);
            referenceUs = (double)(clock() - begin) * 1000000.0 /
                          CLOCKS_PER_SEC / repetitions;
        }
        assert(candidate.walkOk && reference.walkOk);
        printf("WALK trial=%u reference_us=%.3f candidate_us=%.3f\n",
               trial, referenceUs, candidateUs);
    }
    free(candidate.commands);
    free(candidate.posOff);
    free(reference.commands);
    free(reference.posOff);
}
#endif

static uint8_t current[65536], previous[65536], expected[65536];
static uint32_t offsets[256];
static int32_t pairs[256];
static FiPosKey keys[256];
static uint32_t used, positions;
static FiAoMarker markers[FI_AO_MAX];
static uint32_t markerCount;
static uint8_t referenceScratch[65536], candidateBeforeReference[65536];
static FiPreparedPair candidatePairBefore[FI_PAIR_CACHE_MAX];
static uint8_t candidatePairStateBefore[FI_PAIR_CACHE_MAX];

static void mark(unsigned kind) {
    assert(markerCount<FI_AO_MAX);
    markers[markerCount]=(FiAoMarker){0};
    markers[markerCount].offset=used;
    markers[markerCount].kind=kind;
    markers[markerCount++].camera=0;
}

static void put32(uint32_t v) {
    current[used++] = v>>24; current[used++] = v>>16;
    current[used++] = v>>8; current[used++] = v;
}
static void aurora(uint16_t sub, unsigned payload) {
    current[used++] = GX_AURORA;
    current[used++] = sub>>8; current[used++] = sub;
    while (payload--) current[used++] = 0;
}
static void matrix(unsigned slot, unsigned variant) {
    FiMtx m={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    if (variant==2) m.m[0][0]=-1;
    if (variant==3) m.m[1][1]=0;
    current[used++] = GX_LOAD_XF_REG;
    put32((11u<<16)|(slot*12));
    offsets[positions]=used;
    pairs[positions]=variant==5 ? -1 : (int32_t)positions;
    keys[positions].camera=variant==6 ? 1 : 0;
    keys[positions].model=positions;
    keys[positions].ordinal=0;
    keys[positions].valid=1;
    fi_write_mtx(current+used,&m);
    ++positions; used+=48;
    current[used++]=GX_LOAD_XF_REG;
    put32((8u<<16)|(0x400+slot*9));
    for (unsigned n=0;n<9;++n) { wr_bef32(current+used,n%4==0 ? 1.f : 0.f); used+=4; }
}

static void vertices(unsigned n) {
    const unsigned xyz=n&1, stride=xyz ? 12 : 8;
    current[used++]=GX_LOAD_CP_REG;current[used++]=0x50;put32(GX_DIRECT<<9);
    current[used++]=GX_LOAD_CP_REG;current[used++]=0x70;put32(xyz|(GX_F32<<1));
    current[used++]=GX_TRIANGLES;current[used++]=0;current[used++]=3;
    for(unsigned i=0;i<3*stride;++i)current[used++]=0;
    aurora(GX_AURORA_DRAW_SIZED,0);
    current[used++]=GX_TRIANGLES;put32(3*stride);
    for(unsigned i=0;i<3*stride;++i)current[used++]=0;
    aurora(GX_AURORA_DRAW_INDEXED,0);
    current[used++]=GX_TRIANGLES;current[used++]=0;current[used++]=4;put32(6);
    for(unsigned i=0;i<12+4*stride;++i)current[used++]=0;
}

static void malformed_lengths(void) {
    FiStream stream={0};
    stream.data=current;
    FiVtxState state;
    FiCmd command;
    fi_seed_state(&state,&stream);
    assert(!fi_walk_one(NULL,0,0,&state,&command));
    assert(!fi_walk_one(NULL,10,9,&state,&command));
    for (unsigned i=0;i<64;++i) {
        used=0; aurora(GX_AURORA_DRAW_SIZED,0);
        current[used++]=GX_TRIANGLES; put32(UINT32_MAX-i);
        stream.size=used;
        fi_walk_stream(&stream);
        assert(!stream.walkOk);
    }
    const uint32_t counts[]={0x80000000u,0x7ffffffbu,0xfffffffbu,UINT32_MAX};
    for (unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);++i) {
        used=0;
        current[used++]=GX_LOAD_CP_REG;current[used++]=0x50;put32(GX_DIRECT<<9);
        current[used++]=GX_LOAD_CP_REG;current[used++]=0x70;put32(1|(GX_F32<<1));
        aurora(GX_AURORA_DRAW_INDEXED,0);
        current[used++]=GX_TRIANGLES;current[used++]=0;current[used++]=0;
        put32(counts[i]);stream.size=used;
        fi_walk_stream(&stream);assert(!stream.walkOk);
    }
    // Exact-sized valid commands pass; truncating one byte fails.
    used=0;vertices(1);stream.size=used;
    fi_walk_stream(&stream);assert(stream.walkOk);
    --stream.size;fi_walk_stream(&stream);assert(!stream.walkOk);
    free(stream.commands);
    puts("Malformed draw-length overflow and boundary checks passed");
}

static void varied_pair(unsigned n, unsigned trial) {
    /* Real rotation, nonuniform scale and shear; plus translation/rotation
     * cuts, mirrored/zero bases, tiny scales and non-finite inputs. */
    FiTrs a={{0},{1,1,1},{0,0,0,1}}, b=a;
    FiMtx ma,mb;
    unsigned kind=n%12;
    double angle=(trial+n)*.037;
    a.q[1]=sin(angle*.5);a.q[3]=cos(angle*.5);
    b.q[1]=sin((angle+(kind==5 ? .6 : .017))*.5);
    b.q[3]=cos((angle+(kind==5 ? .6 : .017))*.5);
    a.t[0]=n*2.;b.t[0]=a.t[0]+(kind==4 ? 50. : 1.25);
    a.s[1]=1.3;b.s[1]=kind==7 ? 1.4 : 1.3;
    fi_compose(&a,&ma);fi_compose(&b,&mb);
    if (kind==8) { ma.m[0][1]+=.2;mb.m[0][1]+=.21; }
    if (kind==2) { for(unsigned r=0;r<3;++r)ma.m[r][0]*=-1.; }
    if (kind==3) { for(unsigned r=0;r<3;++r)mb.m[r][1]=0.; }
    if (kind==9) { ma.m[0][0]=NAN; }
    if (kind==10) { mb.m[1][2]=INFINITY; }
    if (kind==11) { for(unsigned r=0;r<3;++r)ma.m[r][2]*=1.e-9; }
    if (kind==0) ma=mb;
    fi_write_mtx(previous+offsets[n],&ma);
    fi_write_mtx(current+offsets[n],&mb);
}

/* The frozen builder is allowed to write its own output and diagnostics, but
 * it must never use the candidate's resident plan image as scratch.  Keep the
 * reference buffer separate and verify that the candidate owner, plan, memo,
 * AO state, and pair-cache identity survive the reference call unchanged. */
static uint32_t build_reference_scratch(const FiStream *cur, const FiStream *prev,
                                        int interp, double alpha,
                                        uint8_t *bytes, FiAoMarker *marks,
                                        uint32_t *markCount, uint32_t *census,
                                        double *maxima)
{
    uint8_t *candidateBuf = s_replayBuf;
    uint32_t candidateCap = s_replayCap;
    int candidateOwner = s_replayBufOwner;
    FiReplayPlan candidatePlan = s_replayPlan;
    FiReplayMemo candidateMemo = s_replayMemo;
    FiAoMarker candidateAo[FI_AO_MAX];
    uint32_t candidateAoCount = s_replayAoCount;
    uint32_t candidateLength = candidatePlan.valid ? candidatePlan.length : 0;
    uint64_t candidatePairEpoch = s_pairEpoch;
    FiPreparedPair *candidatePair = s_pairCache;
    uint8_t *candidateState = s_pairState;
    uint32_t candidatePairCap = s_pairCacheCap;
    uint32_t candidateStateCap = s_pairStateCap;
    uint32_t candidatePairCount = s_pairCacheCount;
    const FiStream *candidatePairCur = s_pairCacheCur;
    const FiStream *candidatePairPrev = s_pairCachePrev;
    FiReplayPlan planAfterReference;
    FiReplayMemo memoAfterReference;
    uint32_t len;

    assert(candidateLength <= sizeof(candidateBeforeReference));
    if (candidateLength != 0) {
        assert(candidateBuf != NULL && candidateLength <= candidateCap);
        memcpy(candidateBeforeReference, candidateBuf, candidateLength);
    }
    memcpy(candidateAo, s_replayAo, sizeof(candidateAo));
    assert(candidatePairCount <= FI_PAIR_CACHE_MAX);
    if (candidatePairCount != 0) {
        assert(candidatePair != NULL && candidateState != NULL);
        memcpy(candidatePairBefore, candidatePair,
               candidatePairCount * sizeof(candidatePairBefore[0]));
        memcpy(candidatePairStateBefore, candidateState, candidatePairCount);
    }

    s_replayBuf = referenceScratch;
    s_replayCap = sizeof(referenceScratch);
    s_replayBufOwner = FI_REPLAY_BUF_FULL;
    len = fi_build_replay_reference(cur, prev, interp, alpha);
    assert(len > 0 && len <= sizeof(referenceScratch));
    memcpy(bytes, referenceScratch, len);
    if (markCount != NULL) *markCount = s_replayAoCount;
    if (marks != NULL) memcpy(marks, s_replayAo, sizeof(FiAoMarker) * FI_AO_MAX);
    if (census != NULL) {
        census[0] = s_dbgPosSeen;
        census[1] = s_dbgRewritten;
        census[2] = s_dbgSnapUnpaired;
        census[3] = s_dbgSnapCamera;
        census[4] = s_dbgSnapGate;
        census[5] = s_dbgSnapDecompose;
        census[6] = s_dbgVerbatimEq;
        census[7] = s_dbgScaleHold;
        census[8] = s_dbgCopyClear;
    }
    if (maxima != NULL) {
        maxima[0] = s_dbgMaxTrans;
        maxima[1] = s_dbgMinQdot;
        maxima[2] = s_dbgMaxScaleRatio;
        maxima[3] = s_dbgMaxHeldScale;
        maxima[4] = s_dbgMaxRoundTrip;
        maxima[5] = s_dbgMaxAlpha0Err;
    }
    planAfterReference = s_replayPlan;
    memoAfterReference = s_replayMemo;
    /* The reference output is separate, so the resident candidate bytes must
     * still be exactly what they were before the reference call. */
    if (candidateLength != 0)
        assert(memcmp(candidateBuf, candidateBeforeReference, candidateLength) == 0);
    assert(memcmp(&planAfterReference, &candidatePlan, sizeof(candidatePlan)) == 0);
    assert(memcmp(&memoAfterReference, &candidateMemo, sizeof(candidateMemo)) == 0);
    assert(s_pairEpoch == candidatePairEpoch && s_pairCache == candidatePair &&
           s_pairState == candidateState && s_pairCacheCap == candidatePairCap &&
           s_pairStateCap == candidateStateCap && s_pairCacheCount == candidatePairCount &&
           s_pairCacheCur == candidatePairCur && s_pairCachePrev == candidatePairPrev);
    if (candidatePairCount != 0) {
        assert(memcmp(candidatePair, candidatePairBefore,
                      candidatePairCount * sizeof(candidatePairBefore[0])) == 0);
        assert(memcmp(candidateState, candidatePairStateBefore,
                      candidatePairCount) == 0);
    }

    s_replayBuf = candidateBuf;
    s_replayCap = candidateCap;
    s_replayBufOwner = candidateOwner;
    s_replayPlan = candidatePlan;
    s_replayMemo = candidateMemo;
    s_replayAoCount = candidateAoCount;
    memcpy(s_replayAo, candidateAo, sizeof(candidateAo));
    assert(s_replayBuf == candidateBuf && s_replayCap == candidateCap &&
           s_replayBufOwner == candidateOwner &&
           s_replayPlan.valid == candidatePlan.valid &&
           s_replayMemo.valid == candidateMemo.valid &&
           s_replayAoCount == candidateAoCount);
    return len;
}

static void compare_replay(const FiStream *cur, const FiStream *prev, int interp,
                           double alpha) {
    FiAoMarker marks[FI_AO_MAX];
    uint32_t markCount;
    uint32_t census[9];
    double maxima[6];
    fi_replay_memo_invalidate();
    if (s_replayCap) memset(s_replayBuf,0xA5,s_replayCap);
    uint32_t len=build_reference_scratch(cur,prev,interp,alpha,expected,marks,
                                         &markCount,census,maxima);
    assert(len>0 && len<=used);
    fi_replay_memo_invalidate();
    memset(s_replayBuf,0x5A,s_replayCap);
    unsigned walks=test_walk_calls;
#ifdef MP6_FI_REPLAY_TEST
    uint32_t planHitsBefore=s_replayPlanHits;
#endif
    assert(fi_build_replay(cur,prev,interp,alpha)==len);
    assert(test_walk_calls==walks);
    assert(memcmp(expected,s_replayBuf,len)==0);
    assert(s_replayAoCount==markCount && memcmp(marks,s_replayAo,markCount*sizeof(*marks))==0);
    uint32_t after[]={s_dbgPosSeen,s_dbgRewritten,s_dbgSnapUnpaired,s_dbgSnapCamera,
        s_dbgSnapGate,s_dbgSnapDecompose,s_dbgVerbatimEq,s_dbgScaleHold,s_dbgCopyClear};
    double maximaAfter[]={s_dbgMaxTrans,s_dbgMinQdot,s_dbgMaxScaleRatio,s_dbgMaxHeldScale,
                          s_dbgMaxRoundTrip,s_dbgMaxAlpha0Err};
#ifdef MP6_FI_REPLAY_TEST
    /* Plan replay intentionally resets diagnostics before replaying recorded
     * matrix actions; full-builder fallback must still preserve the old
     * diagnostic contract. */
    if (s_replayPlanHits == planHitsBefore) {
        assert(memcmp(census,after,sizeof(census))==0);
        assert(memcmp(maxima,maximaAfter,sizeof(maxima))==0);
    }
#else
    assert(memcmp(census,after,sizeof(census))==0);
    assert(memcmp(maxima,maximaAfter,sizeof(maxima))==0);
#endif
}

#ifdef MP6_FI_REPLAY_TEST
/* Keep the plan checks in the existing native builder/reference fixture.  The
 * stream generator, frozen reference, and AO-marker comparison above are the
 * contract; a second plan-only harness would duplicate them and drift. */
static void plan_fastpath_oracle(FiStream *cur, FiStream *prev)
{
    static uint8_t refBytes[65536];
    FiAoMarker refMarks[FI_AO_MAX];
    uint32_t refLen, refMarkCount, hits;
    FiStream alternate;
    int oldCurSealed = cur->sealed, oldPrevSealed = prev->sealed;
    const double alphas[] = {0.0, 0.13, 0.50, 0.91};

    puts("Replay plan private oracle start");
    test_plan_alloc_fail = 0;
    test_plan_actions_addr = (void **)&s_replayPlan.actions;
    test_plan_template_addr = (void **)&s_replayPlan.templateData;
    cur->sealed = 1;
    prev->sealed = 1;

    /* Retire both memo layers: the first successful call must record a plan,
     * not accidentally inherit an earlier one-entry image. */
    fi_pair_cache_invalidate();
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, 0.17) > 0);
    assert(s_replayPlan.valid && s_replayPlan.actionCount > 0);

    /* The full builder records the plan before commit, so commit itself must
     * reject a corrupted action even when replay preflight has not run.  Use
     * an out-of-range output offset and verify the cache is invalidated
     * instead of installing a template containing an out-of-bounds copy. */
    {
        FiReplayPlanAction savedAction = s_replayPlan.actions[0];
        uint32_t savedLength = s_replayPlan.length;
        s_replayPlan.actions[0].outOffset = savedLength;
        fi_replay_plan_commit(cur, prev, 1, savedLength);
        assert(!s_replayPlan.valid);
        s_replayPlan.actions[0] = savedAction;
        fi_pair_cache_invalidate();
        fi_replay_memo_invalidate();
        assert(fi_build_replay(cur, prev, 1, 0.17) > 0);
        assert(s_replayPlan.valid && s_replayPlan.actionCount > 0);
    }

    for (unsigned i = 0; i < sizeof(alphas) / sizeof(alphas[0]); ++i) {
        fi_replay_memo_invalidate();
        refLen = build_reference_scratch(cur, prev, 1, alphas[i],
                                         refBytes, refMarks, &refMarkCount,
                                         NULL, NULL);
        assert(refLen > 0 && refLen < sizeof(refBytes));

        fi_replay_memo_invalidate();
        hits = s_replayPlanHits;
        assert(fi_build_replay(cur, prev, 1, alphas[i]) == refLen);
        assert(s_replayPlanHits == hits + 1);
        assert(memcmp(refBytes, s_replayBuf, refLen) == 0);
        assert(s_replayAoCount == refMarkCount &&
               memcmp(refMarks, s_replayAo,
                      refMarkCount * sizeof(*refMarks)) == 0);
    }

    /* Camera cuts still take the recorded image, but each position action
     * rechecks the live cut gate while replaying it. */
    camera_cut = 1;
    fi_replay_memo_invalidate();
    refLen = build_reference_scratch(cur, prev, 1, .72, refBytes, refMarks,
                                     &refMarkCount, NULL, NULL);
    fi_replay_memo_invalidate();
    hits = s_replayPlanHits;
    assert(fi_build_replay(cur, prev, 1, .72) == refLen);
    assert(s_replayPlanHits == hits + 1);
    assert(memcmp(refBytes, s_replayBuf, refLen) == 0);
    assert(s_replayAoCount == refMarkCount &&
           memcmp(refMarks, s_replayAo,
                  refMarkCount * sizeof(*refMarks)) == 0);
    camera_cut = 0;

    /* Force action recording to fail, then force only template commit to
     * fail. Both paths must return the exact full-builder image. */
    fi_pair_cache_invalidate();
    fi_replay_memo_invalidate();
    /* Retained-capacity builds keep valid storage across invalidation.  Zero
     * the selected capacity so the injected failure is a real grow failure,
     * not a pre-grow rejection while the existing buffer is still enough. */
    s_replayPlan.actionCap = 0;
    test_plan_template_addr = NULL; /* action-array growth only */
    test_plan_alloc_fail = 1;
    compare_replay(cur, prev, 1, .31);
    assert(!s_replayPlan.valid);
    test_plan_alloc_fail = 0;
    test_plan_template_addr = (void **)&s_replayPlan.templateData;
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .31) > 0);
    assert(s_replayPlan.valid);

    fi_pair_cache_invalidate();
    fi_replay_memo_invalidate();
    s_replayPlan.templateCap = 0;
    test_plan_actions_addr = NULL; /* template growth/commit only */
    test_plan_alloc_fail = 1;
    compare_replay(cur, prev, 1, .37);
    assert(!s_replayPlan.valid);
    test_plan_alloc_fail = 0;
    test_plan_actions_addr = (void **)&s_replayPlan.actions;
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .37) > 0);
    assert(s_replayPlan.valid);

    /* Unsealed current streams are never eligible for a plan. */
    cur->sealed = 0;
    fi_replay_memo_invalidate();
    compare_replay(cur, prev, 1, .41);
    assert(!s_replayPlan.valid);
    cur->sealed = 1;

    /* Pair-cache invalidation is the central plan epoch boundary. */
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .47) > 0);
    assert(s_replayPlan.valid);
    fi_pair_cache_invalidate();
    assert(!s_replayPlan.valid);
    hits = s_replayPlanHits;
    compare_replay(cur, prev, 1, .47);
    assert(s_replayPlanHits == hits);

    /* A different stream object is an identity miss despite identical bytes,
     * keys, commands, and backing storage. */
    alternate = *cur;
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .53) > 0);
    assert(s_replayPlan.valid);
    hits = s_replayPlanHits;
    compare_replay(&alternate, prev, 1, .53);
    assert(s_replayPlanHits == hits);
    fi_pair_cache_invalidate();

    /* Diagnostics disable the plan path; compare_replay still checks exact
     * bytes/AO and the miss path retains the full-builder diagnostics. */
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .59) > 0);
    assert(s_replayPlan.valid);
    hits = s_replayPlanHits;
    s_diag = 3;
    compare_replay(cur, prev, 1, .59);
    assert(s_replayPlanHits == hits);
    s_diag = 0;

    test_plan_alloc_fail = 0;
    test_plan_actions_addr = (void **)&s_replayPlan.actions;
    test_plan_template_addr = (void **)&s_replayPlan.templateData;
    cur->sealed = oldCurSealed;
    prev->sealed = oldPrevSealed;
    assert(s_replayPlanBuilds > 0 && s_replayPlanHits > 0 &&
           s_replayPlanMisses > 0 && s_replayPlanInvalidates > 0);
    printf("Replay plan counters builds=%u hits=%u misses=%u invalidates=%u\n",
           s_replayPlanBuilds, s_replayPlanHits, s_replayPlanMisses,
           s_replayPlanInvalidates);
    puts("Replay plan byte/marker/cut/fallback oracle passed");
}

#ifdef MP6_FI_REPLAY_PLAN_RETAINED_CAPACITY
static void plan_capacity_lifecycle_oracle(FiStream *cur, FiStream *prev)
{
    void *savedActions, *savedTemplate;
    uint32_t savedActionCap, savedTemplateCap;
    int oldCurSealed = cur->sealed, oldPrevSealed = prev->sealed;

    puts("Replay plan retained-capacity oracle start");
    test_plan_actions_addr = (void **)&s_replayPlan.actions;
    test_plan_template_addr = (void **)&s_replayPlan.templateData;
    test_plan_alloc_fail = 0;
    cur->sealed = 1;
    prev->sealed = 1;

    fi_pair_cache_invalidate();
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .23) > 0);
    assert(s_replayPlan.valid && s_replayPlan.actions != NULL &&
           s_replayPlan.templateData != NULL);
    savedActions = s_replayPlan.actions;
    savedTemplate = s_replayPlan.templateData;
    savedActionCap = s_replayPlan.actionCap;
    savedTemplateCap = s_replayPlan.templateCap;
    assert(savedActionCap <= FI_REPLAY_PLAN_CACHE_MAX_ACTIONS);
    assert(savedTemplateCap <= FI_REPLAY_PLAN_CACHE_MAX_BYTES);

    /* Repeated pair invalidation must clear identity/validity without
     * reallocating the bounded arrays. */
    for (unsigned i = 0; i < 32; ++i) {
        fi_pair_cache_invalidate();
        assert(!s_replayPlan.valid);
        assert(s_replayPlan.actions == savedActions &&
               s_replayPlan.templateData == savedTemplate &&
               s_replayPlan.actionCap == savedActionCap &&
               s_replayPlan.templateCap == savedTemplateCap);
        fi_replay_memo_invalidate();
        assert(fi_build_replay(cur, prev, 1, .11 + i * .01) > 0);
        assert(s_replayPlan.valid && s_replayPlan.actions == savedActions &&
               s_replayPlan.templateData == savedTemplate &&
               s_replayPlan.actionCap == savedActionCap &&
               s_replayPlan.templateCap == savedTemplateCap);
    }

    /* Action recording and template commit failures both retain capacity and
     * return the exact full-builder image.  Force each selected capacity to
     * zero first: with retained storage, this proves the hook rejects a real
     * realloc/grow request rather than an already-satisfied request. */
    fi_pair_cache_invalidate();
    fi_replay_memo_invalidate();
    s_replayPlan.actionCap = 0;
    test_plan_template_addr = NULL;
    test_plan_alloc_fail = 1;
    compare_replay(cur, prev, 1, .31);
    assert(!s_replayPlan.valid && s_replayPlan.actions == savedActions &&
           s_replayPlan.templateData == savedTemplate &&
           s_replayPlan.actionCap == 0);
    test_plan_alloc_fail = 0;
    s_replayPlan.actionCap = savedActionCap;
    test_plan_template_addr = (void **)&s_replayPlan.templateData;
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .31) > 0);
    assert(s_replayPlan.valid && s_replayPlan.actions == savedActions &&
           s_replayPlan.templateData == savedTemplate);

    fi_pair_cache_invalidate();
    fi_replay_memo_invalidate();
    s_replayPlan.templateCap = 0;
    test_plan_actions_addr = NULL;
    test_plan_alloc_fail = 1;
    compare_replay(cur, prev, 1, .37);
    assert(!s_replayPlan.valid && s_replayPlan.actions == savedActions &&
           s_replayPlan.templateData == savedTemplate &&
           s_replayPlan.templateCap == 0);
    test_plan_alloc_fail = 0;
    s_replayPlan.templateCap = savedTemplateCap;
    test_plan_actions_addr = (void **)&s_replayPlan.actions;
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .37) > 0);
    assert(s_replayPlan.valid && s_replayPlan.actions == savedActions &&
           s_replayPlan.templateData == savedTemplate);

    /* A large non-matrix command census must not consume the matrix-action
     * ceiling.  Use distinct BP writes (the walker batches adjacent NOPs)
     * followed by one position matrix, so commandCount exceeds the ceiling
     * while the recorded matrix action count remains tiny. */
    {
        static uint8_t censusData[65536], censusExpected[65536];
        static uint32_t censusPosOff[1];
        static FiPosKey censusKeys[1];
        FiStream census = {0};
        uint32_t censusUsed = 0, referenceLen, candidateLen;
        unsigned i;

        for (i = 0; i < FI_REPLAY_PLAN_CACHE_MAX_ACTIONS + 1u; ++i) {
            censusData[censusUsed++] = GX_LOAD_BP_REG;
            censusData[censusUsed++] = 0;
            censusData[censusUsed++] = 0;
            censusData[censusUsed++] = 0;
            censusData[censusUsed++] = 0;
        }
        censusPosOff[0] = censusUsed + 5;
        censusData[censusUsed++] = GX_LOAD_XF_REG;
        censusData[censusUsed++] = 0;
        censusData[censusUsed++] = 0x0B;
        censusData[censusUsed++] = 0;
        censusData[censusUsed++] = 0;
        memset(censusData + censusUsed, 0, 48);
        censusUsed += 48;
        censusKeys[0] = (FiPosKey){0};
        censusKeys[0].valid = 1;
        census.data = censusData;
        census.size = censusUsed;
        census.posOff = censusPosOff;
        census.posCap = 1;
        census.posKey = censusKeys;
        census.keyCount = 1;
        census.sealed = 1;
        fi_walk_stream(&census);
        assert(census.walkOk && census.commandCount >
               FI_REPLAY_PLAN_CACHE_MAX_ACTIONS && census.posCount == 1);

        fi_replay_memo_invalidate();
        referenceLen = build_reference_scratch(&census, NULL, 0, .41,
                                               censusExpected, NULL, NULL,
                                               NULL, NULL);
        assert(referenceLen > 0 && referenceLen < sizeof(censusExpected));
        fi_pair_cache_invalidate();
        fi_replay_memo_invalidate();
        candidateLen = fi_build_replay(&census, NULL, 0, .41);
        assert(candidateLen == referenceLen &&
               memcmp(censusExpected, s_replayBuf, referenceLen) == 0);
        assert(s_replayPlan.valid && s_replayPlan.actionCount > 0 &&
               s_replayPlan.actionCount <= FI_REPLAY_PLAN_CACHE_MAX_ACTIONS);
        free(census.commands);
    }

    /* The bounded record helper itself must fail closed when the actual
     * matrix-action census is over the limit.  This avoids allocating a
     * multi-hundred-kilobyte synthetic stream merely to exercise the guard. */
    {
        FiCmd matrixCmd = {0};
        matrixCmd.offset = 0;
        matrixCmd.size = 53;
        matrixCmd.op = GX_LOAD_XF_REG;
        matrixCmd.xfAddr = 0;
        matrixCmd.xfCount = 12;
        fi_pair_cache_invalidate();
        fi_replay_memo_invalidate();
        /* Saturating the real action counter models the next matrix action
         * beyond the bounded record budget without fabricating commandCount
         * or allocating a giant synthetic stream. */
        s_replayPlan.actionCount = FI_REPLAY_PLAN_CACHE_MAX_ACTIONS;
        assert(!fi_replay_plan_record_action(
            &matrixCmd, 0, 0, FI_PLAN_POS,
            FI_REPLAY_PLAN_CACHE_MAX_ACTIONS + 1u));
        assert(!s_replayPlan.valid &&
               s_replayPlan.actionCount == FI_REPLAY_PLAN_CACHE_MAX_ACTIONS);
    }
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .41) > 0);
    assert(s_replayPlan.valid && s_replayPlan.actions == savedActions &&
           s_replayPlan.templateData == savedTemplate &&
           s_replayPlan.actionCap == savedActionCap &&
           s_replayPlan.templateCap == savedTemplateCap);

    /* Unsealed windows cannot commit a plan but keep the bounded storage. */
    cur->sealed = 0;
    fi_replay_memo_invalidate();
    compare_replay(cur, prev, 1, .53);
    assert(!s_replayPlan.valid && s_replayPlan.actions == savedActions &&
           s_replayPlan.templateData == savedTemplate);
    cur->sealed = 1;

    test_plan_alloc_fail = 0;
    test_plan_actions_addr = (void **)&s_replayPlan.actions;
    test_plan_template_addr = (void **)&s_replayPlan.templateData;
    cur->sealed = oldCurSealed;
    prev->sealed = oldPrevSealed;
    printf("Retained plan storage actions=%u template_bytes=%u invalidation_cycles=32\n",
           savedActionCap, savedTemplateCap);
    puts("Replay plan retained-capacity/lifecycle oracle passed");
}
#endif
#endif

static void assert_replay_memo_hit(const FiStream *cur, const FiStream *prev,
                                   int interp, double firstAlpha,
                                   double secondAlpha)
{
    FiAoMarker marks[FI_AO_MAX];
    uint32_t firstLen, secondLen, markCount;
    uint32_t builds, hits;
    /* Start from a retired entry so this proves one full build followed by a
     * hit, rather than accidentally inheriting a prior test's output. */
    fi_pair_cache_invalidate();
    firstLen = fi_build_replay(cur, prev, interp, firstAlpha);
    assert(firstLen > 0 && s_replayMemo.valid);
    memcpy(expected, s_replayBuf, firstLen);
    markCount = s_replayAoCount;
    memcpy(marks, s_replayAo, sizeof(marks));
    builds = s_replayMemoBuilds;
    hits = s_replayMemoHits;
    secondLen = fi_build_replay(cur, prev, interp, secondAlpha);
    assert(secondLen == firstLen);
    assert(s_replayMemoBuilds == builds);
    assert(s_replayMemoHits == hits + 1);
    assert(memcmp(expected, s_replayBuf, firstLen) == 0);
    assert(s_replayAoCount == markCount &&
           memcmp(marks, s_replayAo, sizeof(marks)) == 0);
}

static void assert_replay_memo_miss(const FiStream *cur, const FiStream *prev,
                                    int interp, double alpha)
{
    uint32_t builds, hits;
    fi_pair_cache_invalidate();
    assert(fi_build_replay(cur, prev, interp, alpha) > 0);
    builds = s_replayMemoBuilds;
    hits = s_replayMemoHits;
    assert(fi_build_replay(cur, prev, interp, alpha + .25) > 0);
    assert(s_replayMemoBuilds == builds + 1);
    assert(s_replayMemoHits == hits);
    assert(!s_replayMemo.valid);
}

#ifdef MP6_FI_REPLAY_TEST
static void plan_camera_memo_transition_oracle(FiStream *cur, FiStream *prev)
{
    static uint8_t expectedBytes[65536];
    uint32_t expectedLen, candidateLen;
    uint32_t builds, hits, copies;
    unsigned actionIndex, byteIndex;
    int oldCut = camera_cut;
    int oldCurSealed = cur->sealed, oldPrevSealed = prev->sealed;

    puts("Replay plan stable/unstable/stable oracle start");
    camera_cut = 0;
    cur->sealed = 1;
    prev->sealed = 1;
    fi_pair_cache_invalidate();
    fi_replay_memo_invalidate();
    assert(fi_build_replay(cur, prev, 1, .11) > 0);
    assert(s_replayMemo.valid && s_replayPlan.valid);
    builds = s_replayMemoBuilds;
    hits = s_replayMemoHits;
    copies = s_replayPlanTemplateCopies;

    /* Keep the candidate's resident plan owner live while the reference runs
     * in scratch.  If POS/NRM restoration is removed, this independent
     * comparison must fail instead of comparing the same stale buffer twice. */
    assert(s_replayBufOwner == FI_REPLAY_BUF_PLAN);
    /* Deliberately poison each resident matrix payload.  The plan replay must
     * restore the current stream's original POS/NRM bytes before the cut
     * policy decides whether to rewrite them; otherwise the negative control
     * can accidentally pass when two consecutive frames happen to match. */
    for (actionIndex = 0; actionIndex < s_replayPlan.actionCount; ++actionIndex) {
        const FiReplayPlanAction *action = &s_replayPlan.actions[actionIndex];
        uint32_t bytes = action->kind == FI_PLAN_POS ? 48u : 36u;
        for (byteIndex = 0; byteIndex < bytes; ++byteIndex)
            s_replayBuf[action->outOffset + 5 + byteIndex] =
                (uint8_t)(0xA0u + ((actionIndex * 17u + byteIndex) & 0x3Fu));
    }
    camera_cut = 1;
    expectedLen = build_reference_scratch(cur, prev, 1, .37, expectedBytes,
                                          NULL, NULL, NULL, NULL);
    assert(expectedLen > 0 && expectedLen < sizeof(expectedBytes));
    candidateLen = fi_build_replay(cur, prev, 1, .37);
    assert(candidateLen == expectedLen &&
           memcmp(expectedBytes, s_replayBuf, expectedLen) == 0);
    assert(!s_replayMemo.valid);
    assert(s_replayMemoBuilds == builds && s_replayMemoHits == hits);
    assert(s_replayPlanTemplateCopies == copies);

    /* A stable camera immediately after the cut must still use the same
     * resident plan layout, and must not inherit the cut frame's verbatim
     * matrix payloads. */
    camera_cut = 0;
    expectedLen = build_reference_scratch(cur, prev, 1, .83, expectedBytes,
                                          NULL, NULL, NULL, NULL);
    assert(expectedLen > 0 && expectedLen < sizeof(expectedBytes));
    candidateLen = fi_build_replay(cur, prev, 1, .83);
    assert(candidateLen == expectedLen &&
           memcmp(expectedBytes, s_replayBuf, expectedLen) == 0);
    assert(!s_replayMemo.valid);
    assert(s_replayPlanTemplateCopies == copies);

#ifdef MP6_BENCHMARK_PLAN
    {
        const unsigned windows = 400;
        const unsigned replays = 8;
        clock_t begin;
        double coldUs, warmUs;
        uint32_t copiesAtStart, coldCopies, warmCopies;

        /* Compare the old ownership transition (template copy on every
         * replay) with the resident-plan path.  Both runs execute the same
         * matrix actions and are byte-checked by the oracle above. */
        copiesAtStart = s_replayPlanTemplateCopies;
        begin = clock();
        for (unsigned window = 0; window < windows; ++window) {
            for (unsigned replay = 0; replay < replays; ++replay) {
                s_replayMemo.valid = 0;
                s_replayBufOwner = FI_REPLAY_BUF_MEMO;
                assert(fi_build_replay(cur, prev, 1,
                                       ((double)replay + .5) / replays) > 0);
            }
        }
        coldUs = (double)(clock() - begin) * 1000000.0 / CLOCKS_PER_SEC /
                 (double)(windows * replays);
        coldCopies = s_replayPlanTemplateCopies - copiesAtStart;

        begin = clock();
        for (unsigned window = 0; window < windows; ++window) {
            for (unsigned replay = 0; replay < replays; ++replay) {
                s_replayMemo.valid = 0;
                s_replayBufOwner = FI_REPLAY_BUF_PLAN;
                assert(fi_build_replay(cur, prev, 1,
                                       ((double)replay + .5) / replays) > 0);
            }
        }
        warmUs = (double)(clock() - begin) * 1000000.0 / CLOCKS_PER_SEC /
                 (double)(windows * replays);
        warmCopies = s_replayPlanTemplateCopies - copiesAtStart - coldCopies;
        printf("PLAN_OWNER replays=%u cold_us=%.3f warm_us=%.3f "
               "cold_template_copies=%u warm_template_copies=%u\n",
               replays, coldUs, warmUs, coldCopies, warmCopies);
    }
#endif

    camera_cut = oldCut;
    cur->sealed = oldCurSealed;
    prev->sealed = oldPrevSealed;
    puts("Replay plan stable/unstable/stable oracle passed");
}
#endif

#ifdef MP6_BENCHMARK_MEMO
static void memo_component_benchmark(const FiStream *cur, const FiStream *prev)
{
    static const unsigned replayCounts[] = {1, 2, 4, 8};
    const unsigned windows = 1000;
    for (unsigned ci = 0; ci < sizeof(replayCounts) / sizeof(replayCounts[0]); ++ci) {
        const unsigned count = replayCounts[ci];
        double fullUs, memoUs;
        clock_t begin;
        fi_replay_memo_invalidate();
        begin = clock();
        for (unsigned window = 0; window < windows; ++window) {
            for (unsigned replay = 0; replay < count; ++replay) {
                fi_replay_memo_invalidate();
                assert(fi_build_replay(cur, prev, 0,
                                       ((double)replay + .5) / count) > 0);
            }
        }
        fullUs = (double)(clock() - begin) * 1000000.0 / CLOCKS_PER_SEC /
                 (double)(windows * count);
        fi_replay_memo_invalidate();
        begin = clock();
        for (unsigned window = 0; window < windows; ++window) {
            fi_replay_memo_invalidate();
            for (unsigned replay = 0; replay < count; ++replay)
                assert(fi_build_replay(cur, prev, 0,
                                       ((double)replay + .5) / count) > 0);
        }
        memoUs = (double)(clock() - begin) * 1000000.0 / CLOCKS_PER_SEC /
                 (double)(windows * count);
        printf("MEMO_COMPONENT_ONLY replays=%u full_us=%.3f memo_us=%.3f\n",
               count, fullUs, memoUs);
    }
}
#endif

int main(void) {
    s_diag=0;
    malformed_lengths();
    walker_oracle_cases();
#ifdef MP6_BENCHMARK_WALK
    walker_benchmark();
#endif
    for (unsigned trial=0;trial<40;++trial) {
        used=positions=markerCount=0;
        mark(0);
        for (unsigned n=0;n<80;++n) {
            for(unsigned k=0;k<(n*13+trial)%91;++k) {
                current[used++]=GX_NOP;
                if(n%10==0 && k==3)mark(1);
                if(n%10==0 && k==8)mark(2);
            }
            matrix(n%10,n%8);
            vertices(n);
            if(n%9==0) { current[used++]=0x61;put32(0x52000000u|((n&1)?1u<<11:0)); }
            if(n%7==0) { aurora(GX_AURORA_BEGIN_OFFSCREEN,8);mark(0);matrix(n%10,1);aurora(GX_AURORA_END_OFFSCREEN,0); }
            if(n%5==0) aurora(GX_AURORA_LOAD_COPY_DEST,8);
            if(n%11==0) aurora(GX_AURORA_DESTROY_TEXOBJ,4);
            if(n%13==0) aurora(GX_AURORA_REQUEST_DEPTH_SNAPSHOT,0);
        }
        for(unsigned i=0;i<20;++i) current[used++]=GX_NOP;
        mark(0);
        assert(used<sizeof(current) && positions<256);
        memcpy(previous,current,used);
        for(unsigned n=0;n<positions;++n) {
            unsigned variant=n%8;
            if(variant==1 || variant==4) wr_bef32(previous+offsets[n]+12,variant==4 ? -100.f : -1.f);
            if(variant==7) wr_bef32(previous+offsets[n],.9f);
            if(trial%2) varied_pair(n,trial);
        }
        FiStream cur={0},prev={0};
        cur.data=current;cur.size=used;cur.posCount=positions;cur.posOff=offsets;cur.posKey=keys;cur.prevPos=pairs;
        prev=cur;prev.data=previous;
        cur.aoCount=markerCount;memcpy(cur.ao,markers,markerCount*sizeof(*markers));
        cur.keyCount=positions;cur.posCap=256;
        prev.keyCount=positions;prev.walkOk=1;
        fi_walk_stream(&cur);
        assert(cur.walkOk && cur.commandCount<used/4);
        for(int interp=0;interp<2;++interp) for(unsigned step=0;step<5;++step) {
            double alpha=step*.25;
            unsigned prepared=test_prepare_calls;
            compare_replay(&cur,&prev,interp,alpha);
            if(interp)assert(s_dbgRewritten>0);
            if(!interp || (step>0 && positions<=FI_PAIR_CACHE_MAX))
                assert(test_prepare_calls==prepared);
        }
        if (trial == 0) {
#ifdef MP6_FI_REPLAY_TEST
            plan_fastpath_oracle(&cur, &prev);
#ifdef MP6_FI_REPLAY_PLAN_RETAINED_CAPACITY
            plan_capacity_lifecycle_oracle(&cur, &prev);
#endif
#endif
            uint32_t builds, hits;
            /* Freeze both endpoints to make the safe static candidate. Both
             * interpolation modes must reuse the same command/marker image,
             * while switching modes retires the entry. */
            memcpy(previous, current, used);
            for (unsigned n = 0; n < positions; ++n) keys[n].camera = 0;
            prev.data = previous;
            assert_replay_memo_hit(&cur, &prev, 0, .00, .73);
            assert_replay_memo_hit(&cur, &prev, 1, .00, .91);
            builds = s_replayMemoBuilds;
            hits = s_replayMemoHits;
            assert(fi_build_replay(&cur, &prev, 0, .25) > 0);
            assert(s_replayMemoBuilds == builds + 1 && s_replayMemoHits == hits);

            /* A camera cut is not a static-safe hit even when its endpoint
             * bytes are equal; the hit path rechecks the bounded camera mask. */
            fi_pair_cache_invalidate();
            assert(fi_build_replay(&cur, &prev, 1, .10) > 0);
            builds = s_replayMemoBuilds;
            hits = s_replayMemoHits;
            camera_cut = 1;
            assert(fi_build_replay(&cur, &prev, 1, .20) > 0);
            assert(s_replayMemoBuilds == builds + 1 && s_replayMemoHits == hits);
            assert(!s_replayMemo.valid);
            camera_cut = 0;

#ifdef MP6_FI_REPLAY_TEST
            plan_camera_memo_transition_oracle(&cur, &prev);
#endif

            /* Pair-cache allocation failure remains an exact fallback, and
             * equal endpoints are still safe to memoize without that scratch. */
            fi_pair_cache_invalidate();
            test_cache_grow_fail = 1;
            assert(fi_build_replay(&cur, &prev, 1, .30) > 0);
            assert(s_replayMemo.valid);
            builds = s_replayMemoBuilds;
            hits = s_replayMemoHits;
            test_cache_grow_fail = 0;
            assert(fi_build_replay(&cur, &prev, 1, .40) > 0);
            assert(s_replayMemoBuilds == builds && s_replayMemoHits == hits + 1);
#ifdef MP6_BENCHMARK_MEMO
            memo_component_benchmark(&cur, &prev);
#endif
            /* A failed walk cannot hit the previous static image, even when
             * the stream storage and stream object are reused afterward. */
            current[used - 1] = 0xff;
            fi_walk_stream(&cur);
            assert(fi_build_replay(&cur, &prev, 1, .50) == 0);
            assert(!s_replayMemo.valid);
            current[used - 1] = GX_NOP;
            fi_walk_stream(&cur);
            assert(cur.walkOk);
            for (unsigned n = 0; n < positions; ++n)
                keys[n].camera = (n % 8u == 6u) ? 1 : 0;
        }
        if (trial & 1u) {
            /* At least one moving/gated/invalid pair is present in every odd
             * trial; zero rewrites alone must never admit a memo entry. */
            assert_replay_memo_miss(&cur, &prev, 1, .11);
        }
        /* Allocation failure is exact fallback, never a dropped rewrite. */
        fi_pair_cache_invalidate();test_cache_grow_fail=1;
        compare_replay(&cur,&prev,1,.37);assert(!s_pairCacheCount);
        test_cache_grow_fail=0;compare_replay(&cur,&prev,1,.37);
        /* A camera cut is still consulted after endpoints have been cached. */
        camera_cut=1;compare_replay(&cur,&prev,1,.7);camera_cut=0;
        /* Re-walking or re-pairing the same storage must retire old endpoints. */
        varied_pair(1,trial+200);
        fi_walk_stream(&cur);compare_replay(&cur,&prev,1,.61);
        cur.prevPos=NULL;cur.prevPosCap=0;
        fi_pair_stream(&cur,&prev);compare_replay(&cur,&prev,1,.43);
        free(cur.prevPos);cur.prevPos=pairs;
        FiStream alternate=prev;alternate.data=current;
        compare_replay(&cur,&alternate,1,.5);
        compare_replay(&cur,&prev,1,.5);
        if(trial==0) {
            s_diag=3;compare_replay(&cur,&prev,1,.35);s_diag=0;
        }
        FiCmd *commands=cur.commands;
        uint32_t capacity=cur.commandCap;
        cur.ao[0].offset=offsets[0]+1; /* inside an XF command, not a boundary */
        fi_walk_stream(&cur);
        assert(!cur.walkOk && fi_build_replay(&cur,&prev,1,.5)==0);
        cur.ao[0]=markers[0];
        fi_walk_stream(&cur);
        assert(cur.walkOk && cur.commands==commands && cur.commandCap==capacity);
        current[used-1]=0xff; /* malformed tail must still fail closed */
        fi_walk_stream(&cur);
        assert(fi_build_replay(&cur,&prev,1,.5)==0);
#ifdef MP6_BENCHMARK
        current[used-1]=GX_NOP;
        fi_walk_stream(&cur);
        fi_pair_cache_invalidate();
        fi_build_replay(&cur,&prev,1,.5);
        double costs[2];
        for (unsigned order=0;order<2;++order) {
            unsigned candidate=(order+trial)%2;
            clock_t begin=clock();
            for (unsigned n=0;n<200;++n) {
                uint32_t bytes=candidate ? fi_build_replay(&cur,&prev,1,.5) :
                                          fi_build_replay_reference(&cur,&prev,1,.5);
                assert(bytes>0);
            }
            costs[candidate]=(double)(clock()-begin)*1000000./CLOCKS_PER_SEC/200.;
        }
        printf("REPLAY trial=%u reference_us=%.3f candidate_us=%.3f\n",trial,costs[0],costs[1]);
#ifdef MP6_BENCHMARK_ENDPOINTS
        for(unsigned windowReplays=1;windowReplays<=8;windowReplays*=2) {
            for(unsigned order=0;order<2;++order) {
                unsigned candidate=(order+trial)%2;
                clock_t begin=clock();
                for(unsigned window=0;window<2000;++window) {
                    fi_pair_cache_invalidate();
                    for(unsigned r=0;r<windowReplays;++r) {
                        uint32_t bytes=candidate ? fi_build_replay(&cur,&prev,1,(r+.5)/windowReplays) :
                                                  fi_build_replay_reference(&cur,&prev,1,(r+.5)/windowReplays);
                        assert(bytes>0);
                    }
                }
                costs[candidate]=(double)(clock()-begin)*1000000./CLOCKS_PER_SEC/2000./windowReplays;
            }
            printf("WINDOW trial=%u replays=%u reference_us=%.3f candidate_us=%.3f\n",
                   trial,windowReplays,costs[0],costs[1]);
        }
#endif
#endif
        free(cur.commands);
    }
    assert(s_pairCache && s_pairState);
    mp6_fi_savestate_reset();
    assert(!s_pairCache && !s_pairState && !s_pairCacheCur && !s_pairCachePrev);
    assert(!s_pairCacheCount && !s_pairCacheCap && !s_pairStateCap && model_resets==1);
    assert(!s_replayMemo.valid && !s_replayBuf && !s_replayCap);
#ifdef MP6_FI_REPLAY_TEST
    /* Savestate reset is the lifecycle release point, including when normal
     * pair invalidation retains bounded plan capacity. */
    assert(!s_replayPlan.valid && !s_replayPlan.actions &&
           !s_replayPlan.templateData && !s_replayPlan.actionCap &&
           !s_replayPlan.templateCap);
    assert(s_replayBufOwner == FI_REPLAY_BUF_NONE);
#endif
    puts("400 exact replay byte/marker comparisons passed");
    return 0;
}
