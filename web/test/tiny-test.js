// MP6 web packager tests -- a deliberately tiny test harness. No test
// framework dependency (the brief: no build step, nothing to `npm install`)
// -- `node web/test/run.js` must work on a bare Node install.

const tests = [];

/** Registers a test. `fn` may be async; a thrown error (or rejection) fails it. */
export function test(name, fn) {
    tests.push({ name, fn, skip: false });
}

/** Registers a test that always reports as skipped (used for the env-gated integration test). */
export function skip(name, reason) {
    tests.push({ name, fn: null, skip: true, reason });
}

/** Registers a test that runs only if `condition` is true, else reports skipped. */
export function testIf(condition, name, fn, reason) {
    if (condition) test(name, fn);
    else skip(name, reason);
}

export async function runAll() {
    let pass = 0;
    let fail = 0;
    let skipped = 0;
    for (const t of tests) {
        if (t.skip) {
            skipped++;
            console.log(`  SKIP - ${t.name}${t.reason ? ` (${t.reason})` : ""}`);
            continue;
        }
        try {
            await t.fn();
            pass++;
            console.log(`  ok   - ${t.name}`);
        } catch (e) {
            fail++;
            console.log(`  FAIL - ${t.name}`);
            const detail = e && e.stack ? e.stack : String(e);
            for (const line of detail.split("\n")) console.log(`         ${line}`);
        }
    }
    console.log(`\n${pass} passed, ${fail} failed, ${skipped} skipped, ${tests.length} total\n`);
    return fail === 0;
}

export function assertEqual(actual, expected, message) {
    if (actual !== expected) {
        throw new Error(`${message || "assertEqual"}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
    }
}

export function assertDeepEqual(actual, expected, message) {
    const a = JSON.stringify(actual);
    const e = JSON.stringify(expected);
    if (a !== e) throw new Error(`${message || "assertDeepEqual"}: expected ${e}, got ${a}`);
}

export function assertTrue(value, message) {
    if (!value) throw new Error(message || "assertTrue: value was falsy");
}

export function assertBytesEqual(actual, expected, message) {
    if (actual.length !== expected.length) {
        throw new Error(`${message || "assertBytesEqual"}: length ${actual.length} != expected ${expected.length}`);
    }
    for (let i = 0; i < actual.length; i++) {
        if (actual[i] !== expected[i]) {
            throw new Error(`${message || "assertBytesEqual"}: byte ${i} is ${actual[i]}, expected ${expected[i]}`);
        }
    }
}

export async function assertThrows(fn, messageIncludes, testMessage) {
    let threw = null;
    try {
        await fn();
    } catch (e) {
        threw = e;
    }
    if (!threw) throw new Error(testMessage || "expected function to throw, but it did not");
    if (messageIncludes && !String(threw.message).includes(messageIncludes)) {
        throw new Error(
            `${testMessage || "assertThrows"}: error message "${threw.message}" did not include "${messageIncludes}"`
        );
    }
}
