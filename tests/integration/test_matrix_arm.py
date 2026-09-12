"""Execute NDK-built matrix kernels under ARM64 instruction emulation.

Correctness only, never a device or performance benchmark.
Optional dependency: pip install --target build/arm-math-test-deps unicorn
"""
import hashlib,json,random,struct,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'build/arm-math-test-deps'))
import unicorn
from unicorn import arm64_const as reg

CODE=0x100000; DATA=0x300000; STOP=0x2f0000
compiler=ROOT/'build/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
source=(ROOT/'build/android-aurora-source/lib/dolphin/mtx/mtx.c').read_text()
scalar=source[source.index('static inline void mtx_concat_scalar('):source.index('void C_MTXConcat(')]
candidate=source[source.index('static inline void mtx_concat_scalar('):source.index('void C_MTXConcatArray(')]
prefix='''#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef float Mtx[3][4];
typedef float (*MtxPtr)[4];
static void C_MTXCopy(const Mtx a,Mtx b) {
 if(a!=b) for(unsigned r=0;r<3;r++) for(unsigned c=0;c<4;c++) b[r][c]=a[r][c];
}
'''
images=[]
with tempfile.TemporaryDirectory(prefix='matrix-arm-',dir=ROOT/'build') as temp:
    directory=Path(temp)
    script=directory/'kernel.ld'
    script.write_text('SECTIONS { . = 0x100000; .text : { *(.text.entry) *(.text*) } .rodata : { *(.rodata*) } /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) } }')
    for name,body,entry in [('reference',scalar,'static inline void mtx_concat_scalar('),('candidate',candidate,'void C_MTXConcat(')]:
        src=directory/(name+'.c'); binary=directory/(name+'.bin')
        src.write_text(prefix+body.replace(entry,'__attribute__((section(".text.entry"))) void subject('))
        command=[str(compiler),'--target=aarch64-linux-android26','-O2','-DNDEBUG','-DMP6_EXPERIMENT_VECTOR_CONCAT=1','-fno-strict-aliasing',
                 '-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
                 '-nostdlib','-static','-no-pie','-Wl,--build-id=none','-Wl,--oformat=binary','-Wl,-T,'+str(script),str(src),'-o',str(binary)]
        subprocess.run(command,check=True,capture_output=True)
        images.append(binary.read_bytes())

machines=[]
for image in images:
    machine=unicorn.Uc(unicorn.UC_ARCH_ARM64,unicorn.UC_MODE_ARM)
    machine.reg_write(reg.UC_ARM64_REG_CPACR_EL1,3<<20)
    machine.mem_map(CODE,0x400000)
    machine.mem_write(CODE,image)
    machines.append(machine)

def run(machine,data,layout):
    machine.mem_write(DATA,data)
    for register,offset in zip((reg.UC_ARM64_REG_X0,reg.UC_ARM64_REG_X1,reg.UC_ARM64_REG_X2),layout):
        machine.reg_write(register,DATA+offset*4)
    machine.reg_write(reg.UC_ARM64_REG_SP,0x4ff000)
    machine.reg_write(reg.UC_ARM64_REG_X30,STOP)
    machine.emu_start(CODE,STOP,count=10000)
    assert machine.reg_read(reg.UC_ARM64_REG_PC)==STOP,'kernel failed to return'
    return struct.unpack('<64I',machine.mem_read(DATA,256))

def nan(bits):return bits&0x7f800000==0x7f800000 and bits&0x7fffff!=0
layouts=((0,16,32),(0,16,0),(0,16,16),(0,0,0),(0,0,32),(0,16,1),(0,16,17),(16,0,3),(4,20,0),(8,20,12),(0,1,32),(1,2,0))
rng=random.Random(421781); count=0
for trial in range(1200):
    values=[]
    for i in range(64):
        if trial%3==0: bits=rng.getrandbits(1)<<31
        elif trial%3==1: bits=(rng.getrandbits(32)&0x807fffff)|(rng.randrange(254)<<23)
        else: bits=struct.unpack('<I',struct.pack('<f',rng.uniform(-1000,1000)))[0]
        values.append(bits)
    data=struct.pack('<64I',*values)
    for layout in layouts:
        expected=run(machines[0],data,layout); actual=run(machines[1],data,layout)
        for i,(a,b) in enumerate(zip(expected,actual)):
            assert a==b or (nan(a) and nan(b)),f'trial={trial} layout={layout} element={i} {a:08x}!={b:08x}'
        count+=1
result=dict(layouts=count,unicorn=unicorn.__version__,compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),
            kernel_sha256=[hashlib.sha256(image).hexdigest() for image in images],
            scope='ARM64 instruction-emulated correctness, not Android execution or timing')
(ROOT/'build/matrix-arm-validation.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
