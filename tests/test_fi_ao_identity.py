"""CPU regressions for frame-interpolation HSF/AO object identity pairing.

The fixture compiles the production walker/pairer (through the same extraction
used by ``test_frame_replay_batch``) and drives it with small retained streams.
The stream payloads are real 48-byte matrix byte blocks, so the object-coherence
checks exercise the same ``memcmp`` path used by replay rather than a key-only
facade.
"""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import build

try:
    from tests.test_frame_replay_batch import replay_test_source
except ImportError:  # unittest discovery can put tests/ directly on sys.path.
    from test_frame_replay_batch import replay_test_source


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/gx/frame_interp.c"


CPP_PAIR_TEST = r"""
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp6_fi_model.h"

long mp6_tick_count;

int mp6_console_cvar_get(int id, int value) {
    (void)id;
    return value;
}

int mp6_fi_model_camera_stable(int camera) {
    (void)camera;
    return 1;
}

int mp6_enh_unlocked_fps(void) { return 0; }
int mp6_tick_interpolation_possible(void) { return 0; }
int mp6_fi_capture_camera_id(void) { return 0; }
int mp6_fi_capture_context_next(int *camera, int *model, uint32_t *generation,
                                uint16_t *sub, uint16_t *ordinal) {
    (void)camera; (void)model; (void)generation; (void)sub; (void)ordinal;
    return 0;
}
void mp6_ao_begin_frame(void) {}
void aurora_gx_set_drain_capture(void (*fn)(const void *, uint32_t, void *), void *user) {
    (void)fn; (void)user;
}
void aurora_gx_export_vtx_layout(uint8_t *desc, uint8_t *cnt, uint8_t *type) {
    (void)desc; (void)cnt; (void)type;
}
void mp6_fi_model_reset(void) {}
void mp6_fi_model_snapshot(void) {}
void mp6_fi_model_animlog(long tick) { (void)tick; }
uint64_t mp6_host_monotonic_ns(void) { return 0; }
int mp6_fi_deformed_list(const Mp6FiDeformed **out) { *out = NULL; return 0; }
void mp6_fi_deformed_reset(void) {}
int mp6_launcher_cfg_smooth_frames(void) { return -1; }

typedef struct {
    uint16_t sub;
    uint16_t ordinal;
    uint8_t payload;
    int16_t model;
    int8_t camera;
    uint32_t generation;
} TestRec;

typedef struct {
    FiStream stream;
    uint8_t data[32u * 48u];
    uint32_t offsets[32];
    FiPosKey keys[32];
} TestStream;

typedef struct {
    FiStream stream;
    uint8_t data[128];
    uint32_t offsets[2];
    FiPosKey keys[2];
    FiCmd commands[2];
} ReplayStream;

static TestRec rec_ctx(uint16_t sub, uint16_t ordinal, uint8_t payload,
                       int16_t model, int8_t camera, uint32_t generation) {
    TestRec value;
    value.sub = sub;
    value.ordinal = ordinal;
    value.payload = payload;
    value.model = model;
    value.camera = camera;
    value.generation = generation;
    return value;
}

static TestRec rec(uint16_t sub, uint16_t ordinal, uint8_t payload) {
    return rec_ctx(sub, ordinal, payload, 17, 3, 9);
}

static FiPosKey key_from_rec(const TestRec *record) {
    FiPosKey value;
    memset(&value, 0, sizeof(value));
    value.valid = 1;
    value.model = record->model;
    value.camera = record->camera;
    value.generation = record->generation;
    value.sub = record->sub;
    value.ordinal = record->ordinal;
    return value;
}

/* Distinct, non-zero bytes model an actual matrix payload. Equal payload tags
 * produce bit-identical 48-byte blocks, as the visible/AO copy contract needs. */
static void fill_payload(uint8_t *dst, uint8_t tag) {
    unsigned i;
    for (i = 0; i < 48; ++i)
        dst[i] = (uint8_t)(tag ^ (uint8_t)(i * 29u) ^ (uint8_t)(i >> 1));
}

static void init_stream(TestStream *out, const TestRec *records, uint32_t count) {
    uint32_t i;
    memset(out, 0, sizeof(*out));
    assert(count <= 32);
    out->stream.data = out->data;
    out->stream.size = count * 48u;
    out->stream.posOff = out->offsets;
    out->stream.posCount = count;
    out->stream.posKey = out->keys;
    out->stream.keyCount = count;
    out->stream.walked = 1;
    out->stream.walkOk = 1;
    for (i = 0; i < count; ++i) {
        out->offsets[i] = i * 48u;
        out->keys[i] = key_from_rec(&records[i]);
        fill_payload(out->data + out->offsets[i], records[i].payload);
    }
}

static void dispose_stream(TestStream *stream) {
    free(stream->stream.prevPos);
    stream->stream.prevPos = NULL;
    stream->stream.prevPosCap = 0;
}

static void dispose_replay_stream(ReplayStream *stream) {
    free(stream->stream.prevPos);
    stream->stream.prevPos = NULL;
    stream->stream.prevPosCap = 0;
}

static void assert_map(const TestStream *current, const int32_t *expected,
                       uint32_t count) {
    uint32_t i;
    assert(current->stream.prevPos != NULL);
    for (i = 0; i < count; ++i)
        assert(current->stream.prevPos[i] == expected[i]);
}

static void be32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void init_replay_stream(ReplayStream *out, float tx) {
    FiMtx matrix = {{{1, 0, 0, tx}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    TestRec main_record = rec(0x4010u, 0, 0x5a);
    TestRec aux_record = rec(0x8010u, 0, 0x5a);
    const uint32_t command_size = 53;
    const uint32_t addresses[] = {0, 12};
    uint32_t i;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < 2; ++i) {
        uint32_t base = i * command_size;
        const TestRec *record = i == 0 ? &main_record : &aux_record;
        out->data[base] = GX_LOAD_XF_REG;
        be32(out->data + base + 1, (11u << 16) | addresses[i]);
        fi_write_mtx(out->data + base + 5, &matrix);
        out->offsets[i] = base + 5;
        out->keys[i] = key_from_rec(record);
        out->commands[i].offset = base;
        out->commands[i].size = command_size;
        out->commands[i].op = GX_LOAD_XF_REG;
        out->commands[i].auroraSub = 0xffffu;
        out->commands[i].bpReg = 0xffu;
        out->commands[i].xfAddr = addresses[i];
        out->commands[i].xfCount = 12;
    }
    out->stream.data = out->data;
    out->stream.size = 2 * command_size;
    out->stream.posOff = out->offsets;
    out->stream.posCount = 2;
    out->stream.posKey = out->keys;
    out->stream.keyCount = 2;
    out->stream.walked = 1;
    out->stream.walkOk = 1;
    out->stream.commands = out->commands;
    out->stream.commandCount = 2;
    out->stream.commandCap = 2;
}

static void seal_stream(TestStream *stream, long tick) {
    stream->stream.sealed = 1;
    stream->stream.tick = tick;
}

static void test_census_rotation_and_lifecycle(void) {
    const TestRec records[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11),
    };
    const int32_t expected[] = {0, 1};
    TestStream predecessor, previous, current, alternate, next, full;
    unsigned before;

    init_stream(&predecessor, records, 2);
    init_stream(&previous, records, 2);
    init_stream(&current, records, 2);
    init_stream(&alternate, records, 2);
    init_stream(&next, records, 2);
    init_stream(&full, records, 2);
    seal_stream(&predecessor, 10);
    seal_stream(&previous, 11);
    seal_stream(&current, 12);
    seal_stream(&alternate, 13);
    seal_stream(&next, 14);
    seal_stream(&full, 15);

    fi_census_owner_invalidate();
    s_fiTableBuilds = 0;
    fi_pair_stream(&previous.stream, &predecessor.stream);
    assert(s_fiTableBuilds == 4);
    assert_map(&previous, expected, 2);

    /* An unexpected predecessor forces all four tables and transfers owner. */
    fi_pair_stream(&alternate.stream, &predecessor.stream);
    assert(s_fiTableBuilds == 8);
    /* Restore `previous` as owner, then the real two-slot rotation reuses it. */
    fi_pair_stream(&current.stream, &predecessor.stream);
    assert(s_fiTableBuilds == 12);
    before = s_fiTableBuilds;
    fi_pair_stream(&next.stream, &current.stream);
    assert(s_fiTableBuilds == before + 2);
    assert_map(&next, expected, 2);

    /* Forced full rebuild has identical maps to the two-table rotation. */
    fi_census_owner_invalidate();
    before = s_fiTableBuilds;
    fi_pair_stream(&full.stream, &current.stream);
    assert(s_fiTableBuilds == before + 4);
    assert_map(&full, expected, 2);

    dispose_stream(&predecessor);
    dispose_stream(&previous);
    dispose_stream(&current);
    dispose_stream(&alternate);
    dispose_stream(&next);
    dispose_stream(&full);
    fi_census_owner_invalidate();
}

static void test_census_lifecycle_rotation(void) {
    ReplayStream predecessor, previous, current;
    const FiStream *ownerStream;
    const FiPosKey *ownerKeys;
    long ownerTick;
    uint32_t ownerCount;
    unsigned before;
    init_replay_stream(&predecessor, 0.0f);
    init_replay_stream(&previous, 0.0f);
    init_replay_stream(&current, 0.0f);
    current.stream.posCap = 2;
    predecessor.stream.sealed = 1;
    predecessor.stream.tick = 20;
    previous.stream.sealed = 1;
    previous.stream.tick = 21;
    current.stream.sealed = 1;
    current.stream.tick = 22;

    fi_census_owner_invalidate();
    s_fiTableBuilds = 0;
    fi_pair_stream(&previous.stream, &predecessor.stream);
    assert(s_fiTableBuilds == 4);
    before = s_fiTableBuilds;
    ownerStream = s_censusOwner.stream;
    ownerTick = s_censusOwner.tick;
    ownerKeys = s_censusOwner.posKey;
    ownerCount = s_censusOwner.keyCount;

    /* Reset/walk the other bank while retaining the sealed previous owner. */
    fi_stream_reset(&current.stream);
    assert(s_censusOwner.valid && s_censusOwner.stream == ownerStream &&
           s_censusOwner.tick == ownerTick && s_censusOwner.posKey == ownerKeys &&
           s_censusOwner.keyCount == ownerCount);
    init_replay_stream(&current, 0.0f);
    current.stream.sealed = 1;
    current.stream.tick = 22;
    current.stream.posCap = 2;
    fi_walk_stream(&current.stream);
    assert(current.stream.walkOk);
    assert(s_censusOwner.valid && s_censusOwner.stream == ownerStream &&
           s_censusOwner.tick == ownerTick && s_censusOwner.posKey == ownerKeys &&
           s_censusOwner.keyCount == ownerCount);
    fi_pair_stream(&current.stream, &previous.stream);
    assert(s_fiTableBuilds == before + 2);

    /* Re-walking the owner itself invalidates it, so this pair is full. */
    current.keys[0].ordinal = 1;
    fi_walk_stream(&current.stream);
    assert(current.stream.walkOk);
    before = s_fiTableBuilds;
    fi_pair_stream(&current.stream, &previous.stream);
    assert(s_fiTableBuilds == before + 4);
    assert(current.stream.prevPos[0] < 0 && current.stream.prevPos[1] < 0);

    /* Alternate the two storage banks again: the current owner survives
     * resetting/walking the other bank and is reused in the reverse direction. */
    fi_stream_reset(&previous.stream);
    init_replay_stream(&previous, 0.0f);
    previous.stream.sealed = 1;
    previous.stream.tick = 23;
    previous.stream.posCap = 2;
    fi_walk_stream(&previous.stream);
    assert(previous.stream.walkOk);
    before = s_fiTableBuilds;
    fi_pair_stream(&previous.stream, &current.stream);
    assert(s_fiTableBuilds == before + 2);
    assert(previous.stream.prevPos[0] < 0 && previous.stream.prevPos[1] < 0);

    dispose_replay_stream(&predecessor);
    dispose_replay_stream(&previous);
    dispose_replay_stream(&current);
    fi_census_owner_invalidate();
}

static void test_census_unsealed_fallback(void) {
    const TestRec records[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11),
    };
    TestStream previous, current;
    init_stream(&previous, records, 2);
    init_stream(&current, records, 2);
    fi_census_owner_invalidate();
    s_fiTableBuilds = 0;
    fi_pair_stream(&current.stream, &previous.stream);
    assert(s_fiTableBuilds == 4);
    fi_pair_stream(&current.stream, &previous.stream);
    assert(s_fiTableBuilds == 8);
    dispose_stream(&current);
    dispose_stream(&previous);
    fi_census_owner_invalidate();
}

static void test_key_domains(void) {
    FiPosKey mainLow = key_from_rec(&(TestRec){0x4000u, 0, 0x10, 17, 3, 9});
    FiPosKey mainHigh = key_from_rec(&(TestRec){0x7fffu, 0, 0x10, 17, 3, 9});
    FiPosKey auxLow = key_from_rec(&(TestRec){0x8000u, 0, 0x10, 17, 3, 9});
    FiPosKey auxHigh = key_from_rec(&(TestRec){0xbfffu, 0, 0x10, 17, 3, 9});
    FiPosKey c000 = key_from_rec(&(TestRec){0xc000u, 0, 0x10, 17, 3, 9});
    FiPosKey immediate = key_from_rec(&(TestRec){0xffffu, 0, 0x10, 17, 3, 9});
    FiPosKey deferred = key_from_rec(&(TestRec){0x0001u, 0, 0x10, 17, 3, 9});
    assert(fi_key_object_group(&mainLow) == 0x4000u);
    assert(fi_key_object_group(&auxLow) == 0x4000u);
    assert(fi_key_object_group(&mainHigh) == 0x7fffu);
    assert(fi_key_object_group(&auxHigh) == 0x7fffu);
    assert(fi_key_object_group(&c000) == 0);
    assert(fi_key_object_group(&immediate) == 0);
    assert(fi_key_object_group(&deferred) == 0);
    assert(fi_key_same_group(&mainLow, &auxLow));
    assert(fi_key_same_group(&immediate, &deferred));
    assert(!fi_key_equal(&mainLow, &auxLow));
    assert(!fi_key_equal(&immediate, &deferred));
}

static void test_reordered_main_and_aux(void) {
    const TestRec previous[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11), rec(0x8010u, 1, 0x11),
        rec(0x4020u, 0, 0x22), rec(0x8020u, 0, 0x22),
    };
    const TestRec current[] = {
        rec(0x8010u, 1, 0x11), rec(0x4020u, 0, 0x22), rec(0x4010u, 0, 0x11),
        rec(0x8010u, 0, 0x11), rec(0x8020u, 0, 0x22),
    };
    const int32_t expected[] = {2, 3, 0, 1, 4};
    TestStream prev, cur;
    init_stream(&prev, previous, 5);
    init_stream(&cur, current, 5);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, expected, 5);
    dispose_stream(&cur);
}

static void test_unrelated_add_and_remove(void) {
    const TestRec only_a[] = {rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11)};
    const TestRec a_plus_b[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11),
        rec(0x4020u, 0, 0x22), rec(0x8020u, 0, 0x22),
    };
    const TestRec b_removed_and_a_reordered[] = {
        rec(0x8010u, 0, 0x11), rec(0x4010u, 0, 0x11),
    };
    const int32_t add_expected[] = {0, 1, -1, -1};
    const int32_t remove_expected[] = {1, 0};
    TestStream prev, cur;
    init_stream(&prev, only_a, 2);
    init_stream(&cur, a_plus_b, 4);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, add_expected, 4);
    dispose_stream(&cur);
    dispose_stream(&prev);

    init_stream(&prev, a_plus_b, 4);
    init_stream(&cur, b_removed_and_a_reordered, 2);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, remove_expected, 2);
    dispose_stream(&cur);
}

static void test_legacy_count_guard(void) {
    const TestRec stable_previous[] = {rec(0xffffu, 0, 0x31), rec(0x0001u, 0, 0x31)};
    const TestRec stable_current[] = {rec(0x0001u, 0, 0x31), rec(0xffffu, 0, 0x31)};
    const TestRec one_previous[] = {rec(0xffffu, 0, 0x31)};
    const TestRec two_current[] = {rec(0xffffu, 0, 0x31), rec(0x0001u, 0, 0x31)};
    const int32_t stable_expected[] = {1, 0};
    const int32_t snapped_expected[] = {-1, -1};
    TestStream prev, cur;
    init_stream(&prev, stable_previous, 2);
    init_stream(&cur, stable_current, 2);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, stable_expected, 2);
    dispose_stream(&cur);
    dispose_stream(&prev);

    init_stream(&prev, one_previous, 1);
    init_stream(&cur, two_current, 2);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, snapped_expected, 2);
    dispose_stream(&cur);
}

static void test_object_rejections(void) {
    const TestRec object_a[] = {rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11)};
    const TestRec missing_main[] = {rec(0x8010u, 0, 0x11)};
    const TestRec duplicate_main_previous[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11), rec(0x8010u, 1, 0x11),
    };
    const TestRec duplicate_main[] = {
        rec(0x4010u, 0, 0x11), rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11),
    };
    const TestRec previous_aux_pair[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11), rec(0x8010u, 1, 0x11),
    };
    const TestRec duplicate_aux[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11), rec(0x8010u, 0, 0x11),
    };
    const TestRec aux_count_changed[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11), rec(0x8010u, 1, 0x11),
    };
    const TestRec payload_mismatch[] = {rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x12)};
    const TestRec swapped_key[] = {rec(0x4020u, 0, 0x22), rec(0x8020u, 0, 0x22)};
    const int32_t snap2[] = {-1, -1};
    const int32_t snap3[] = {-1, -1, -1};
    TestStream prev, cur;

    /* A same-count object swap is not motion: the old object keys are absent. */
    init_stream(&prev, object_a, 2);
    init_stream(&cur, swapped_key, 2);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, snap2, 2);
    dispose_stream(&cur);
    dispose_stream(&prev);

    init_stream(&prev, object_a, 2);
    init_stream(&cur, missing_main, 1);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, (const int32_t[]){-1}, 1);
    dispose_stream(&cur);
    dispose_stream(&prev);

    /* Same aggregate count on both sides: a duplicate visible key must reject
     * the unique surviving AO copy too, not only the duplicate entries. */
    init_stream(&prev, duplicate_main_previous, 3);
    init_stream(&cur, duplicate_main, 3);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, snap3, 3);
    dispose_stream(&cur);
    dispose_stream(&prev);

    init_stream(&prev, previous_aux_pair, 3);
    init_stream(&cur, duplicate_aux, 3);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, snap3, 3);
    dispose_stream(&cur);
    dispose_stream(&prev);

    init_stream(&prev, object_a, 2);
    init_stream(&cur, aux_count_changed, 3);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, snap3, 3);
    dispose_stream(&cur);
    dispose_stream(&prev);

    /* The exact keys and counts match; only the AO copy bytes disagree with
     * their canonical visible matrix. The whole object must still snap. */
    init_stream(&prev, object_a, 2);
    init_stream(&cur, payload_mismatch, 2);
    fi_pair_stream(&cur.stream, &prev.stream);
    assert_map(&cur, snap2, 2);
    dispose_stream(&cur);
    dispose_stream(&prev);

    /* A previous-tick payload mismatch is equally unsafe. */
    {
        const TestRec previous_bad[] = {rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x12)};
        init_stream(&prev, previous_bad, 2);
        init_stream(&cur, object_a, 2);
        fi_pair_stream(&cur.stream, &prev.stream);
        assert_map(&cur, snap2, 2);
        dispose_stream(&cur);
        dispose_stream(&prev);
    }

    /* Generation changes never pair even when the sub/ordinal is same. */
    {
        const TestRec previous_context[] = {rec(0x4010u, 0, 0x11)};
        const TestRec changed_generation[] = {rec_ctx(0x4010u, 0, 0x11, 17, 3, 10)};
        init_stream(&prev, previous_context, 1);
        init_stream(&cur, changed_generation, 1);
        fi_pair_stream(&cur.stream, &prev.stream);
        assert_map(&cur, (const int32_t[]){-1}, 1);
        dispose_stream(&cur);
        dispose_stream(&prev);
    }

    /* Camera changes are a separate identity boundary. */
    {
        const TestRec previous_context[] = {rec(0x4010u, 0, 0x11)};
        const TestRec changed_camera[] = {rec_ctx(0x4010u, 0, 0x11, 17, 4, 9)};
        init_stream(&prev, previous_context, 1);
        init_stream(&cur, changed_camera, 1);
        fi_pair_stream(&cur.stream, &prev.stream);
        assert_map(&cur, (const int32_t[]){-1}, 1);
        dispose_stream(&cur);
        dispose_stream(&prev);
    }
}

static void test_census_reuse_object_guard(void) {
    const TestRec object_a[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11),
    };
    const TestRec payload_mismatch[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x12),
    };
    TestStream previous, current, next, full;
    unsigned before;

    init_stream(&previous, object_a, 2);
    init_stream(&current, payload_mismatch, 2);
    init_stream(&next, object_a, 2);
    init_stream(&full, object_a, 2);
    seal_stream(&previous, 30);
    seal_stream(&current, 31);
    seal_stream(&next, 32);
    seal_stream(&full, 33);

    fi_census_owner_invalidate();
    s_fiTableBuilds = 0;
    fi_pair_stream(&current.stream, &previous.stream);
    assert(s_fiTableBuilds == 4);
    assert_map(&current, (const int32_t[]){-1, -1}, 2);
    before = s_fiTableBuilds;
    fi_pair_stream(&next.stream, &current.stream);
    assert(s_fiTableBuilds == before + 2);
    assert_map(&next, (const int32_t[]){-1, -1}, 2);

    /* The forced-full path must reject the same visible/private mismatch. */
    fi_census_owner_invalidate();
    before = s_fiTableBuilds;
    fi_pair_stream(&full.stream, &current.stream);
    assert(s_fiTableBuilds == before + 4);
    assert_map(&full, (const int32_t[]){-1, -1}, 2);

    dispose_stream(&previous);
    dispose_stream(&current);
    dispose_stream(&next);
    dispose_stream(&full);
    fi_census_owner_invalidate();
}

static void test_census_owner_rebuild_allocation_failure(void) {
    const TestRec records[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11),
    };
    const int32_t expected[] = {0, 1};
    TestStream previous, current, next;
    unsigned before;

    init_stream(&previous, records, 2);
    init_stream(&current, records, 2);
    init_stream(&next, records, 2);
    seal_stream(&previous, 34);
    seal_stream(&current, 35);
    seal_stream(&next, 36);

    fi_census_owner_invalidate();
    s_fiTableBuilds = 0;
    fi_pair_stream(&current.stream, &previous.stream);
    assert(s_fiTableBuilds == 4);
    before = s_fiTableBuilds;

    /* After the owner swap these previous-bank structs become the current
     * rebuild targets. Fail their first table growth, not the owner tables. */
    s_prevGroupTab.cap = 0;
    s_prevNameTab.cap = 0;
    test_table_grow_calls = 0;
    test_table_grow_fail = 1;
    fi_pair_stream(&next.stream, &current.stream);
    test_table_grow_fail = 0;
    assert(s_fiTableBuilds == before + 2);
    assert(!s_censusOwner.valid);
    assert(next.stream.prevPos[0] < 0 && next.stream.prevPos[1] < 0);

    /* Failure is fail-closed, and the next call recovers through four-table
     * fallback with the same map as the successful owner path. */
    before = s_fiTableBuilds;
    fi_pair_stream(&next.stream, &current.stream);
    assert(s_fiTableBuilds == before + 4);
    assert_map(&next, expected, 2);

    dispose_stream(&previous);
    dispose_stream(&current);
    dispose_stream(&next);
    fi_census_owner_invalidate();
}

static void test_table_allocation_failure(void) {
    const TestRec previous_records[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11),
    };
    TestStream prev, cur;
    init_stream(&prev, previous_records, 2);
    init_stream(&cur, previous_records, 2);
    /* fi_pair_stream first grows prevPos, then the first group-table slot.
     * Fail that table allocation and require the explicit all-four-table
     * guard to degrade the whole current stream to unpaired. */
    s_curGroupTab.cap = 0;
    s_curNameTab.cap = 0;
    s_prevGroupTab.cap = 0;
    s_prevNameTab.cap = 0;
    test_table_grow_calls = 0;
    test_table_grow_fail = 1;
    fi_census_owner_invalidate();
    s_fiTableBuilds = 0;
    fi_pair_stream(&cur.stream, &prev.stream);
    test_table_grow_fail = 0;
    assert(s_fiTableBuilds == 4);
    assert_map(&cur, (const int32_t[]){-1, -1}, 2);
    dispose_stream(&cur);
    dispose_stream(&prev);
}

static void test_census_global_invalidation(void) {
    const TestRec records[] = {
        rec(0x4010u, 0, 0x11), rec(0x8010u, 0, 0x11),
    };
    TestStream previous, current;
    init_stream(&previous, records, 2);
    init_stream(&current, records, 2);
    seal_stream(&previous, 40);
    seal_stream(&current, 41);

    fi_census_owner_invalidate();
    fi_pair_stream(&current.stream, &previous.stream);
    assert(s_censusOwner.valid);
    s_active = 1;
    fi_set_active(0);
    assert(!s_censusOwner.valid);

    fi_pair_stream(&current.stream, &previous.stream);
    assert(s_censusOwner.valid);
    mp6_fi_savestate_reset();
    assert(!s_censusOwner.valid);

    dispose_stream(&current);
    dispose_stream(&previous);
}

static void test_replay_transform_at_half_alpha(void) {
    ReplayStream previous, current;
    uint8_t reference[128];
    uint32_t reference_len, optimized_len;
    float translated;
    init_replay_stream(&previous, 0.0f);
    init_replay_stream(&current, 1.0f);
    fi_pair_stream(&current.stream, &previous.stream);
    assert(current.stream.prevPos[0] == 0);
    assert(current.stream.prevPos[1] == 1);

    reference_len = fi_build_replay_reference(&current.stream, &previous.stream, 1, 0.5);
    assert(reference_len == 106);
    memcpy(reference, s_replayBuf, reference_len);
    optimized_len = fi_build_replay(&current.stream, &previous.stream, 1, 0.5);
    assert(optimized_len == reference_len);
    assert(memcmp(reference, s_replayBuf, reference_len) == 0);
    translated = rd_bef32(s_replayBuf + 5 + 12);
    assert(fabsf(translated - 1.5f) < 0.00001f);
    assert(fabsf(rd_bef32(s_replayBuf + 58 + 12) - 1.5f) < 0.00001f);
    assert(memcmp(s_replayBuf + 5, s_replayBuf + 58, 48) == 0);
    assert(s_dbgRewritten == 2);
    free(current.stream.prevPos);
}

int main(void) {
    s_diag = 0;
    test_key_domains();
    test_reordered_main_and_aux();
    test_unrelated_add_and_remove();
    test_legacy_count_guard();
    test_object_rejections();
    test_census_rotation_and_lifecycle();
    test_census_unsealed_fallback();
    test_census_lifecycle_rotation();
    test_census_reuse_object_guard();
    test_census_owner_rebuild_allocation_failure();
    test_table_allocation_failure();
    test_census_global_invalidation();
    test_replay_transform_at_half_alpha();
    puts("FI AO identity pairing and replay checks passed");
    return 0;
}
"""


