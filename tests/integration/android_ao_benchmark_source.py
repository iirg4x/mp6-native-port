"""Pure source transforms for an isolated, fixed-seed Android board benchmark."""

MARKER = 'MP6_ANDROID_AO_BENCHMARK_SOURCE'


def validate_window(warmup_ticks=600, sample_ticks=1200):
    """Bound the experiment independently of its external wall-clock guard."""
    if type(warmup_ticks) is not int or not 120 <= warmup_ticks <= 6000:
        raise ValueError('warmup ticks must be an integer in 120..6000')
    if type(sample_ticks) is not int or not 120 <= sample_ticks <= 4000:
        raise ValueError('sample ticks must be an integer in 120..4000')
    if warmup_ticks + sample_ticks > 10000:
        raise ValueError('board window must end by age 10000')


def replace_once(text, old, new):
    count = text.count(old)
    if count != 1:
        raise ValueError(f'expected exactly one source anchor, found {count}: {old!r}')
    return text.replace(old, new, 1)


def transform_source(kind, text, warmup_ticks=600, sample_ticks=1200):
    """Return instrumented source without reading or changing any files."""
    validate_window(warmup_ticks, sample_ticks)
    if MARKER in text:
        raise ValueError('source is already an Android AO benchmark transform')
    if kind == 'frand':
        if 'mp6_benchmark_seed_board' in text:
            raise ValueError('source already contains benchmark RNG instrumentation')
        text = replace_once(text, 'seed = OSGetTime();',
                            'seed = 0x12345678u; /* isolated benchmark seed */')
        text += '\nvoid mp6_benchmark_seed_board(void) { frand_seed = 0xA341316Cu; }\n'
    elif kind == 'world01':
        if 'mp6_benchmark_seed_board' in text:
            raise ValueError('source already contains benchmark board instrumentation')
        anchor = 'void MB1_Create(void)\n{'
        text = replace_once(text, anchor, anchor +
            '\n    extern void mp6_benchmark_seed_board(void);\n    mp6_benchmark_seed_board();')
    elif kind == 'bridge':
        if '[ANDROID-AO-BENCH' in text or '[ENGINE-BENCH]' in text:
            raise ValueError('source already contains a board measurement window')
        text = replace_once(text, '    aurora_shutdown();\n    exit(0);', '''    aurora_shutdown();
#if defined(__ANDROID__)
    /* Isolated Android benchmark only: explicit launcher/UI/renderer cleanup
     * above has completed. Avoid process-wide CRT destructors racing Android
     * HWUI teardown after the measured window; this is not a player lifecycle
     * change. Preserve all diagnostic output before terminating the process. */
    fflush(NULL);
    _Exit(0);
#else
    exit(0);
#endif''')
        anchor = '    if (tEntry > 0) g_phSamples++;'
        instrumentation = r'''
    /* A single native board window; the host retains the thermal/time guard. */
    {
        extern unsigned mp6_diag_board_ticks(void);
        extern int mp6_diag_board_no(void);
        const unsigned qaBoardAge = mp6_diag_board_ticks();
        static double qaTotal[5];
        static unsigned qaCount, qaFirstAge, qaLastAge;
        static int qaInvalid;
        static uint64_t qaGpuRequest;
        static long qaGpuRequestTick;
        if (!qaInvalid && mp6_diag_board_no() == 0 &&
                qaBoardAge >= @WARMUP@ && qaBoardAge < @END@) {
            if (qaBoardAge != @WARMUP@ + qaCount || tEntry <= 0 || tPrevReturn <= 0) {
                fprintf(stderr, "[ANDROID-AO-BENCH-INVALID] expected_age=%u actual_age=%u ticks=%u\n",
                        @WARMUP@ + qaCount, qaBoardAge, qaCount);
                fflush(stderr);
                qaInvalid = 1;
                mp6_max_ticks = mp6_tick_count + 60;
            } else {
                if (qaCount == 0) {
                    qaFirstAge = qaBoardAge;
                    fprintf(stderr, "[ANDROID-AO-BENCH-BEGIN] first_age=%u target_last_age=%u ticks=%u global_tick=%ld\n",
                            qaFirstAge, @LAST@, @COUNT@, mp6_tick_count);
                    fflush(stderr);
                }
                qaLastAge = qaBoardAge;
                qaTotal[0] += (double)(tEntry - tPrevReturn) / 1e6;
                qaTotal[1] += (double)(tEndFrame - tEntry) / 1e6;
                qaTotal[2] += (double)(tSeal - tEndFrame) / 1e6;
                qaTotal[3] += (double)(g_phLastReturnNs - tThrottleOut) / 1e6;
                qaTotal[4] += (double)(g_phLastReturnNs - tPrevReturn) / 1e6;
                if (++qaCount == @COUNT@) {
                    fprintf(stderr, "[ANDROID-AO-BENCH] ticks=%u first_age=%u last_age=%u global_tick=%ld game=%.6f submit=%.6f seal=%.6f post=%.6f frame=%.6f\n",
                            qaCount, qaFirstAge, qaLastAge, mp6_tick_count,
                            qaTotal[0] / qaCount, qaTotal[1] / qaCount,
                            qaTotal[2] / qaCount, qaTotal[3] / qaCount, qaTotal[4] / qaCount);
                    /* Request one worker-owned history snapshot. Never sort or
                     * wait for GPU readback on the game thread. Its response
                     * has one coherent window for means AND percentiles. */
                    qaGpuRequestTick = mp6_tick_count;
                    qaGpuRequest = aurora_gpu_timing_snapshot_request();
                    fprintf(stderr, "[ANDROID-AO-GPU-REQUEST] id=%llu global_tick=%ld\n",
                            (unsigned long long)qaGpuRequest, qaGpuRequestTick);
                    if (!qaGpuRequest) {
                        fprintf(stderr, "[ANDROID-AO-BENCH-INVALID] GPU snapshot unsupported or disabled\n");
                        qaInvalid = 1;
                    }
                    fflush(stderr);
                    mp6_max_ticks = mp6_tick_count + 60;
                }
            }
        }
        if (!qaInvalid && qaGpuRequest) {
            AuroraGpuTimingSnapshot qaSnapshot;
            if (mp6_tick_count - qaGpuRequestTick < 30 &&
                    aurora_gpu_timing_snapshot_read(qaGpuRequest, &qaSnapshot)) {
                    const AuroraGpuStats qaGpu = qaSnapshot.stats;
                    fprintf(stderr, "[ENGINE-GPU-SNAPSHOT] id=%llu generation=%llu global_tick=%ld "
                            "span_median=%.6f span_p95=%.6f span_max=%.6f "
                            "between_median=%.6f between_p95=%.6f between_max=%.6f "
                            "ao_mean=%.6f ao_median=%.6f ao_p95=%.6f ao_max=%.6f\n",
                            (unsigned long long)qaSnapshot.requestId,
                            (unsigned long long)qaSnapshot.generation, mp6_tick_count,
                            qaSnapshot.spanMedianMs, qaSnapshot.spanP95Ms, qaGpu.spanMaxMs,
                            qaSnapshot.betweenMedianMs, qaSnapshot.betweenP95Ms, qaGpu.betweenMaxMs,
                            qaSnapshot.aoAverageMs, qaSnapshot.aoMedianMs,
                            qaSnapshot.aoP95Ms, qaSnapshot.aoMaxMs);
                    fprintf(stderr, "[ENGINE-GPU] status=%u samples=%u dropped=%u span=%.6f between=%.6f\n",
                            qaGpu.status, qaGpu.sampleCount, qaGpu.droppedFrames,
                            qaGpu.spanAverageMs, qaGpu.betweenAverageMs);
                    for (unsigned qaRow = 0; qaRow < qaGpu.rowCount && qaRow < AURORA_GPU_STAT_ROWS; ++qaRow)
                        fprintf(stderr, "[ENGINE-GPU-PASS] %s avg=%.6f max=%.6f\n",
                                qaGpu.rows[qaRow].name, qaGpu.rows[qaRow].averageMs, qaGpu.rows[qaRow].maxMs);
                    fflush(stderr);
                    qaGpuRequest = 0;
            } else if (mp6_tick_count - qaGpuRequestTick >= 30) {
                fprintf(stderr, "[ANDROID-AO-BENCH-INVALID] GPU snapshot timed out id=%llu\n",
                        (unsigned long long)qaGpuRequest);
                fflush(stderr);
                qaInvalid = 1;
                qaGpuRequest = 0;
            }
        }
    }
'''
        for key, value in {'WARMUP': warmup_ticks, 'END': warmup_ticks + sample_ticks,
                           'LAST': warmup_ticks + sample_ticks - 1, 'COUNT': sample_ticks}.items():
            instrumentation = instrumentation.replace('@' + key + '@', str(value) + 'u')
        text = replace_once(text, anchor, anchor + instrumentation)
        text = '#include <aurora/gpu_timing_snapshot.h>\n' + text
    else:
        raise ValueError('unknown benchmark source kind: ' + str(kind))
    return '/* ' + MARKER + ': local diagnostic only; never a release asset. */\n' + text
