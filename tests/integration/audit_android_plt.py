"""Read-only AArch64 PLT audit, optionally resolving an authenticated simpleperf trace.

Counts sampled cycles, not elapsed frame time. Requires the matching unstripped
ELF and the port's NDK tools; no device state or production binary is modified.
"""
import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
NDK = ROOT/'build/android-sdk/ndk/27.3.13750724'
BIN = NDK/'toolchains/llvm/prebuilt/windows-x86_64/bin'


def command(tool, *args):
    return subprocess.check_output([str(BIN/(tool+'.exe')), *map(str, args)], text=True)


def dynamic_symbols(elf):
    symbols = {}
    for line in command('llvm-readelf', '--dyn-syms', '--wide', elf).splitlines():
        m = re.match(r'\s*\d+:\s+([0-9a-f]+)\s+\d+\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.+)', line)
        if m:
            value, kind, binding, visibility, section, name = m.groups()
            symbols[name] = dict(kind=kind, binding=binding, visibility=visibility,
                                 defined=section!='UND', value=int(value,16))
    return symbols


def audit(elf, profile=None):
    header = command('llvm-readelf', '-h', elf)
    if 'AArch64' not in header:
        raise ValueError('this audit only supports AArch64 ELF')
    symbols = dynamic_symbols(elf)
    build_id = re.search(r'Build ID: ([0-9a-f]+)', command('llvm-readelf', '-n', elf))[1]
    unversioned = defaultdict(list)
    for name, symbol in symbols.items():
        unversioned[name.split('@')[0]].append(symbol)
    disassembly = command('llvm-objdump', '-d', '--section=.plt', elf)
    stubs = []
    for m in re.finditer(r'^([0-9a-f]+) <(.+)@plt>:', disassembly, re.M):
        address, name = int(m[1],16), m[2]
        # objdump omits the version suffix from import stub labels.
        matches = unversioned[name]
        if len(matches) != 1:
            raise ValueError('ambiguous/missing dynamic symbol: '+name)
        symbol = matches[0]
        category = ('import' if not symbol['defined'] else
                    'strong_function' if symbol['kind']=='FUNC' and symbol['binding']=='GLOBAL' else
                    'weak_function' if symbol['kind']=='FUNC' and symbol['binding']=='WEAK' else 'other')
        stubs.append(dict(address=address, name=name, category=category))
    stubs.sort(key=lambda s:s['address'])
    # Current NDK lld uses 16-byte non-BTI/PAC entries; refuse to guess otherwise.
    if not stubs or any(b['address']-a['address']!=16 for a,b in zip(stubs,stubs[1:])):
        raise ValueError('unexpected AArch64 PLT layout')
    result = dict(elf=str(elf), build_id=build_id, entries=len(stubs),
                  categories=dict(Counter(s['category'] for s in stubs)))
    if profile:
        sys.path.insert(0,str(NDK/'simpleperf'))
        from simpleperf_report_lib import ReportLib
        lib = ReportLib()
        lib.SetRecordFile(str(profile))
        addresses = [s['address'] for s in stubs]
        weights, counts, total, paths = Counter(), Counter(), 0, set()
        while lib.GetNextSample():
            sample = lib.GetCurrentSample()
            symbol = lib.GetSymbolOfCurrentSample()
            total += sample.period
            if not symbol.dso_name.endswith('/libmp6game.so'):
                continue
            if symbol.dso_name not in paths:
                recorded = lib.GetBuildIdForPath(symbol.dso_name).lower().removeprefix('0x')
                if not recorded or recorded != build_id.ljust(len(recorded),'0'):
                    raise ValueError('ELF does not match recorded build ID')
                paths.add(symbol.dso_name)
            index = bisect_right(addresses, symbol.vaddr_in_file)-1
            if index>=0 and symbol.vaddr_in_file < addresses[index]+16:
                weights[index] += sample.period
                counts[index] += 1
        lib.Close()
        if len(paths)!=1 or not total:
            raise ValueError('expected one recorded MP6 image and nonempty CPU profile')
        category_cycles = Counter()
        for index, cycles in weights.items():
            category_cycles[stubs[index]['category']] += cycles
        result.update(profile=str(profile), total_cycles=total,
                      cycle_percent={k:round(v/total*100,4) for k,v in category_cycles.items()},
                      sampled_stubs=[dict(stubs[i], samples=counts[i], cycles=w,
                                          percent=round(w/total*100,4)) for i,w in weights.most_common()])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--profile', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = audit(args.elf, args.profile)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result,indent=2)+'\n')
    display = dict(result)
    if 'sampled_stubs' in display:
        display['sampled_stubs'] = display['sampled_stubs'][:15]
    print(json.dumps(display,indent=2))


if __name__ == '__main__':
    main()
