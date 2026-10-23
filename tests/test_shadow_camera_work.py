"""Execute the actual per-camera shadow scheduling block, not a Python model."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from tests.test_widescreen_effects import patched

ROOT = Path(__file__).resolve().parents[1]


class ShadowCameraWork(unittest.TestCase):
    def test_empty_views_do_not_render_unused_maps_and_last_pass_clears(self):
        source = patched('src/game/hsfman.c')
        start = source.index('            shadowCameraNo = -1;')
        end = source.index('\n        }\n        if(NoSyncF', start)
        body = source[start:end]
        harness = r'''
#include <assert.h>
#include "mp6_shadow_quality.h"
#define HU3D_CAM_MAX 4
typedef struct { float fov; int scissorW, scissorH; } Camera;
static Camera Hu3DCamera[4];
static int Hu3DShadowBuf[4], *Hu3DShadow, Hu3DCameraNo, Hu3DCameraBit;
static int Hu3DShadowCamBit = 3, calls, ids[4], clears[4];
typedef int BOOL;
#define TRUE 1
#define FALSE 0
static void Hu3DShadowExec(int clear) {
    assert(Hu3DShadow == &Hu3DShadowBuf[Hu3DCameraNo]);
    ids[calls] = Hu3DCameraNo; clears[calls++] = clear;
}
static void schedule(void) {
    Camera *cameraP; int i, shadowCameraNo, cameraBit;
''' + body + r'''
}
int main(void) {
    for (int i=0;i<4;++i) Hu3DCamera[i]=(Camera){-1,640,480};
    Hu3DCamera[0].fov=Hu3DCamera[1].fov=30;
    schedule();
    assert(calls==2 && ids[0]==0 && ids[1]==1 && !clears[0] && clears[1]);
    calls=0; Hu3DCamera[1].scissorW=0; schedule();
    assert(calls==1 && ids[0]==0 && clears[0]);
    calls=0; Hu3DCamera[0].scissorH=0; Hu3DCamera[1].scissorW=640; schedule();
    assert(calls==1 && ids[0]==1 && clears[0]);
    calls=0; Hu3DCamera[1].scissorW=0; schedule();
    assert(calls==2 && !clears[0] && clears[1]);
    calls=0; Hu3DCamera[0].scissorH=480; Hu3DShadowCamBit=2; schedule();
    assert(calls==1 && ids[0]==1 && clears[0]);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temporary:
            root = Path(temporary)
            subject, exe = root/'shadow.c', root/'shadow.exe'
            subject.write_text(harness)
            result = subprocess.run([build.ZIG, 'cc', '-UNDEBUG', '-I'+str(ROOT/'include'),
                str(subject), '-o', str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
