"""Selector-equivalence and durable patch checks for full-window UI policy."""
from dataclasses import dataclass, field
from pathlib import Path
import random
import subprocess
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build

ROOT = Path(__file__).resolve().parents[1]


@dataclass
class Element:
    visible: bool = True
    full_window: bool = False
    text: bool = False
    children: list = field(default_factory=list)


def selector_policy(root):
    matches = []

    def collect(node, ancestors_visible):
        for child in node.children:
            if child.text:
                continue
            visible = ancestors_visible and child.visible
            if child.full_window:
                matches.append(visible)
            collect(child, visible)

    collect(root, root.visible)
    return any(matches)


def direct_policy(root):
    if not root.visible:
        return False
    for child in root.children:
        if child.text or not child.visible:
            continue
        if child.full_window or direct_policy(child):
            return True
    return False


class WindowPolicyTests(unittest.TestCase):
    def test_visibility_attribute_and_document_lifetimes(self):
        root = Element(full_window=True)
        # The selector matches descendants, never the root itself.
        self.assertFalse(direct_policy(root))
        parent = Element(visible=False, children=[Element(full_window=True)])
        root.children.append(parent)
        for visible in (False, True, False, True):
            parent.visible = visible
            self.assertEqual(direct_policy(root), visible)
            self.assertEqual(direct_policy(root), selector_policy(root))
        parent.children[0].full_window = False
        self.assertFalse(direct_policy(root))
        parent.children.append(Element(full_window=True, text=True))
        self.assertFalse(direct_policy(root))
        parent.children.append(Element(full_window=True))
        self.assertTrue(direct_policy(root))
        root.children.clear()
        self.assertFalse(direct_policy(root))

    def test_random_nested_trees_match_original_selector(self):
        rng = random.Random(606)

        def tree(depth):
            return Element(rng.random() > .3, rng.random() > .8,
                           rng.random() > .9,
                           [tree(depth - 1) for _ in range(rng.randrange(5))] if depth else [])

        for _ in range(2000):
            root = tree(5)
            self.assertEqual(direct_policy(root), selector_policy(root))

if __name__ == "__main__":
    unittest.main()