def _pair_fixture_source() -> str:
    source = replay_test_source()
    source = source.replace(
        "static int test_cache_grow_fail;\n",
        "static int test_cache_grow_fail;\n"
        "static int test_table_grow_fail;\n"
        "static unsigned test_table_grow_calls;\n",
        1,
    )
    source = source.replace(
        "    if (need <= *cap) return 1;\n",
        "    if (test_table_grow_fail && test_table_grow_calls++ == 1) return 0;\n"
        "    if (need <= *cap) return 1;\n",
        1,
    )
    return source + CPP_PAIR_TEST


class FiAoIdentityTests(unittest.TestCase):
    def test_extracted_pair_stream_and_replay_cases(self):
        with tempfile.TemporaryDirectory(prefix="fi-ao-identity-", dir=ROOT / "build") as folder:
            directory = Path(folder)
            source = directory / "fi_ao_identity_selftest.c"
            source.write_text(_pair_fixture_source(), encoding="utf-8")
            exe = directory / "fi-ao-identity-selftest.exe"
            result = subprocess.run(
                [build.ZIG, "cc", "-std=c11", "-O2", "-UNDEBUG",
                 "-DMP6_FI_REPLAY_TEST",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-I" + str(ROOT / "include"),
                 "-I" + str(ROOT / "src/host"),
                 "-I" + str(ROOT / "third_party/aurora/include"),
                 str(source), "-o", str(exe)],
                capture_output=True, text=True, timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_object_identity_contract_is_wired(self):
        source = SOURCE.read_text(encoding="utf-8")
        group_start = source.index("static uint16_t fi_key_object_group")
        object_group = source[group_start:source.index("/* Legacy emission-index keys", group_start)]
        pair_start = source.index("static void fi_pair_stream")
        pairing = source[pair_start:source.index("/* =======================================================================\n * TRS", pair_start)]
        self.assertIn("0x4000u + (key->sub & 0x3FFFu)", object_group)
        self.assertIn("key->sub < 0xC000u", object_group)
        self.assertIn("fi_validate_object_groups(cur, prev)", pairing)
        self.assertIn("s_curGroupTab.rejected", pairing)
        self.assertIn("gCur != gPrev || oCur != 1 || oPrev != 1", pairing)
        self.assertIn("memcmp(cur->data + cur->posOff[i]", source)
        self.assertIn("memcmp(prev->data + prev->posOff[copyPrev]", source)


if __name__ == "__main__":
    unittest.main()
