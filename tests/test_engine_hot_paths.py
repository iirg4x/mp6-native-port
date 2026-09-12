"""Run the real registry and window-metrics helpers against lifetime/event oracles."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT=Path(__file__).resolve().parents[1]


def function(text,name):
    return re.search(r'^(?:static )?(?:int|void) '+name+r'\([^;]*?\)\n\{.*?^\}',text,re.M|re.S).group()


class EngineHotPaths(unittest.TestCase):
    def compile_run(self,source):
        with tempfile.TemporaryDirectory(prefix='engine-hot-',dir=ROOT/'build') as directory:
            path=Path(directory)/'test.c'
            path.write_text(source)
            exe=Path(directory)/'test.exe'
            result=subprocess.run([build.ZIG,'cc',*build.COMMON_FLAGS,'-I'+str(ROOT/'tests/native'),
                '-O2','-UNDEBUG',str(path),'-o',str(exe)],cwd=ROOT,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_array_registry_lifetime_and_hash_collisions(self):
        self.compile_run('#include "gxarray_registry_selftest.c"\n')

    def test_window_metrics_are_refreshed_at_event_boundaries(self):
        text=(ROOT/'src/gx/aurora_bridge.c').read_text()
        declaration=text[text.index('static SDL_Window *g_mp6AspectWindow'):text.index('static void mp6_window_metrics_invalidate(void)\n{')]
        helpers=function(text,'mp6_window_metrics_invalidate')+'\n'+function(text,'mp6_window_metrics_get')
        self.compile_run('''#include <assert.h>
#include <stddef.h>
typedef struct { int width,height,available; } SDL_Window;
static int queries;
static int SDL_GetWindowSizeInPixels(SDL_Window *w,int *x,int *y) {
    queries++; *x=w->width; *y=w->height; return w->available;
}
'''+declaration+helpers+'''
int main(void) {
    int x,y;
    SDL_Window a={2960,1848,1},b={1920,1080,1};
    assert(!mp6_window_metrics_get(&x,&y)); assert(queries==0);
    g_mp6AspectWindow=&a;
    for(int i=0;i<10000;i++) {assert(mp6_window_metrics_get(&x,&y)); assert(x==2960 && y==1848);}
    assert(queries==1);
    a.width=1848; a.height=2960; mp6_window_metrics_invalidate();
    assert(mp6_window_metrics_get(&x,&y)); assert(x==1848 && y==2960 && queries==2);
    g_mp6AspectWindow=&b; assert(mp6_window_metrics_get(&x,&y)); assert(x==1920 && y==1080);
    b.available=0; mp6_window_metrics_invalidate(); assert(!mp6_window_metrics_get(&x,&y));
    b.available=1; b.height=0; assert(!mp6_window_metrics_get(&x,&y));
    b.height=1200; assert(mp6_window_metrics_get(&x,&y)); assert(y==1200);
    g_mp6AspectWindow=NULL; assert(!mp6_window_metrics_get(&x,&y));
}
''')
        for name in ('mp6_dispatch_aurora_events','mp6_bridge_window_policy_init',
                     'mp6_bridge_apply_content_aspect_policy','mp6_widescreen_set_enabled'):
            self.assertIn('mp6_window_metrics_invalidate();',function(text,name))
        self.assertIn('mp6_window_metrics_get(&w, &h)',function(text,'mp6_widescreen_render_width'))


if __name__=='__main__': unittest.main()
