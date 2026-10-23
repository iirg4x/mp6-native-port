"""Native ABI and reload boundaries for the recovered MGMfree overlay."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build, native_mgmfree, native_namespace


ROOT = Path(__file__).resolve().parents[1]


def recovered_bridge_inventory(source):
    body = source.split('#define MP6_RECOVERED_DLLS(X)', 1)[1]
    body = body.split('#define MP6_ENUM_RECOVERED_DLL', 1)[0]
    rows = {}
    for raw in re.findall(r'^\s+X\(([^)]*)\)', body, re.M):
        fields = [field.strip() for field in raw.split(',')]
        fields[1] = fields[1].strip('"')
        fields[4] = fields[4].strip('"')
        rows[fields[0]] = tuple(fields[1:])
    return rows


class MgmfreeIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decomp = Path(apply_patches.require_pinned_decomp())

    def patched(self, name):
        rel = f"src/REL/mgmfreedll/{name}.c"
        return apply_patches.generated_source(self.decomp, rel)

    def test_only_complete_game_units_are_selected_and_fully_namespaced(self):
        self.assertEqual(native_mgmfree.UNITS, ("menu", "mode_flow"))
        units = list(native_mgmfree.sources(self.decomp))
        self.assertEqual([rel for rel, _ in units], [
            "src/REL/mgmfreedll/menu.c",
            "src/REL/mgmfreedll/mode_flow.c",
        ])
        self.assertNotIn("runtime.c", "\n".join(rel for rel, _ in units))
        all_flags = set(units[0][1])
        for rel, flags in units:
            self.assertEqual(set(flags), all_flags)
            for name in set(re.findall(r"\b(?:fn_1_|lbl_1_)\w+",
                                       (self.decomp / rel).read_text())):
                self.assertIn(f"-D{name}=mgmfree_{name}", flags)
        for name in ("_prolog", "_epilog", "_ctors", "_dtors", "ObjectSetup"):
            self.assertIn(f"-D{name}=mgmfree_{name.lstrip('_')}", all_flags)

    def test_namespace_flags_share_scanner_across_both_units(self):
        files = [self.decomp / "src/REL/mgmfreedll" / f"{unit}.c"
                 for unit in native_mgmfree.UNITS]
        expected = native_namespace.namespace_flags(
            files, "mgmfree",
            extra_names={"_prolog", "_epilog", "_ctors", "_dtors", "ObjectSetup"},
        )
        self.assertEqual(native_mgmfree.namespace_flags(self.decomp), expected)
        self.assertEqual(native_mgmfree.namespace_flags(self.decomp),
                         native_mgmfree.namespace_flags(self.decomp))

    def test_pointer_owners_use_native_width_without_aliasing_byte_arrays(self):
        menu = self.patched("menu")
        flow = self.patched("mode_flow")
        for source in (menu, flow):
            self.assertNotIn("OMOBJ **", source)
            self.assertNotRegex(source, r"(?:extern\s+)?u8 lbl_1_bss_(?:C|28)\[")
        self.assertIn("OMOBJ *lbl_1_bss_C;", menu)
        self.assertIn("OMOBJ *lbl_1_bss_28;", menu)
        self.assertIn("extern OMOBJ *lbl_1_bss_28;", flow)
        self.assertIn("obj = lbl_1_bss_28;", flow)

    def test_big_endian_halfword_owner_has_identical_logical_values(self):
        original = (self.decomp / "src/REL/mgmfreedll/menu.c").read_text()
        raw = re.search(r"u8 lbl_1_data_2B8\[200\] = \{([^}]*)\}",
                        original, re.S).group(1)
        bytes_ = [int(value) for value in re.findall(r"\d+", raw)]
        expected = [(bytes_[i] << 8) | bytes_[i + 1]
                    for i in range(0, len(bytes_), 2)]
        patched = self.patched("menu")
        typed = re.search(r"s16 lbl_1_data_2B8\[100\] = \{([^}]*)\}",
                          patched, re.S).group(1)
        self.assertEqual([int(value) for value in re.findall(r"\d+", typed)],
                         expected)
        self.assertIn("&lbl_1_data_2B8[rowNo * 24 + selection * 4]", patched)
        self.assertIn("&lbl_1_data_2B8[16]", patched)
        self.assertNotIn("(s16 *)(lbl_1_data_2B8", patched)

    def test_every_bss_owner_and_mutable_initializer_resets_on_each_prolog(self):
        menu = self.patched("menu")
        definitions = set(re.findall(
            r"^(?!extern\b)[^\n;]*\b(lbl_1_bss_[0-9A-F]+)(?:\[[^;]*\])?;",
            menu, re.M))
        def listed(kind):
            match = re.search(r"#define MP6_MGMFREE_MENU_" + kind
                              + r"\(X\)((?:[^\n]*\\\n)*[^\n]*)", menu)
            return set(re.findall(r"X\((\w+)\)", match.group(1)))
        # Generated lists (tools/gen_rel_state.py); exact coverage is
        # tests/test_rel_state_lists.py.
        self.assertEqual(listed("BSS"), definitions)
        self.assertLessEqual({"lbl_1_data_380", "lbl_1_data_388", "lbl_1_data_3AE",
                              "lbl_1_data_3CC", "lbl_1_data_3CE", "lbl_1_data_4DC",
                              "lbl_1_data_4DE"}, listed("DATA"))
        prolog = menu.split("int _prolog(void)", 1)[1].split("void _epilog", 1)[0]
        self.assertIn("const MGMFREE_VOID_FUNC *ctor", prolog)
        bridge = (ROOT / "src/os/dll_bridge.c").read_text()
        self.assertIn("#define MP6_MGMFREE_CTORS { mp6_mgmfree_reset, 0 }", bridge)

    def test_bridge_exposes_real_lifecycle_before_missing_overlay_fallback(self):
        bridge = (ROOT / "src/os/dll_bridge.c").read_text()
        self.assertEqual(recovered_bridge_inventory(bridge)['mgmfree'], (
            'dll/mgmfreedll.rel', 'mgmfree_prolog', 'mgmfree_epilog',
            'mgmfreedll', 'MGMFREE', 'MP6_MGMFREE_CTORS'))
        self.assertIn('mp6_recovered_dll_by_path(path)', bridge)
        self.assertIn('mp6_recovered_dll_by_id(dll)', bridge)
        self.assertIn('mp6_dll_bridge_report_bound(recovered->name);', bridge)
        # The recovered menu still hands the selected game to instdll; the
        # bridge resolves that path through the recovered lifecycle first.
        self.assertIn("omOvlCallEx(DLL_instdll, 1, 0, 0);", self.patched("menu"))

    def test_projected_bus_masks_use_shared_centered_scissors(self):
        menu = self.patched("menu")
        self.assertIn("mp6_widescreen_camera_centered_scissor(HU3D_CAM1 | HU3D_CAM2 | HU3D_CAM3 |\n"
                      "                                         HU3D_CAM4 | HU3D_CAM5, TRUE);", menu)
        self.assertLess(menu.index("Hu3DCameraCreate(HU3D_CAM_ALL);"),
                        menu.index("mp6_widescreen_camera_centered_scissor("))
        # Original projection, model positions and mask dimensions stay native;
        # the shared executable camera test verifies mapping and aspect toggles.
        self.assertEqual(menu.count("screen.x *= 1.1111112f;"), 2)
        self.assertEqual(menu.count("screen.y - 96.0f, 64.0f, 64.0f);"), 2)

    def test_two_recovered_units_compile_as_native_objects(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            patched = Path(temporary) / "patched"
            apply_patches.apply_all(out_dir=str(patched))
            for rel, flags in native_mgmfree.sources(self.decomp):
                source = patched / rel
                obj = Path(temporary) / (Path(rel).stem + ".o")
                result = subprocess.run([
                    build.ZIG, "cc", "-I", str(patched / "include"),
                    *build.COMMON_FLAGS, *flags,
                    "-Werror=implicit-function-declaration",
                    "-Werror=incompatible-pointer-types", "-Werror=int-conversion",
                    "-c", str(source), "-o", str(obj),
                ], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0,
                                 f"{rel} failed native compilation:\n{result.stderr}")
                self.assertTrue(obj.is_file())


if __name__ == "__main__":
    unittest.main()
