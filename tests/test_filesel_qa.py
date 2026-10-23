"""File-action QA requires real completion and never accepts menu entry alone."""
import unittest

from tests.integration.background_qa import MARKER
from tests.integration.run_filesel_qa import action_script, validate_log


class FileselQaTests(unittest.TestCase):
    def log(self, stage, before, after=None):
        text = MARKER + '\nverified hidden=1 input-subsystems=0\n'
        text += '[FILESEL-QA] slots=' + before + ' selected=0\n'
        text += f'[EVENT] qa.filesel.done={stage} num=1 tick=800\n'
        if after:
            text += '[FILESEL-QA] slots=' + after + ' selected=1\n'
            choice = 1 if stage == 'copy' else 2
            text += f'[EVENT] qa.filesel.choice=selected num={choice} tick=600\n'
        return text

    def test_native_action_and_persisted_slot_states_are_required(self):
        for stage, before, after in (
            ('create', '0,0,0', None), ('copy', '1,0,0', '1,1,0'),
            ('erase', '1,1,0', '1,0,0'), ('verify', '1,0,0', None),
        ):
            with self.subTest(stage=stage):
                log = self.log(stage, before, after)
                self.assertFalse(validate_log(log, stage, 0, False)[1])
                self.assertTrue(validate_log(log.replace('qa.filesel.done=', 'unrelated='),
                                             stage, 0, False)[1])
                self.assertTrue(validate_log(log, stage, 1, False)[1])
                self.assertTrue(validate_log(log, stage, 0, True)[1])

    def test_wrong_copy_or_erased_original_are_rejected(self):
        for log, stage in ((self.log('copy', '1,0,0', '1,0,0'), 'copy'),
                           (self.log('erase', '1,1,0', '0,1,0'), 'erase')):
            self.assertTrue(validate_log(log, stage, 0, False)[1])

    def test_controller_isolation_is_mandatory(self):
        log = self.log('verify', '1,0,0')
        self.assertTrue(validate_log(log.replace(MARKER, ''), 'verify', 0, False)[1])
        self.assertTrue(validate_log(log.replace('input-subsystems=0', 'input-subsystems=1'),
                                     'verify', 0, False)[1])

    def test_script_waits_for_real_actions_and_uses_separate_startup_budget(self):
        for stage in ('create', 'copy', 'erase', 'verify'):
            script = action_script(stage)
            self.assertIn('timeout:9000000;', script)
            self.assertIn('qa.filesel.ready/1;timeout:3500;', script)
            if stage != 'verify':
                self.assertIn('qa.filesel.done/' + stage, script)


if __name__ == '__main__':
    unittest.main()
