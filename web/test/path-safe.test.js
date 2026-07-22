// Table-driven tests for js/path-safe.js's isSafeRelPath(), mirroring
// mp6-native's platform/content/test_path_safe.cpp (commit 7aa2674) case
// for case so the two ports' validators stay in lockstep.

import { test, assertEqual } from "./tiny-test.js";
import { isSafeRelPath } from "../js/path-safe.js";

const CASES = [
    // traversal / escape attempts: all REJECTED
    ["data/../../evil", false, "traversal via .. components"],
    ["/abs/evil", false, "absolute path"],
    ["..\\win", false, "backslash + .."],
    ["..", false, "bare .."],
    [".", false, "bare ."],
    ["data/..", false, ".. as a non-leading component"],
    ["data/./x", false, ". as a component"],
    ["data//x", false, "empty component (doubled slash)"],
    ["data/", false, "empty component (trailing slash)"],
    ["C:/x", false, "drive-qualified, forward slash"],
    ["C:\\x", false, "drive-qualified, backslash"],
    ["data\\sub\\evil", false, "backslash-separated"],
    ["data/sub/../../../x", false, "traversal past the root"],
    ["data/e\nvil", false, "embedded control byte (LF)"],
    ["data/e\tvil", false, "embedded control byte (TAB)"],
    ["", false, "empty string"],

    // legitimate wanted-set paths: all ACCEPTED
    ["data/x.bin", true, "plain nested path"],
    ["sys/fst.bin", true, "sys/fst.bin itself"],
    ["data/sub/file.bin", true, "deeper nesting"],
    ["opening.bnr", true, "top-level file, no directory"],
    ["sound/MP6_SND.msm", true, "exact-match wanted file"],
    ["mess/e/message.bin", true, "mess/ prefix"],
    ["data/...oddbutlegal", true, "three dots is a real name, not .."],
    ["data/.hidden", true, "leading dot is a legal component"],
    ["data/a.b.c", true, "dots inside a component are fine"],
];

for (const [input, expected, reason] of CASES) {
    test(`isSafeRelPath(${JSON.stringify(input)}) === ${expected} (${reason})`, () => {
        assertEqual(isSafeRelPath(input), expected);
    });
}

test("isSafeRelPath: non-string input is rejected rather than throwing", () => {
    assertEqual(isSafeRelPath(undefined), false);
    assertEqual(isSafeRelPath(null), false);
});
