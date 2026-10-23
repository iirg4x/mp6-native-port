"""CPU-only coverage for book QA's native XeGTAO policy evidence."""

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
import os
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tests.integration import run_book_ao_qa as runner
from tests.integration.background_qa import MARKER
from tests.integration.run_book_ao_qa import (
    parse_args,
    xegtao_environment,
    xegtao_execution_evidence,
)


def native_log(quality='high', denoise=2, dispatches=12, resolution='full'):
    mode = 'reference' if resolution == 'full' and quality == 'high' and denoise == 2 else 'experimental'
    suffix = '' if denoise == 1 else 'es'
    output = 'full-resolution direct' if resolution == 'full' else 'half-resolution bilateral'
    return (f'[MP6-AO] XeGTAO {mode}: {output} uint output, '
            f'{quality.capitalize()}, {denoise} spatial denoise pass{suffix}; opt-in only\n'
            f'[MP6-AO] encoded=12 composited=12 foliage=0 xegtao={dispatches}\n')


class BookAoQaPolicy(unittest.TestCase):
    def test_defaults_and_all_six_explicit_policies(self):
        defaults = parse_args(['qa-default'])
        self.assertEqual((defaults.xegtao_quality, defaults.xegtao_denoise), ('high', 2))
        self.assertEqual(defaults.xegtao_resolution, 'full')
        for quality in ('low', 'medium', 'high'):
            for denoise in (1, 2):
                with self.subTest(quality=quality, denoise=denoise):
                    args = parse_args(['qa', '--implementation', 'xegtao',
                                       '--xegtao-quality', quality, '--xegtao-denoise', str(denoise)])
                    self.assertEqual((args.xegtao_quality, args.xegtao_denoise), (quality, denoise))
                    self.assertEqual(xegtao_environment(args.implementation, quality, denoise),
                                     {'MP6_AO_XEGTAO': '1', 'MP6_AO_XEGTAO_QUALITY': quality,
                                      'MP6_AO_XEGTAO_DENOISE': str(denoise),
                                      'MP6_AO_XEGTAO_RESOLUTION': 'full'})

    def test_half_resolution_parser_environment_and_effective_evidence(self):
        for quality in ('low', 'medium', 'high'):
            for denoise in (1, 2):
                with self.subTest(quality=quality, denoise=denoise):
                    args = parse_args(['qa', '--implementation', 'xegtao', '--xegtao-resolution', 'half',
                                       '--xegtao-quality', quality, '--xegtao-denoise', str(denoise)])
                    self.assertEqual(args.xegtao_resolution, 'half')
                    env = xegtao_environment(args.implementation, quality, denoise, args.xegtao_resolution)
                    self.assertEqual(env['MP6_AO_XEGTAO_RESOLUTION'], 'half')
                    evidence = xegtao_execution_evidence(native_log(quality, denoise, resolution='half'),
                                                         2, 'xegtao', quality, denoise, 'half')
                    self.assertEqual(evidence, dict(effective_quality=quality, effective_denoise=denoise,
                                                   effective_resolution='half', mode='experimental', dispatches=12))

    def test_legacy_rejects_nondefaults_and_does_not_forward_policy(self):
        self.assertEqual(xegtao_environment('legacy', 'high', 2), {'MP6_AO_XEGTAO': '0'})
        for options in (['--xegtao-quality', 'low'], ['--xegtao-quality', 'medium'],
                        ['--xegtao-denoise', '1'], ['--xegtao-resolution', 'half']):
            with self.subTest(options=options), redirect_stderr(StringIO()), self.assertRaises(SystemExit) as error:
                parse_args(['qa', *options])
            self.assertEqual(error.exception.code, 2)

    def test_invalid_policy_arguments_fail_before_running(self):
        for options in (['--xegtao-quality', 'ultra'], ['--xegtao-quality', 'High'],
                        ['--xegtao-denoise', '0'], ['--xegtao-denoise', '3'],
                        ['--xegtao-resolution', 'quarter'], ['--xegtao-resolution', 'Half']):
            with self.subTest(options=options), redirect_stderr(StringIO()), self.assertRaises(SystemExit) as error:
                parse_args(['qa', '--implementation', 'xegtao', *options])
            self.assertEqual(error.exception.code, 2)

    def test_effective_native_policy_and_real_counter_are_both_required(self):
        for quality in ('low', 'medium', 'high'):
            for denoise in (1, 2):
                evidence = xegtao_execution_evidence(native_log(quality, denoise), 2, 'xegtao', quality, denoise)
                self.assertEqual(evidence['effective_quality'], quality)
                self.assertEqual(evidence['effective_denoise'], denoise)
                self.assertEqual(evidence['effective_resolution'], 'full')
                self.assertEqual(evidence['dispatches'], 12)
        wrong = native_log('high', 2)
        with self.assertRaisesRegex(AssertionError, 'policy mismatch'):
            xegtao_execution_evidence(wrong, 2, 'xegtao', 'low', 1)
        for log in ('MP6_AO_XEGTAO=1\nxegtao=12\n',
                    '[MP6-AO] encoded=12 composited=12 foliage=0 xegtao=12\n',
                    native_log('low', 1, 0), native_log('low', 1).splitlines()[0]):
            with self.subTest(log=log), self.assertRaises(AssertionError):
                xegtao_execution_evidence(log, 2, 'xegtao', 'low', 1)
        with self.assertRaisesRegex(AssertionError, 'policy mismatch'):
            xegtao_execution_evidence(native_log('low', 1) + native_log('high', 2), 2, 'xegtao', 'low', 1)

    def test_half_resolution_refuses_fallback_bad_mode_missing_dispatches_and_mixed_logs(self):
        half = native_log(resolution='half')
        for log in (native_log(), half.replace('experimental:', 'reference:'),
                    half.replace('bilateral', 'direct'), half + native_log(),
                    half.replace('xegtao=12', 'xegtao=0'), half.splitlines()[0]):
            with self.subTest(log=log), self.assertRaises(AssertionError):
                xegtao_execution_evidence(log, 2, 'xegtao', 'high', 2, 'half')
        with self.assertRaisesRegex(AssertionError, 'policy mismatch'):
            xegtao_execution_evidence(half, 2, 'xegtao', 'high', 2)
        with self.assertRaisesRegex(AssertionError, 'Malformed'):
            xegtao_execution_evidence(half + '[MP6-AO] XeGTAO invalid policy\n',
                                      2, 'xegtao', 'high', 2, 'half')

    def test_ao_disabled_has_no_claim_of_effective_policy(self):
        evidence = xegtao_execution_evidence('', 0, 'xegtao', 'low', 1)
        self.assertIsNone(evidence['effective_quality'])
        self.assertIsNone(evidence['effective_denoise'])
        self.assertIsNone(evidence['effective_resolution'])
        self.assertIsNone(evidence['dispatches'])

    def test_replay_validation_is_retained(self):
        with redirect_stderr(StringIO()), self.assertRaises(SystemExit):
            parse_args(['qa', '--require-replay'])
        args = parse_args(['qa', '--require-replay', '--unlocked'])
        self.assertTrue(args.require_replay and args.unlocked)

    def test_mocked_capture_records_effective_policy_and_preserves_isolation(self):
        # Exercise the real runner handoff and metadata without launching any
        # program. The fake subprocess supplies one minimal replay capture.
        with tempfile.TemporaryDirectory(prefix='book-ao-policy-') as directory:
            root = Path(directory)
            executable = root/'fixture.exe'
            executable.write_bytes(MARKER.encode())
            observed = {}

            def fake_run(command, **kwargs):
                observed.update(kwargs['env'])
                self.assertEqual(command, [str(executable), '1900'])
                kwargs['stdout'].write(native_log('low', 1, resolution='half'))
                frames = Path(kwargs['cwd'])/'frames'
                frames.mkdir()
                frame = bytearray(100)
                struct.pack_into('<II', frame, 12, 1, 1)
                frame[96:] = bytes((40, 50, 60, 255))
                (frames/'frame.mfd').write_bytes(frame)
                (frames/'index.csv').write_text('replay\n1\n')
                return SimpleNamespace(returncode=0)

            with patch.object(runner, 'ROOT', root), patch.object(runner.subprocess, 'run', side_effect=fake_run), \
                    patch.dict(os.environ, {'MP6_UNRELATED': 'drop-me', 'MP6_AO_XEGTAO_QUALITY': 'high'}), \
                    redirect_stdout(StringIO()):
                runner.main(['qa', '--exe', str(executable), '--implementation', 'xegtao',
                             '--xegtao-quality', 'low', '--xegtao-denoise', '1', '--xegtao-resolution', 'half',
                             '--capture-count', '1', '--unlocked', '--require-replay'])
            self.assertEqual(observed['MP6_AO_XEGTAO'], '1')
            self.assertEqual(observed['MP6_AO_XEGTAO_QUALITY'], 'low')
            self.assertEqual(observed['MP6_AO_XEGTAO_DENOISE'], '1')
            self.assertEqual(observed['MP6_AO_XEGTAO_RESOLUTION'], 'half')
            self.assertNotIn('MP6_UNRELATED', observed)
            self.assertEqual(observed['MP6_BACKGROUND_QA'], '1')
            self.assertEqual(observed['SDL_AUDIODRIVER'], 'dummy')
            self.assertEqual(observed['MP6_UNLOCKED_FPS'], '1')
            metadata = json.loads((root/'build/book-ao-qa/qa/result.json').read_text())
            self.assertEqual((metadata['xegtao_quality'], metadata['xegtao_denoise']), ('low', 1))
            self.assertEqual(metadata['xegtao_resolution'], 'half')
            self.assertEqual(metadata['xegtao_execution'], dict(effective_quality='low', effective_denoise=1,
                                                              effective_resolution='half', mode='experimental', dispatches=12))
            self.assertEqual(metadata['replay_captures'], 1)
            self.assertEqual(metadata['captures'], ['frame.png'])


if __name__ == '__main__':
    unittest.main()
