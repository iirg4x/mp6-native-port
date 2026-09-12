"""A declined replay must not sleep using a stale frame deadline remainder."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReplayDeadlineRefresh(unittest.TestCase):
    def test_declined_replay_resamples_deadline(self):
        source=(ROOT/'src/gx/aurora_bridge.c').read_text()
        tail=source.split('fiDeclined = 1; /* final for this window',1)[1]
        self.assertLess(tail.index('continue;'),tail.index('mp6_host_sleep_ns('))


if __name__=='__main__': unittest.main()
