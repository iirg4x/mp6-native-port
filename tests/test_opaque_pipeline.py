"""Verify the exact production blend decision, including destination alpha."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from test_engine_pass import function

ROOT=Path(__file__).resolve().parents[1]


class OpaquePipeline(unittest.TestCase):
    def test_all_supported_blend_modes_and_write_masks(self):
        path=ROOT/'build/aurora-release-source/lib/gx/gx.cpp'
        if not path.exists():self.skipTest('configured local renderer unavailable')
        text=path.read_text()
        funcs=function(text,'static inline wgpu::BlendFactor to_blend_factor(')+'\n'+function(text,'static inline wgpu::BlendState to_blend_state(')
        decision=text[text.index('  const auto replaces ='):text.index('  const std::array colorTargets')]
        with tempfile.TemporaryDirectory(prefix='opaque-test-',dir=ROOT/'build') as tmp:
            folder=Path(tmp)
            source=folder/'test.cpp'
            source.write_text("""
#include <cstdint>
#include <cassert>
#include <cstdlib>
using u32=uint32_t;
enum GXBlendMode {GX_BM_NONE,GX_BM_BLEND,GX_BM_SUBTRACT,GX_BM_LOGIC};
enum GXLogicOp {GX_LO_CLEAR,GX_LO_COPY,GX_LO_NOOP};
enum GXBlendFactor {GX_BL_ZERO,GX_BL_ONE,GX_BL_SRCCLR,GX_BL_INVSRCCLR,
 GX_BL_SRCALPHA,GX_BL_INVSRCALPHA,GX_BL_DSTALPHA,GX_BL_INVDSTALPHA};
namespace wgpu {
enum class BlendFactor {Zero,One,Src,Dst,OneMinusSrc,OneMinusDst,SrcAlpha,
 OneMinusSrcAlpha,DstAlpha,OneMinusDstAlpha,Constant};
enum class BlendOperation {Add,ReverseSubtract};
struct BlendComponent {BlendOperation operation;BlendFactor srcFactor,dstFactor;};
struct BlendState {BlendComponent color,alpha;};
}
#define DEFAULT_FATAL(...) default: std::abort()
"""+funcs+"""
bool candidate(wgpu::BlendState blendState,bool rgb,bool alpha) {
  struct {bool colorUpdate,alphaUpdate;} config{rgb,alpha};
"""+decision+"""
  return opaque;
}
int main(){
 for(int mode=0;mode<4;++mode)for(int src=0;src<8;++src)for(int dst=0;dst<8;++dst)
 for(int op=0;op<3;++op)for(int constant=0;constant<2;++constant)for(int mask=0;mask<4;++mask){
   auto state=to_blend_state(GXBlendMode(mode),GXBlendFactor(src),GXBlendFactor(dst),
       GXLogicOp(op),constant ? 128 : UINT32_MAX);
   bool identity=mode==GX_BM_NONE || (mode==GX_BM_LOGIC && op==GX_LO_COPY) ||
                 (mode==GX_BM_BLEND && src==GX_BL_ONE && dst==GX_BL_ZERO);
   bool expected=(!(mask&1) || identity) && (!(mask&2) || (identity && !constant));
   assert(candidate(state,mask&1,mask&2)==expected);
 }
}
""")
            result=subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG',str(source),
                '-o',str(folder/'test.exe')],capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(folder/'test.exe')],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__=='__main__':unittest.main()
