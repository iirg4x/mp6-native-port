from pathlib import Path
import struct
import tempfile
import unittest

from tools import unlock_save_gci as subject


def card():
    raw = bytearray(0xA040)
    raw[:6] = b"GP6E01"
    for base in subject.BOX_BASES:
        for box in range(3):
            start = subject.GCI_HEADER + base + box * subject.BOX_SIZE
            raw[start:start + subject.BOX_BODY] = bytes(
                (i * 17 + box) % 256 for i in range(subject.BOX_BODY))
            raw[start:start + 4] = b"SAVE" if box == 0 else b"EMPT"
            struct.pack_into(">H", raw, start + subject.BOX_BODY,
                             subject.checksum(raw[start:start + subject.BOX_BODY]))
    struct.pack_into(">H", raw, 0x2080, subject.checksum(raw[0x40:0x2080]))
    return bytes(raw)


class UnlockSave(unittest.TestCase):
    def test_only_unlock_bits_and_checksums_change(self):
        original = card()
        updated = subject.unlock(original, 0)
        allowed = set()
        for base in subject.BOX_BASES:
            start = subject.GCI_HEADER + base
            allowed.update(range(start + 0x24, start + 0x34))
            allowed.update((start + 0x428, start + subject.BOX_BODY,
                            start + subject.BOX_BODY + 1))
            self.assertEqual(updated[start + 0x24:start + 0x34], b"\xff" * 16)
            self.assertEqual(updated[start + 0x428], original[start + 0x428] | 0x70)
        self.assertTrue(all(i in allowed for i, (a, b) in enumerate(zip(original, updated)) if a != b))
        subject.validate(updated)
        self.assertEqual(subject.unlock(updated, 0), updated)

    def test_corrupt_foreign_empty_and_invalid_boxes_are_rejected(self):
        original = card()
        for index in (0, 0x40, 0x2100, 0x6100):
            corrupt = bytearray(original)
            corrupt[index] ^= 1
            with self.assertRaises(ValueError):
                subject.unlock(corrupt, 0)
        for box in (-1, 1, 2, 3):
            with self.assertRaises(ValueError):
                subject.unlock(original, box)
        with self.assertRaises(ValueError):
            subject.unlock(original[:-1], 0)

    def test_apply_makes_exact_backup_and_second_apply_is_noop(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.gci"
            original = card()
            path.write_bytes(original)
            backup = subject.apply(path, 0)
            self.assertEqual(backup.read_bytes(), original)
            self.assertEqual(path.read_bytes(), subject.unlock(original, 0))
            self.assertIsNone(subject.apply(path, 0))


if __name__ == "__main__":
    unittest.main()
