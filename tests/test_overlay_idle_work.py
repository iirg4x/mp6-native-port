"""Source and state-model checks for the overlay's per-frame idle path."""

from dataclasses import dataclass
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


@dataclass
class TransitionState:
    open: bool = False
    opened: bool = False
    done: bool = False


def set_presence(state: TransitionState, present: bool) -> bool:
    if state.open == present:
        return False
    state.open = present
    return True


def set_pseudo(state: TransitionState, name: str, active: bool) -> bool:
    old = getattr(state, name)
    if old == active:
        return False
    setattr(state, name, active)
    return True


def open_transition(state: TransitionState) -> int:
    changes = int(set_presence(state, True))
    changes += int(set_pseudo(state, "opened", True))
    changes += int(set_pseudo(state, "done", False))
    return changes


def close_transition(state: TransitionState) -> int:
    return int(set_presence(state, False))


class OverlayIdleWorkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src/gx/ui/overlay.cpp").read_text(encoding="utf-8")
        cls.header = (ROOT / "src/gx/ui/overlay.hpp").read_text(encoding="utf-8")

    def test_transition_helpers_skip_unchanged_rml_writes(self):
        source = self.source
        helper = source[source.index("void set_attribute_presence") : source.index("void remove_toast")]
        self.assertIn("if (!element->HasAttribute(name))", helper)
        self.assertIn("else if (element->HasAttribute(name))", helper)
        self.assertIn("element->IsPseudoClassSet(name) != active", helper)

        state = TransitionState(open=True, opened=True)
        self.assertEqual(open_transition(state), 0)
        self.assertEqual(close_transition(state), 1)
        self.assertEqual(close_transition(state), 0)

    def test_fading_transition_can_reopen_and_clears_done(self):
        state = TransitionState(open=True, opened=True)
        close_transition(state)
        state.done = True
        # `opened` stays latched across a fade so a reopened element only
        # changes its open attribute and the completed marker.
        self.assertEqual(open_transition(state), 2)
        self.assertEqual(state, TransitionState(open=True, opened=True, done=False))

    def test_first_open_still_happens_after_creation(self):
        create = self.source[self.source.index("Rml::Element *create_toast") : self.source.index("Rml::Element *create_controller_warning")]
        self.assertNotIn('SetAttribute("open"', create)
        self.assertNotIn("open_transition", create)
        create_block = self.source[self.source.index("if (mCurrentToast == nullptr)") : self.source.index("else if (!toasts.empty())")]
        self.assertLess(create_block.index("create_toast"), create_block.index("mCurrentToastStartTime = clock::now()"))

    def test_progress_is_cached_and_cleared_before_toast_removal(self):
        source = self.source
        self.assertIn("Rml::Element *mCurrentToastProgress = nullptr;", self.header)
        self.assertIn("create_toast(mDocument, toast, mCurrentToastProgress)", source)
        self.assertIn("mCurrentToastProgress->SetAttribute(\"value\", remaining)", source)
        self.assertIn('mCurrentToastProgress->GetAttribute<float>("value", -1.f) != remaining', source)
        self.assertNotIn('GetElementsByTagName(list, "progress")', source)
        remove = source[source.index("void remove_toast") : source.index("} // namespace", source.index("void remove_toast"))]
        self.assertLess(remove.index("progress = nullptr"), remove.index("remove_element(toast)"))

    def test_overlay_lifecycle_uses_guards_for_each_transition_owner(self):
        source = self.source
        for owner in ("mControllerWarning", "mMenuNotification", "mCurrentToast"):
            self.assertIn(f"open_transition({owner});", source)
            self.assertIn(f"close_transition({owner});", source)
        self.assertNotIn('mControllerWarning->SetAttribute("open", "")', source)
        self.assertNotIn('mMenuNotification->SetAttribute("open", "")', source)
        self.assertNotIn('mCurrentToast->SetAttribute("open", "")', source)


if __name__ == "__main__":
    unittest.main()
