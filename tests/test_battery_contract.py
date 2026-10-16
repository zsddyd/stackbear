#!/usr/bin/env python3
"""Static contract: every screen carries the battery readout.

Battery used to appear only on HOME, so stepping into a sub-page hid it - and
"how much charge is left" is exactly the thing you want to glance at while you
are in one. The fix routes every screen through `base_screen()`, which creates
and registers the readout centrally. This test pins that down at the source
level, because the UI itself cannot be exercised without LVGL.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BatteryOnEveryScreenTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ui = (ROOT / "main" / "sb_ui.c").read_text(encoding="utf-8")

    def function_body(self, name: str) -> str:
        match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", self.ui)
        if not match:
            raise AssertionError(f"function not found: {name}")
        start = match.end() - 1
        depth = 0
        for index in range(start, len(self.ui)):
            if self.ui[index] == "{":
                depth += 1
            elif self.ui[index] == "}":
                depth -= 1
                if depth == 0:
                    return self.ui[start + 1:index]
        raise AssertionError(f"function is unterminated: {name}")

    def test_base_screen_creates_and_registers_the_readout(self) -> None:
        body = self.function_body("base_screen")
        self.assertIn("LV_ALIGN_TOP_RIGHT", body)
        self.assertIn("s_batt_label", body)

    def test_every_screen_is_built_by_base_screen(self) -> None:
        # Each page either calls base_screen() directly or goes through
        # page_init(), which does. A new page that skipped it would silently
        # lose the battery, so require one of the two here.
        for name in ["build_home", "build_help", "build_provision", "page_init"]:
            self.assertIn(
                "base_screen(",
                self.function_body(name),
                f"{name}() must create its screen through base_screen()",
            )

    def test_the_gauge_is_read_once_for_all_screens(self) -> None:
        self.assertEqual(
            self.ui.count("bsp_battery_soc()"),
            1,
            "the fuel gauge must be read in exactly one place, not per page",
        )
        self.assertIn("battery_refresh()", self.function_body("refresh"))


if __name__ == "__main__":
    unittest.main()
