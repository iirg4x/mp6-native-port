#!/usr/bin/env python3
"""Turn wasm-ld's trapping signature-mismatch stubs into adapters.

The decomp's translation units disagree about many prototypes: a function
defined `void f(s16)` is declared `int f(int)` elsewhere, or called through
an implicit declaration, or a generated shim is defined `int f()` but called
with arguments. Native ABIs absorb this (extra arguments are ignored, a
missing return value is whatever is in the register). WebAssembly checks
call signatures exactly, so wasm-ld routes each mismatched direct call to a
stub named "signature_mismatch:<f>" whose body is `unreachable`.

This rewrites every such stub, in a linked but not yet post-processed
module (emcc --oformat=bare), into what the native call would have done:
pass the arguments the callee takes (zero for any it expects but was not
given, extras dropped), call the real function, and adapt the result
(dropped, or zero when the caller expects one the callee does not return).
Integer/float register-class mismatches are reinterpreted bitwise.

    python tools/wasm_sigfix.py module.wasm      # rewrites in place
"""
import sys

I32, I64, F32, F64 = 0x7F, 0x7E, 0x7D, 0x7C
STUB_PREFIX = "signature_mismatch:"


def uleb(data, pos):
    result = shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return result, pos


def enc_uleb(value):
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def name_str(data, pos):
    n, pos = uleb(data, pos)
    return data[pos:pos + n].decode("utf-8", "replace"), pos + n


def skip_limits(data, pos):
    flags = data[pos]
    pos += 1
    _, pos = uleb(data, pos)
    if flags & 1:
        _, pos = uleb(data, pos)
    return pos


def zero_const(t):
    return {I32: b"\x41\x00", I64: b"\x42\x00", F32: b"\x43" + b"\0" * 4,
            F64: b"\x44" + b"\0" * 8}[t]


def convert(src, dst):
    """Instructions turning a value of type src on the stack into dst."""
    if src == dst:
        return b""
    table = {
        (I32, I64): b"\xAC", (I64, I32): b"\xA7",
        (I32, F32): b"\xBE", (F32, I32): b"\xBC",
        (I64, F64): b"\xBF", (F64, I64): b"\xBD",
        (F32, F64): b"\xBB", (F64, F32): b"\xB6",
        (I32, F64): b"\xBE\xBB", (F64, I32): b"\xB6\xBC",
        (I64, F32): b"\xA7\xBE", (F32, I64): b"\xBC\xAC",
    }
    if (src, dst) in table:
        return table[(src, dst)]
    return b"\x1A" + zero_const(dst)  # drop, then a zero of the wanted type


def main(path):
    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\0asm":
        raise SystemExit(f"{path}: not a wasm module")
    sections = []  # (id, start_of_content, end)
    pos = 8
    while pos < len(data):
        sid = data[pos]
        size, cpos = uleb(data, pos + 1)
        sections.append((sid, pos, cpos, cpos + size))
        pos = cpos + size

    types, func_types, names = [], [], {}
    for sid, _hdr, start, end in sections:
        p = start
        if sid == 1:
            count, p = uleb(data, p)
            for _ in range(count):
                assert data[p] == 0x60, "unsupported type form"
                p += 1
                n, p = uleb(data, p)
                params = list(data[p:p + n])
                p += n
                n, p = uleb(data, p)
                results = list(data[p:p + n])
                p += n
                types.append((params, results))
        elif sid == 2:
            count, p = uleb(data, p)
            for _ in range(count):
                _, p = name_str(data, p)
                _, p = name_str(data, p)
                kind = data[p]
                p += 1
                if kind == 0:
                    t, p = uleb(data, p)
                    func_types.append(t)
                elif kind == 1:
                    p = skip_limits(data, p + 1)
                elif kind == 2:
                    p = skip_limits(data, p)
                elif kind == 3:
                    p += 2
                elif kind == 4:
                    _, p = uleb(data, p + 1)
        elif sid == 3:
            imported = len(func_types)
            count, p = uleb(data, p)
            for _ in range(count):
                t, p = uleb(data, p)
                func_types.append(t)
        elif sid == 0:
            sname, p = name_str(data, p)
            if sname != "name":
                continue
            while p < end:
                sub = data[p]
                size, p = uleb(data, p + 1)
                subend = p + size
                if sub == 1:
                    count, q = uleb(data, p)
                    for _ in range(count):
                        idx, q = uleb(data, q)
                        nm, q = name_str(data, q)
                        names[idx] = nm
                p = subend

    by_name = {nm: idx for idx, nm in names.items()}
    stubs = {idx: nm[len(STUB_PREFIX):] for idx, nm in names.items() if nm.startswith(STUB_PREFIX)}
    if not stubs:
        print("wasm_sigfix: no signature-mismatch stubs")
        return 0
    if not names:
        raise SystemExit("wasm_sigfix: module has no name section (link with -g2 or higher)")

    code = next(s for s in sections if s[0] == 10)
    _sid, hdr, start, end = code
    count, p = uleb(data, start)
    bodies = []
    for _ in range(count):
        size, q = uleb(data, p)
        bodies.append(bytes(data[q:q + size]))
        p = q + size

    fixed, unresolved = 0, []
    for idx, target_name in sorted(stubs.items()):
        target = by_name.get(target_name)
        local_index = idx - imported
        if target is None or not 0 <= local_index < len(bodies):
            unresolved.append(target_name)
            continue
        caller_params, caller_results = types[func_types[idx]]
        callee_params, callee_results = types[func_types[target]]
        body = bytearray(b"\x00")  # no locals
        for i, want in enumerate(callee_params):
            if i < len(caller_params):
                body += b"\x20" + enc_uleb(i) + convert(caller_params[i], want)
            else:
                body += zero_const(want)
        body += b"\x10" + enc_uleb(target)
        if callee_results and not caller_results:
            body += b"\x1A"
        elif caller_results and not callee_results:
            body += zero_const(caller_results[0])
        elif caller_results and callee_results:
            body += convert(callee_results[0], caller_results[0])
        body += b"\x0B"
        bodies[local_index] = bytes(body)
        fixed += 1

    content = bytearray(enc_uleb(len(bodies)))
    for b in bodies:
        content += enc_uleb(len(b)) + b
    new_section = bytes([10]) + enc_uleb(len(content)) + content
    data[hdr:end] = new_section
    open(path, "wb").write(data)
    print(f"wasm_sigfix: adapted {fixed} signature-mismatch stub(s)"
          + (f"; unresolved: {', '.join(unresolved)}" if unresolved else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
