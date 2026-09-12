/* Frozen pre-command-index builder: keep independent of the optimized loop. */
static uint32_t fi_build_replay_reference(const FiStream *cur, const FiStream *prev, int interp, double alpha)
{
    FiVtxState vs;
    FiMtx lastPos[10];       /* interpolated pos 3x4 per PN slot (id/3) */
    uint8_t lastPosState[10]; /* 0 = untouched/verbatim, 1 = rewritten */
    uint32_t pos = 0, out = 0, posIdx = 0;
    uint32_t copyFrom = 0, copyTo = 0, copyBytes = 0;
    uint32_t aoIdx = 0;
    int skipDepthOffscreen = 0;

    memset(lastPosState, 0, sizeof(lastPosState));
    s_replayAoCount = 0;
    s_dbgPosSeen = s_dbgRewritten = s_dbgSnapUnpaired = s_dbgSnapGate = s_dbgVerbatimEq = 0;
    s_dbgSnapCamera = s_dbgSnapDecompose = 0;
    s_dbgDropOffscreen = s_dbgDropCopyBind = s_dbgDropCopyExec = s_dbgDropDestroy = 0;
    s_dbgDropOther = s_dbgDropPosLoads = s_dbgCopyClear = 0;
    s_dbgMaxTrans = 0.0;
    s_dbgMinQdot = 1.0;
    s_dbgMaxScaleRatio = 0.0;
    s_dbgScaleModel = s_dbgScaleOrd = -1;
    s_dbgScaleHold = 0;
    s_dbgMaxHeldScale = 0.0;
    s_dbgHeldModel = s_dbgHeldOrd = -1;
    s_dbgMaxRoundTrip = 0.0;
    s_dbgRoundTripModel = s_dbgRoundTripOrd = -1;
    s_dbgMaxAlpha0Err = 0.0;
    s_dbgAlpha0Model = s_dbgAlpha0Ord = -1;
    if (!fi_grow((void **)&s_replayBuf, &s_replayCap, cur->size, 1)) return 0;
    fi_seed_state(&vs, cur);

    while (pos < cur->size) {
        FiCmd c;
        while (aoIdx < cur->aoCount && cur->ao[aoIdx].offset == pos) {
            s_replayAo[s_replayAoCount] = cur->ao[aoIdx++];
            s_replayAo[s_replayAoCount++].offset = out;
        }
        if (aoIdx < cur->aoCount && cur->ao[aoIdx].offset < pos) return 0;
        uint32_t n = fi_walk_one(cur->data, pos, cur->size, &vs, &c);
        uint32_t streamPosIdx = UINT32_MAX;
        int drop = 0;
        int emitCopyClear = 0;
        if (n == 0) return 0; /* cannot happen after a clean seal walk; belt+braces */
        if (fi_is_pos_load(&c)) streamPosIdx = posIdx++;

        if (c.op == GX_AURORA) {
            switch (c.auroraSub) {
            case GX_AURORA_BEGIN_OFFSCREEN:
                skipDepthOffscreen = 1; /* drop bracket + contents (offscreen shadow pass) */
                drop = 1;
                break;
            case GX_AURORA_END_OFFSCREEN:
                skipDepthOffscreen = 0;
                drop = 1;
                break;
            case GX_AURORA_LOAD_COPY_SRC:
            case GX_AURORA_LOAD_COPY_DST:
            case GX_AURORA_LOAD_COPY_DEST:
            case GX_AURORA_REQUEST_DEPTH_SNAPSHOT: /* replay re-arm would poison GXPeekZ */
            case GX_AURORA_SET_COPY_MIP_GEN:
            case GX_AURORA_DESTROY_TEXOBJ:
            case GX_AURORA_DESTROY_TLUT:
            case GX_AURORA_DESTROY_COPY_TEX:
            case GX_AURORA_DESTROY_FRAMEBUFFER_CACHE:
                drop = 1;
                break;
            default:
                break;
            }
        } else if (c.op == 0x61 && c.bpReg == 0x52) {
            /* Suppress the CPU-addressed texture resolve, but preserve the
             * command's clear-after-copy pass boundary below. */
            emitCopyClear = (rd_be32(cur->data + pos + 1) & (1u << 11)) != 0;
            drop = 1;
        }
        if (skipDepthOffscreen) {
            drop = 1;
            emitCopyClear = 0;
        }
        if (drop) {
            /* Keep verbatim commands in one span. A removed/replaced command
             * breaks contiguity, so commit the preceding span first. */
            fi_flush_replay_copy(cur->data, copyFrom, copyTo, &copyBytes);
            if (skipDepthOffscreen || c.auroraSub == GX_AURORA_BEGIN_OFFSCREEN ||
                c.auroraSub == GX_AURORA_END_OFFSCREEN) {
                s_dbgDropOffscreen++;
            } else if (c.op == 0x61 && c.bpReg == 0x52) {
                s_dbgDropCopyExec++;
            } else if (c.auroraSub == GX_AURORA_LOAD_COPY_SRC ||
                       c.auroraSub == GX_AURORA_LOAD_COPY_DST ||
                       c.auroraSub == GX_AURORA_LOAD_COPY_DEST) {
                s_dbgDropCopyBind++;
            } else if (c.auroraSub == GX_AURORA_DESTROY_TEXOBJ ||
                       c.auroraSub == GX_AURORA_DESTROY_TLUT ||
                       c.auroraSub == GX_AURORA_DESTROY_COPY_TEX ||
                       c.auroraSub == GX_AURORA_DESTROY_FRAMEBUFFER_CACHE) {
                s_dbgDropDestroy++;
            } else {
                s_dbgDropOther++;
            }
            if (fi_is_pos_load(&c)) s_dbgDropPosLoads++;
        }
        if (emitCopyClear) s_dbgCopyClear++;
        if (emitCopyClear) {
            /* Replacement is four bytes versus the BP command's five, so the
             * replay buffer (sized to the original stream) always has room. */
            s_replayBuf[out++] = GX_AURORA;
            s_replayBuf[out++] = (uint8_t)(GX_AURORA_REPLAY_COPY_CLEAR >> 8);
            s_replayBuf[out++] = (uint8_t)GX_AURORA_REPLAY_COPY_CLEAR;
            s_replayBuf[out++] = 1;
        }

        if (!drop) {
            if (!copyBytes) {
                copyFrom = pos;
                copyTo = out;
            }
            copyBytes += n;
            if (fi_is_pos_load(&c)) {
                uint32_t slot = c.xfAddr / 4 / 3; /* addr = id*4, id = slot*3 (GX_PNMTX0..9) */
                uint8_t *payload = s_replayBuf + out + 5;
                int rewritten = 0;
                /* Hoisted so the decline census can NAME which of the two
                 * pre-conditions failed instead of one of them vanishing into a
                 * silent fall-through, and so camera_stable is still evaluated
                 * exactly once, only for a paired matrix. */
                int paired = interp && streamPosIdx < cur->posCount &&
                             cur->prevPos[streamPosIdx] >= 0;
                int camStable = paired &&
                    mp6_fi_model_camera_stable(cur->posKey[streamPosIdx].camera);
                s_dbgPosSeen++;
                if (camStable) {
                    uint32_t prevIdx = (uint32_t)cur->prevPos[streamPosIdx];
                    const uint8_t *pa = prev->data + prev->posOff[prevIdx];
                    const uint8_t *pb = cur->data + c.offset + 5;
                    if (memcmp(pa, pb, 48) == 0) {
                        s_dbgVerbatimEq++;
                    }
                    if (memcmp(pa, pb, 48) != 0) { /* byte-equal pairs stay verbatim */
                        FiMtx A, B, O;
                        FiTrs ta, tb, to;
                        fi_read_mtx(pa, &A);
                        fi_read_mtx(pb, &B);
                        if (!fi_decompose(&A, &ta) || !fi_decompose(&B, &tb)) {
                            /* Mirrored/degenerate basis: this pair is presented
                             * verbatim. Counted, not swallowed -- see
                             * s_dbgSnapDecompose. */
                            s_dbgSnapDecompose++;
                        } else {
                            double dx = tb.t[0] - ta.t[0], dy = tb.t[1] - ta.t[1], dz = tb.t[2] - ta.t[2];
                            double qdot = fabs(ta.q[0] * tb.q[0] + ta.q[1] * tb.q[1] +
                                               ta.q[2] * tb.q[2] + ta.q[3] * tb.q[3]);
                            /* Scale is the third TRS channel and the only one
                             * that had no gate. Measured for every pair
                             * (attributed, so a log names the model), and gated
                             * by a CHANNEL HOLD rather than a snap. */
                            double sratio = fi_scale_ratio(&ta, &tb);
                            int holdScale = !fi_no_scale_hold() &&
                                            sratio >= fi_scale_snap_ratio();
                            /* Per-pair gate (FI_TRANS_SNAP_U / FI_ROT_SNAP_QDOT):
                             * a replay advances this matrix one tick FORWARD, so
                             * interpolate only when the tick's motion is small
                             * enough that the overshoot is imperceptible. A fast
                             * translation (spawn/despawn/scene cut) or fast
                             * rotation (menu portrait flip) extrapolates into a
                             * visible smear -- the reported flicker -- so those
                             * pairs pass through stream N's matrix verbatim
                             * (snapped, i.e. that object stays at the tick rate)
                             * while the genuinely-smooth remainder interpolates. */
                            double dmag = sqrt(dx * dx + dy * dy + dz * dz);
                            if (dmag > s_dbgMaxTrans) s_dbgMaxTrans = dmag;
                            if (qdot < s_dbgMinQdot) s_dbgMinQdot = qdot;
                            if (sratio > s_dbgMaxScaleRatio) {
                                s_dbgMaxScaleRatio = sratio;
                                s_dbgScaleModel = (int)cur->posKey[streamPosIdx].model;
                                s_dbgScaleOrd = (int)cur->posKey[streamPosIdx].ordinal;
                            }
                            if (dx * dx + dy * dy + dz * dz >= FI_TRANS_SNAP_U * FI_TRANS_SNAP_U ||
                                qdot <= FI_ROT_SNAP_QDOT) {
                                s_dbgSnapGate++;
                            }
                            if (dx * dx + dy * dy + dz * dz < FI_TRANS_SNAP_U * FI_TRANS_SNAP_U &&
                                qdot > FI_ROT_SNAP_QDOT) {
                                FiMtx RB;   /* fi_compose(fi_decompose(B)): B minus its shear */
                                int rr, cc;
                                if (fi_diag() && (uint32_t)cur->prevPos[streamPosIdx] != streamPosIdx) {
                                    /* fallback-matched pair that will interpolate: if the
                                     * key search bound two DIFFERENT nodes (membership
                                     * shift), this is the mispair-flash mechanism. */
                                    fprintf(stderr, "[MP6-FI] interp-fallback tick=%ld model=%d sub=%u ord=%u "
                                            "pairIdx %u->%d dTrans=%.2f qdot=%.5f\n",
                                            cur->tick, (int)cur->posKey[streamPosIdx].model,
                                            (unsigned)cur->posKey[streamPosIdx].sub,
                                            (unsigned)cur->posKey[streamPosIdx].ordinal,
                                            streamPosIdx, cur->prevPos[streamPosIdx],
                                            sqrt(dx * dx + dy * dy + dz * dz), qdot);
                                }
                                if (fi_diag() >= 3 &&
                                    dx * dx + dy * dy + dz * dz > 1.0) {
                                    /* Level 3: every pair that actually gets
                                     * displaced, with both sides' identity.
                                     * A static object appearing here at all is
                                     * a mispair by construction. */
                                    fprintf(stderr,
                                            "[MP6-FI] rewrite vitick=%ld tick=%ld prevTick=%ld alpha=%.3f "
                                            "idx %u->%u cam=%d model=%d gen=%u sub=%u ord=%u "
                                            "prevCam=%d prevModel=%d prevGen=%u prevSub=%u prevOrd=%u "
                                            "dTrans=%.2f qdot=%.5f\n",
                                            mp6_tick_count, cur->tick, prev->tick, alpha,
                                            streamPosIdx, prevIdx,
                                            (int)cur->posKey[streamPosIdx].camera,
                                            (int)cur->posKey[streamPosIdx].model,
                                            (unsigned)cur->posKey[streamPosIdx].generation,
                                            (unsigned)cur->posKey[streamPosIdx].sub,
                                            (unsigned)cur->posKey[streamPosIdx].ordinal,
                                            (int)prev->posKey[prevIdx].camera,
                                            (int)prev->posKey[prevIdx].model,
                                            (unsigned)prev->posKey[prevIdx].generation,
                                            (unsigned)prev->posKey[prevIdx].sub,
                                            (unsigned)prev->posKey[prevIdx].ordinal,
                                            sqrt(dx * dx + dy * dy + dz * dz), qdot);
                                }
                                /* ---- RESIDUAL CARRY --------------------------
                                 * fi_decompose/fi_compose is lossy on any
                                 * sheared modelview (see fi_decompose): the
                                 * round trip alone re-poses the object even
                                 * with alpha taken out, which is what made
                                 * mode select's right bridge (model 30 ord 3,
                                 * bridge4) shear on EVERY interpolated present
                                 * and snap back on the next tick frame -- an
                                 * A-B-A flicker whose amplitude is set by the
                                 * decomposition error, not by object motion,
                                 * so no motion gate above can ever catch it.
                                 *
                                 * Split B into the part the TRS model can
                                 * represent (RB) and the part it cannot
                                 * (B - RB, the shear). Interpolate only the
                                 * first and add the second back unchanged:
                                 *
                                 *   O = compose(advance(A,B,alpha)) + (B - RB)
                                 *
                                 * At alpha = 0 advance() returns B's own TRS,
                                 * so O == B EXACTLY: a replay frame can never
                                 * disagree with the tick frame it was built
                                 * from. That removes the whole defect class
                                 * rather than one object, and every object
                                 * keeps interpolating (no new snap gate, so
                                 * the smoothness the feature exists for is
                                 * preserved). The residual is a small constant
                                 * in eye space; over a single tick's gated
                                 * rotation (<=10 deg) its failure to rotate
                                 * with the object is second-order.
                                 * Column 3 needs no correction: decompose
                                 * copies the translation verbatim, so the
                                 * residual there is identically zero. */
                                fi_compose(&tb, &RB);
                                fi_advance(&ta, &tb, alpha, holdScale, &to);
                                fi_compose(&to, &O);
                                if (holdScale) {
                                    s_dbgScaleHold++;
                                    if (sratio > s_dbgMaxHeldScale) {
                                        s_dbgMaxHeldScale = sratio;
                                        s_dbgHeldModel = (int)cur->posKey[streamPosIdx].model;
                                        s_dbgHeldOrd = (int)cur->posKey[streamPosIdx].ordinal;
                                    }
                                }
                                if (!fi_no_residual()) {
                                    for (rr = 0; rr < 3; ++rr) {
                                        for (cc = 0; cc < 3; ++cc) {
                                            O.m[rr][cc] += B.m[rr][cc] - RB.m[rr][cc];
                                        }
                                    }
                                }
                                if (fi_diag() >= 3) {
                                    /* maxResid: how much shear the TRS model
                                     * cannot represent (this is what USED to be
                                     * injected as displacement -- it is now
                                     * carried).  maxA0: the real self-check --
                                     * run the whole rewrite at alpha = 0 and
                                     * measure |O0 - B|, which must be 0. */
                                    FiTrs t0chk;
                                    FiMtx O0;
                                    /* Same holdScale the real rewrite used: the
                                     * self-check must exercise the code path
                                     * that shipped, not a second variant of it.
                                     * (alphaS = 0 is a no-op at alpha = 0, so
                                     * maxA0Err must stay 0 either way -- that
                                     * is the property being checked.) */
                                    fi_advance(&ta, &tb, 0.0, holdScale, &t0chk);
                                    fi_compose(&t0chk, &O0);
                                    for (rr = 0; rr < 3; ++rr) {
                                        for (cc = 0; cc < 4; ++cc) {
                                            double e = RB.m[rr][cc] - B.m[rr][cc];
                                            double e0;
                                            if (cc < 3 && !fi_no_residual())
                                                O0.m[rr][cc] += B.m[rr][cc] - RB.m[rr][cc];
                                            e0 = O0.m[rr][cc] - B.m[rr][cc];
                                            if (e < 0.0) e = -e;
                                            if (e0 < 0.0) e0 = -e0;
                                            if (e > s_dbgMaxRoundTrip) {
                                                s_dbgMaxRoundTrip = e;
                                                s_dbgRoundTripModel = (int)cur->posKey[streamPosIdx].model;
                                                s_dbgRoundTripOrd = (int)cur->posKey[streamPosIdx].ordinal;
                                            }
                                            if (e0 > s_dbgMaxAlpha0Err) {
                                                s_dbgMaxAlpha0Err = e0;
                                                s_dbgAlpha0Model = (int)cur->posKey[streamPosIdx].model;
                                                s_dbgAlpha0Ord = (int)cur->posKey[streamPosIdx].ordinal;
                                            }
                                        }
                                    }
                                }
                                fi_flush_replay_copy(cur->data, copyFrom, copyTo, &copyBytes);
                                fi_write_mtx(payload, &O);
                                if (slot < 10) {
                                    lastPos[slot] = O;
                                    lastPosState[slot] = 1;
                                }
                                rewritten = 1;
                            }
                        }
                    }
                }
                /* Exhaustive by construction: rewritten / unpaired / camera-cut
                 * / byte-equal / decompose-refused / motion-gated are disjoint
                 * and cover every pos load counted in s_dbgPosSeen. The sum is
                 * checked below so a later edit cannot reintroduce a silent
                 * class. */
                if (rewritten) {
                    s_dbgRewritten++;
                } else if (!paired) {
                    s_dbgSnapUnpaired++;
                } else if (!camStable) {
                    s_dbgSnapCamera++;
                }
                if (!rewritten && slot < 10) {
                    lastPosState[slot] = 0; /* verbatim pos -> leave its nrm verbatim too */
                }
            } else if (fi_is_nrm_load(&c)) {
                uint32_t slot = (c.xfAddr - 0x400u) / 3 / 3; /* addr = id*3+0x400, id = slot*3 */
                if (slot < 10 && lastPosState[slot]) {
                    /* rewritten pos -> recompute inverse-transpose; on a
                     * singular matrix keep the original bytes */
                    fi_flush_replay_copy(cur->data, copyFrom, copyTo, &copyBytes);
                    (void)fi_write_nrm_from_pos(s_replayBuf + out + 5, &lastPos[slot]);
                }
            }
            out += n;
        }
        pos += n;
    }
    fi_flush_replay_copy(cur->data, copyFrom, copyTo, &copyBytes);
    if (fi_diag() >= 3) {
        /* The census must PARTITION the position loads it saw. Before the two
         * missing buckets existed it did not, and the difference was silently
         * absorbed -- so "no pair was refused for reason X" was unfalsifiable
         * from a log. Any line here means a rewrite decision exists that no
         * counter names, which is a diagnostic defect even when the pixels are
         * right. */
        uint32_t sum = s_dbgRewritten + s_dbgSnapUnpaired + s_dbgSnapCamera +
                       s_dbgVerbatimEq + s_dbgSnapDecompose + s_dbgSnapGate;
        if (sum != s_dbgPosSeen) {
            fprintf(stderr, "[MP6-FI] CENSUS SUM MISMATCH tick=%ld seen=%u sum=%u "
                            "(rw=%u unpaired=%u cam=%u byteEq=%u decomp=%u gate=%u)\n",
                    cur->tick, s_dbgPosSeen, sum, s_dbgRewritten, s_dbgSnapUnpaired,
                    s_dbgSnapCamera, s_dbgVerbatimEq, s_dbgSnapDecompose, s_dbgSnapGate);
        }
    }
    while (aoIdx < cur->aoCount && cur->ao[aoIdx].offset == pos) {
        s_replayAo[s_replayAoCount] = cur->ao[aoIdx++];
        s_replayAo[s_replayAoCount++].offset = out;
    }
    if (aoIdx != cur->aoCount) return 0;
    return out;
}

