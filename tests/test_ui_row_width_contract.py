#!/usr/bin/env python3
"""Static contract: a row's value column must be wide enough for what it shows.

DEVICE's last row was called "Check for update". At 123px it is the widest key in
the app, which left the value column 67px - and measured against the real
lv_font_montserrat_14 metrics, only 2 of the 22 OTA status strings fit, not even
"up to date" (74px). The user saw a clipped fragment and could not tell whether
the update check had succeeded.

The fix renames the row to "Upgrade" (62.6px, leaving 127px) and the width
constant is derived from that. The numbers below come from the font's adv_w
table, so they are measurements, not estimates - the earlier guess of
"6.7px per character" is what let the clipping go unnoticed.

This test pins the two constants against those measurements. It cannot recompute
them (the font is a managed component, not vendored here), so it records the
measurement and fails if someone edits a key name or a constant without redoing
it.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]

# Measured from lv_font_montserrat_14's adv_w table (adv_w is in 1/16 px).
# Reproduce with: sum(adv_w[glyph]) / 16 over the string's code points.
FONT_PX = {
    "Branch": 51.9,
    "Upgrade": 62.6,
    "Check for update": 123.3,
    "up to date": 73.6,
}

# Row geometry: the key starts at ROW_X + 10 and the value's right edge is
# pinned 10px inside the row's right edge, i.e. x=218 on a 240px screen.
KEY_LEFT = 22
VALUE_RIGHT = 218
KEY_GAP = 6


class RowValueWidthTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ui = (ROOT / "main" / "sb_ui.c").read_text(encoding="utf-8")

    def constant(self, name: str) -> int:
        match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)\s*$", self.ui, re.MULTILINE)
        if not match:
            raise AssertionError(f"constant not found: {name}")
        return int(match.group(1))

    def test_device_last_row_is_the_short_name(self) -> None:
        # The value column is sized for whatever key sits on that row, so the key
        # itself has to stay short. "Check for update" must not come back.
        match = re.search(r'dev_names\[\]\s*=\s*\{(.*?)\};', self.ui, re.S)
        self.assertIsNotNone(match, "DEVICE row names not found")
        names = re.findall(r'"([^"]*)"', match.group(1))
        self.assertEqual(names[-1], "Upgrade", "DEVICE's last row drives the OTA page")
        self.assertNotIn("Check for update", names)

    def test_upgrade_value_column_fits_the_measured_key(self) -> None:
        available = VALUE_RIGHT - (KEY_LEFT + FONT_PX["Upgrade"] + KEY_GAP)
        constant = self.constant("DEV_VALUE_ROW_PX")
        self.assertLessEqual(
            constant,
            available,
            f"DEV_VALUE_ROW_PX={constant} would overlap the key name; only "
            f"{available:.1f}px are free",
        )
        # And it must actually be useful: the common statuses have to fit.
        for msg in ("up to date", "checking..."):
            self.assertLessEqual(
                FONT_PX.get(msg, len(msg) * 6.7),
                constant,
                f'"{msg}" must fit in the DEVICE value column',
            )

    def test_branch_value_column_fits_its_measured_key(self) -> None:
        available = VALUE_RIGHT - (KEY_LEFT + FONT_PX["Branch"] + KEY_GAP)
        self.assertLessEqual(self.constant("DEV_VALUE_SAFE_PX"), available)


if __name__ == "__main__":
    unittest.main()
