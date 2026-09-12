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

static uint8_t current[65536], previous[65536], expected[65536];
static uint32_t offsets[256];
static int32_t pairs[256];
static FiPosKey keys[256];
static uint32_t used, positions;
static FiAoMarker markers[FI_AO_MAX];
static uint32_t markerCount;

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

static void compare_replay(const FiStream *cur, const FiStream *prev, int interp,
                           double alpha) {
    if (s_replayCap) memset(s_replayBuf,0xA5,s_replayCap);
    uint32_t len=fi_build_replay_reference(cur,prev,interp,alpha);
    assert(len>0 && len<=used);
    memcpy(expected,s_replayBuf,len);
    FiAoMarker marks[FI_AO_MAX];
    uint32_t markCount=s_replayAoCount;
    memcpy(marks,s_replayAo,sizeof(marks));
    uint32_t census[]={s_dbgPosSeen,s_dbgRewritten,s_dbgSnapUnpaired,s_dbgSnapCamera,
        s_dbgSnapGate,s_dbgSnapDecompose,s_dbgVerbatimEq,s_dbgScaleHold,s_dbgCopyClear};
    double maxima[]={s_dbgMaxTrans,s_dbgMinQdot,s_dbgMaxScaleRatio,s_dbgMaxHeldScale,
                     s_dbgMaxRoundTrip,s_dbgMaxAlpha0Err};
    memset(s_replayBuf,0x5A,s_replayCap);
    unsigned walks=test_walk_calls;
    assert(fi_build_replay(cur,prev,interp,alpha)==len);
    assert(test_walk_calls==walks);
    assert(memcmp(expected,s_replayBuf,len)==0);
    assert(s_replayAoCount==markCount && memcmp(marks,s_replayAo,markCount*sizeof(*marks))==0);
    uint32_t after[]={s_dbgPosSeen,s_dbgRewritten,s_dbgSnapUnpaired,s_dbgSnapCamera,
        s_dbgSnapGate,s_dbgSnapDecompose,s_dbgVerbatimEq,s_dbgScaleHold,s_dbgCopyClear};
    double maximaAfter[]={s_dbgMaxTrans,s_dbgMinQdot,s_dbgMaxScaleRatio,s_dbgMaxHeldScale,
                          s_dbgMaxRoundTrip,s_dbgMaxAlpha0Err};
    assert(memcmp(census,after,sizeof(census))==0);
    assert(memcmp(maxima,maximaAfter,sizeof(maxima))==0);
}

int main(void) {
    s_diag=0;
    malformed_lengths();
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
    puts("400 exact replay byte/marker comparisons passed");
    return 0;
}
