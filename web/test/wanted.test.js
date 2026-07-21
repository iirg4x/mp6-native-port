// Table-driven tests for js/wanted.js against mp6-native's
// platform/content/content_import.cpp:76-88 (path_has_prefix / wanted_file).

import { test, assertEqual, assertDeepEqual } from "./tiny-test.js";
import { pathHasPrefix, wantedFile, filterWanted } from "../js/wanted.js";
import { buildSyntheticIso } from "./fixtures/make-fixture.js";

test("pathHasPrefix: exact match counts (boundary is end-of-string)", () => {
    assertEqual(pathHasPrefix("data", "data"), true);
});
test("pathHasPrefix: prefix followed by '/' counts", () => {
    assertEqual(pathHasPrefix("data/title.bin", "data"), true);
});
test("pathHasPrefix: prefix followed by any other char does NOT count", () => {
    assertEqual(pathHasPrefix("data2/x.bin", "data"), false);
    assertEqual(pathHasPrefix("database", "data"), false);
});
test("pathHasPrefix: unrelated path does not match", () => {
    assertEqual(pathHasPrefix("movie/foo.thp", "data"), false);
});
test("pathHasPrefix: prefix longer than path does not match", () => {
    assertEqual(pathHasPrefix("da", "data"), false);
});

const WANTED_TABLE = [
    ["opening.bnr", true, "exact match"],
    ["sound/MP6_SND.msm", true, "exact match"],
    ["sound/MP6_Str.pdt", true, "exact match"],
    ["sound/MP6_SND.msm.bak", false, "exact-match strings must not prefix-match"],
    ["sound/other.bin", false, "sound/ has no prefix rule, only two named files"],
    ["data", true, "bare prefix dir name itself"],
    ["data/title.bin", true, "prefix match"],
    ["data/sub/deep.bin", true, "prefix match, nested"],
    ["data2/x.bin", false, "prefix boundary: data2 is not data"],
    ["database.bin", false, "prefix boundary: database is not data"],
    ["mess/foo.bin", true, "prefix match"],
    ["mic/bar.bin", true, "prefix match"],
    ["micro/bar.bin", false, "prefix boundary: micro is not mic"],
    ["movie/foo.thp", false, "movie/ is not in the wanted set"],
    ["dll/bootDll.rel", false, "dll/ is not in the wanted set"],
    ["opening.bnr2", false, "exact-match strings must not prefix-match"],
    ["Opening.bnr", false, "case-sensitive, matches C strcmp"],
];

for (const [path, expected, reason] of WANTED_TABLE) {
    test(`wantedFile("${path}") === ${expected} (${reason})`, () => {
        assertEqual(wantedFile(path), expected);
    });
}

test("filterWanted: keeps only wanted entries and preserves their shape", () => {
    const input = [
        { path: "opening.bnr", size: 1 },
        { path: "movie/foo.thp", size: 2 },
        { path: "data/a.bin", size: 3 },
    ];
    const result = filterWanted(input);
    assertDeepEqual(
        result.map((f) => f.path),
        ["opening.bnr", "data/a.bin"]
    );
});

test("filterWanted: matches the synthetic fixture's expected wanted set exactly", () => {
    const fixture = buildSyntheticIso();
    const result = filterWanted(fixture.allFiles);
    assertDeepEqual(
        result.map((f) => f.path).sort(),
        fixture.wantedFiles.map((f) => f.path).sort()
    );
    const totalBytes = result.reduce((sum, f) => sum + f.size, 0);
    assertEqual(totalBytes, fixture.wantedTotalBytes);
});
