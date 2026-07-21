// Unit tests for js/validate.js against mp6-native's
// platform/content/content_import.cpp:172-187 (validate_game_id).

import { test, assertEqual } from "./tiny-test.js";
import { validateGameId } from "../js/validate.js";

test("validateGameId: GP6E01 (USA) passes", () => {
    assertEqual(validateGameId("GP6E01").ok, true);
});

test("validateGameId: a GP6 sibling (different region) gets the 'wrong region' message", () => {
    const result = validateGameId("GP6P01"); // hypothetical PAL id
    assertEqual(result.ok, false);
    assertEqual(result.message.includes("Wrong region"), true);
    assertEqual(result.message.includes("GP6P01"), true);
});

test("validateGameId: an unrelated game ID gets the generic mismatch message", () => {
    const result = validateGameId("GAFE01"); // Animal Crossing, unrelated
    assertEqual(result.ok, false);
    assertEqual(result.message.includes("Not Mario Party 6"), true);
});

test("validateGameId: non-printable bytes are sanitized to '?' before display", () => {
    const id = "GP6\x01\x02\x03";
    const result = validateGameId(id);
    assertEqual(result.ok, false);
    assertEqual(result.message.includes("GP6???"), true);
});
