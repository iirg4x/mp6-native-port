"""Keep cross-file capsule effects compatible on Windows and AArch64.

PPC's aggregate calling convention hid pointer/value mismatches. Check the
actual patched declarations against the effect owner's actual definitions.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


def patched(name):
    relative = 'src/board/' + name + '.c'
    source = (Path(build.DECOMP) / relative).read_text(encoding='utf-8')
    patch = ROOT / 'compat/decomp' / (relative + '.patch')
    return apply_patches.apply_unified_diff(source, patch.read_text()) if patch.exists() else source


class CapsuleCallAbi(unittest.TestCase):
    def test_capspecial_api_matches_owners_on_both_native_targets(self):
        caller = patched('capspecial')
        owner = '\n'.join(patched(path.stem) for path in
                          sorted((Path(build.DECOMP)/'src/board').glob('*.c'))
                          if path.stem != 'capspecial')
        declarations = []
        names = []
        for declaration in re.findall(r'^extern\s+[^;{}]+?\bmb\w+\([^;{}]*\);',
                                      caller, re.M):
            name = re.search(r'\b(mb\w+)\(', declaration)[1]
            definition = re.search(r'^(?:void|int|float|BOOL|s16|MBMODELID|OMOBJ\s*\*)\s*' + name +
                                   r'\([^;{}]*\)\n\{', owner, re.M)
            if definition:
                names.append(name)
                declarations.extend((declaration, definition[0][:-1].strip()+';'))
        self.assertGreaterEqual(len(names), 60)
        self.assertIn('squishCount = mbev_CapPlayerSquishVoiceSet(ids, masuId, FALSE);', caller)
        self.assertRegex(owner, r'void mbev_CapPlayerSquishSet\([^;]+?\)\s*\{\s*'
                         r'mbev_CapPlayerSquishVoiceSet\(playerNo, masuId, FALSE\);\s*\}')
        prelude = '''typedef int BOOL;
typedef short s16;
typedef signed char s8;
typedef unsigned short u16;
typedef s16 MBMODELID;
typedef unsigned int u32;
typedef struct {float x,y,z;} HuVecF;
typedef struct {unsigned char r,g,b,a;} GXColor;
typedef struct OMOBJ OMOBJ;
typedef struct EVCAPWORK EVCAPWORK;
typedef struct CAPWORK CAPWORK;
'''
        prelude += '\n'.join(re.findall(r'^typedef[^;]+\(\*DICE\w+HOOK\)[^;]+;',
                                        patched('dice'), re.M))+'\n'
        with tempfile.TemporaryDirectory(prefix='capspecial-call-abi-', dir=ROOT/'build') as temporary:
            subject = Path(temporary)/'capspecial.c'
            subject.write_text(prelude+'\n'.join(declarations), encoding='utf-8')
            for target in ('x86_64-windows-gnu', 'aarch64-freestanding'):
                with self.subTest(target=target):
                    result = subprocess.run([build.ZIG, 'cc', '-target', target, '-Werror',
                                             '-c', str(subject), '-o', str(subject.with_suffix('.o'))],
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_ray_rotation_calls_pass_float_angles(self):
        caller = patched('capevent')
        owner = patched('math')
        names = ('mbMtxRotXDeg', 'mbMtxRotYDeg', 'mbMtxRotZDeg', 'mbMtxTransCat')
        declarations = []
        calls = []
        for name in names:
            declarations.append(re.search(r'^extern void '+name+r'\([^;]+;', caller, re.M).group())
            declarations.append(re.search(r'^void '+name+r'\([^;]+?\)\n\{', owner, re.M).group()[:-1]+';')
            calls.append(re.search(name+r'\(transform, particleWorkP->[^;]+;', caller).group())
        prelude = ('typedef float Mtx[3][4];\n'
                   'typedef struct {struct {float x,y,z;} _unk3C,_unk18;} Particle;\n')
        body = '\nvoid draw(Mtx transform, Particle *particleWorkP) {\n'+'\n'.join(calls)+'\n}\n'
        with tempfile.TemporaryDirectory(prefix='capsule-ray-abi-', dir=ROOT/'build') as temporary:
            subject = Path(temporary)/'ray.c'
            for valid in (True, False):
                subject.write_text(prelude+('\n'.join(declarations) if valid else '')+body)
                for target in ('x86_64-windows-gnu', 'aarch64-freestanding'):
                    result = subprocess.run([build.ZIG,'cc','-target',target,'-Werror',
                        '-c',str(subject),'-o',str(subject.with_suffix('.o'))],
                        capture_output=True,text=True,timeout=30)
                    self.assertEqual(result.returncode==0,valid,result.stderr)
                    if not valid: self.assertIn('undeclared function',result.stderr)

    def test_effect_declarations_match_implementation(self):
        owner = patched('capevent')
        functions = {
            'capsule': ['mbev_CapEffRingAdd'],
            'capthrow': ['mbev_CapEffRingHitAdd', 'mbev_CapEffExplodeAdd',
                         'mbev_CapEffDustHeavyAdd', 'mbev_CapPlayerMoveVelSet'],
            'captrap': ['mbev_CapEffRingAdd', 'mbev_CapEffRingHitAdd'],
        }
        prelude = ('typedef struct {float x,y,z;} HuVecF;\n'
                   'typedef struct {unsigned char r,g,b,a;} GXColor;\n'
                   'typedef struct OMOBJ OMOBJ;\n')
        with tempfile.TemporaryDirectory(prefix='capsule-call-abi-', dir=ROOT / 'build') as temporary:
            for caller, names in functions.items():
                source = patched(caller)
                declarations = []
                for name in names:
                    actual = re.search(r'^extern\s+[^;{}]+?\b' + name + r'\([^;{}]*\);',
                                       source, re.M).group()
                    expected = re.search(r'^(?:void|int)\s+' + name + r'\([^;{}]*\)\n\{',
                                         owner, re.M).group()[:-1].strip() + ';'
                    declarations.extend([actual, expected])
                    self.assertNotRegex(source, r'\(\(.*?\)'+name+r'\)\(',
                                        'Do not bypass effect argument checking with a cast')
                subject = Path(temporary) / (caller + '.c')
                subject.write_text(prelude + '\n'.join(declarations), encoding='utf-8')
                # Freestanding AArch64 uses the same aggregate parameter ABI
                # without requiring an Android sysroot for this declaration test.
                for target in ('x86_64-windows-gnu', 'aarch64-freestanding'):
                    with self.subTest(caller=caller, target=target):
                        result = subprocess.run([build.ZIG, 'cc', '-target', target,
                                                 '-Werror', '-c', str(subject), '-o',
                                                 str(subject.with_suffix('.o'))],
                                                capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stderr)
                # Negative control: the original declarations must fail the
                # same check, so this cannot silently become a vacuous test.
                original = (Path(build.DECOMP) / ('src/board/' + caller + '.c')).read_text()
                broken = []
                for name in names:
                    broken.append(re.search(r'^extern\s+[^;{}]+?\b' + name + r'\([^;{}]*\);',
                                            original, re.M).group())
                    broken.append(re.search(r'^(?:void|int)\s+' + name + r'\([^;{}]*\)\n\{',
                                            owner, re.M).group()[:-1].strip() + ';')
                subject.write_text(prelude + '\n'.join(broken), encoding='utf-8')
                result = subprocess.run([build.ZIG, 'cc', '-target', 'aarch64-freestanding',
                                         '-c', str(subject), '-o', str(subject.with_suffix('.o'))],
                                        capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0, 'Original ABI mismatch was not detected')
                self.assertIn('conflicting types', result.stderr)


if __name__ == '__main__':
    unittest.main()
