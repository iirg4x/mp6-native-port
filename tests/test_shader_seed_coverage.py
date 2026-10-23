"""The bundled shader seed (res/initial_pipeline_cache.db) must cover the game.

The first launch builds every pipeline in the seed, so anything the seed
lacks compiles in the middle of play the first time it is drawn (40-110 ms
on Android). tools/generate_shader_seed.py records what each run reached in
the seed's aurora_meta 'coverage' entry; this checks it against what the
build plays: every natively playable minigame and every board, each with
every character set, and every capsule the boards list.
Regenerate with: python tools/generate_shader_seed.py --fresh
"""
import json
import sqlite3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / 'res' / 'initial_pipeline_cache.db'

import sys
sys.path.insert(0, str(ROOT / 'tools'))
import generate_shader_seed  # noqa: E402
import native_minigames  # noqa: E402

BOARDS = ('w01dll', 'w02dll', 'w03dll', 'w04dll', 'w05dll', 'w06dll', 'w10dll', 'w11dll',
          's01dll', 's02dll', 's03dll')


class ShaderSeedCoverage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with sqlite3.connect(SEED) as con:
            row = con.execute("SELECT value FROM aurora_meta WHERE key = 'coverage'").fetchone()
            cls.pipelines = con.execute('SELECT COUNT(*) FROM pipeline_cache').fetchone()[0]
        cls.coverage = json.loads(row[0]) if row else None

    def test_seed_records_coverage(self):
        self.assertIsNotNone(self.coverage, 'seed has no coverage entry: regenerate it')
        self.assertEqual(self.coverage['character_sets'], list(generate_shader_seed.CHARACTER_SETS))
        self.assertGreater(self.pipelines, 0)

    def test_every_playable_scene_with_every_character_set(self):
        scenes = self.coverage['scenes']
        expected = sorted({m.lower() for m in native_minigames.MODULES} | set(BOARDS))
        all_sets = list(range(len(generate_shader_seed.CHARACTER_SETS)))
        for scene in expected:
            with self.subTest(scene=scene):
                self.assertIn(scene, scenes, f'{scene} was never recorded')
                self.assertEqual(scenes[scene]['charsets'], all_sets, f'{scene} misses character sets')

    def test_boards_use_every_listed_capsule(self):
        scenes = self.coverage['scenes']
        used, listed = set(), 0
        for board in BOARDS:
            entry = scenes.get(board, {})
            used.update(entry.get('capsules', []))
            listed = max(listed, entry.get('listed', 0))
        self.assertGreater(listed, 0, 'no board ran the capsule sweep')
        self.assertEqual(len(used), listed, 'some capsules were never used while recording')
        for board in BOARDS:
            with self.subTest(board=board):
                self.assertTrue(scenes.get(board, {}).get('capsules'), f'{board} ran no capsule sweep')


if __name__ == '__main__':
    unittest.main()
