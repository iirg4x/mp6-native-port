#!/usr/bin/env node
// MP6 web packager -- test runner. No framework, no `npm install`:
//
//   node web/test/run.js
//
// Runs every *.test.js in this directory (unit tests: FST parser,
// wanted-set filter, output-path safety, CRC-32, ZIP round-trip,
// folder/directory sinks, format/game-ID validation, the full iso-source
// pipeline against the synthetic fixture, and packager orchestration) plus
// the LOCAL-ONLY real ISO integration test, which self-reports SKIP unless
// $MP6_ISO is set -- see test/integration/real-iso.test.js and the feature
// report for exact numbers measured against the real disc.
//
// Exits 0 iff every non-skipped test passed.

import { runAll } from "./tiny-test.js";

console.log("MP6 web packager -- test suite\n");

await import("./crc32.test.js");
await import("./format-detect.test.js");
await import("./validate.test.js");
await import("./fst.test.js");
await import("./wanted.test.js");
await import("./path-safe.test.js");
await import("./zip-roundtrip.test.js");
await import("./folder-source.test.js");
await import("./directory-sink.test.js");
await import("./zip-stream-sink.test.js");
await import("./iso-source.test.js");
await import("./packager.test.js");
await import("./integration/real-iso.test.js");

const ok = await runAll();
process.exit(ok ? 0 : 1);
