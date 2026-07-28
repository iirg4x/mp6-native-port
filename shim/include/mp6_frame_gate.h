#ifndef MP6_FRAME_GATE_H
#define MP6_FRAME_GATE_H

typedef void (*MP6FrameGatePump)(void *user);
typedef int (*MP6FrameGateTryBegin)(void *user);
typedef void (*MP6FrameGateIdle)(void *user);

/* Host-only Android lifecycle gate.  Nothing except the supplied event pump
 * and frame-begin probe executes until a presentable frame exists. */
static inline unsigned mp6_frame_gate_wait(MP6FrameGatePump pump,
                                            MP6FrameGateTryBegin try_begin,
                                            MP6FrameGateIdle idle,
                                            void *user)
{
    unsigned attempts = 0;
    for (;;) {
        pump(user);
        attempts++;
        if (try_begin(user)) break;
        /* poll_events blocks for an ordinary Android pause. This bounded
         * idle covers the distinct failure path where the window is marked
         * presentable but surface/GPU admission keeps declining. */
        if (idle) idle(user);
    }
    return attempts;
}

#endif /* MP6_FRAME_GATE_H */
