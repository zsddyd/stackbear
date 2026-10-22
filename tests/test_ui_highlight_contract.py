#!/usr/bin/env python3
"""Static contract: a rebuilt page puts the highlight back where the cursor is.

List pages are built on demand and deleted on leave, but the cursor and scroll
window deliberately survive that cycle so the user returns to the row they left.
The highlight is an LVGL object that gets recreated, so its position has to be
recomputed from the restored cursor. Hardcoding the first row instead leaves the
highlight on row 0 while the cursor sits on the last row - and the visible
symptom is "DOWN does nothing when entering the page", because the cursor is
already clamped at the bottom. Pressing UP once appears to fix it, which is what
makes the real cause easy to misread as a stuck key.

The UI cannot be exercised without LVGL, so this pins the wiring at the source
level, the same way test_battery_contract.py does.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class HighlightPositionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ui = (ROOT / "main" / "sb_ui.c").read_text(encoding="utf-8")

    def function_body(self, name: str) -> str:
        # Anchor to a definition at line start: a bare `name(` also appears inside
        # prose comments, and matching there would splice one body onto another's
        # opening brace.
        match = re.search(
            rf"^(?:static\s+)?[\w \*]*\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
            self.ui,
            re.MULTILINE,
        )
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

    def test_page_build_restores_the_highlight_from_the_cursor(self) -> None:
        body = self.function_body("page_build")
        self.assertIn("page_sel_y(", body)
        self.assertNotIn(
            "lv_obj_set_pos(p->sel, ROW_X, ROW_Y0)",
            body,
            "the highlight must not be hardcoded to the first row: the cursor "
            "survives a page rebuild, so the two would disagree",
        )

    def test_one_place_computes_the_highlight_row(self) -> None:
        # The animation path and the build path must agree. A second copy of the
        # arithmetic is how they drifted apart in the first place, so the single
        # source has to go through the scroll window rather than repeat
        # `cursor - scroll` itself.
        self.assertIn("sb_scroll_slot(", self.function_body("page_sel_y"))


if __name__ == "__main__":
    unittest.main()
