"""The domain glossary applied to transcripts."""

from __future__ import annotations

import os
import pathlib
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import glossary  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


def _write(text: str) -> pathlib.Path:
    path = pathlib.Path(tempfile.mkdtemp()) / "glossary.txt"
    path.write_text(text, encoding="utf-8")
    return path


def test_basic_replacement() -> None:
    rules = glossary.load(_write("运哥 => 云哥\n同轴县 => 同轴线\n"))
    out = glossary.apply("下午运哥一直不在这,同轴县的线呢", rules)
    check(out == "下午云哥一直不在这,同轴线的线呢", f"got {out!r}")


def test_comments_and_blank_lines_ignored() -> None:
    rules = glossary.load(_write("# 注释\n\n   \n甲 => 乙\n# 尾注\n"))
    check(rules == [("甲", "乙")], f"rules {rules}")


def test_longest_rule_wins() -> None:
    """A short rule must not corrupt a longer match."""
    rules = glossary.load(_write("屏幕 => 屏\n大屏幕 => 大屏\n"))
    check(rules[0][0] == "大屏幕", f"longest first: {rules}")
    check(glossary.apply("大屏幕坏了", rules) == "大屏坏了", "longest rule applied")
    check(glossary.apply("屏幕坏了", rules) == "屏坏了", "short rule still works")


def test_no_chained_rescan() -> None:
    """A replacement must not be rescanned, or rules chain unintentionally."""
    rules = glossary.load(_write("甲 => 乙\n乙 => 丙\n"))
    check(glossary.apply("甲", rules) == "乙", "one pass only")


def test_ascii_boundaries() -> None:
    """An all-ASCII wrong-form (a product code) must not corrupt longer Latin
    words: LE => C798-ASE rewrites "对LE的" but leaves LEVEL and file-led alone."""
    rules = [("LE", "C798-ASE")]
    check(glossary.apply("对LE的屏幕", rules) == "对C798-ASE的屏幕", "code next to Han")
    check(glossary.apply("LE的", rules) == "C798-ASE的", "code before Han")
    check(glossary.apply("LEVEL", rules) == "LEVEL", "must not touch LEVEL")
    check(glossary.apply("file-led", rules) == "file-led", "must not touch file-led")
    check(glossary.apply("LE", rules) == "C798-ASE", "bare code")
    # A rule that is not pure-ascii/alnum gets no boundary (Chinese has no word gaps).
    rules2 = [("应哥", "运哥")]
    check(glossary.apply("应哥哎", rules2) == "运哥哎", "Chinese rule unaffected by boundary logic")


def test_missing_file_is_not_fatal() -> None:
    check(glossary.load("/nonexistent/path/xyz.txt") == [], "missing file -> no rules")
    check(glossary.apply("原文", []) == "原文", "no rules -> unchanged")
    check(glossary.apply("", []) == "", "empty text")


def test_empty_text_and_noop_rules() -> None:
    # A rule whose sides are identical is dropped (it can do nothing).
    rules = glossary.load(_write("同 => 同\n"))
    check(rules == [], f"identity rule dropped: {rules}")


def main() -> int:
    test_basic_replacement()
    test_ascii_boundaries()
    test_comments_and_blank_lines_ignored()
    test_longest_rule_wins()
    test_no_chained_rescan()
    test_missing_file_is_not_fatal()
    test_empty_text_and_noop_rules()
    if FAILURES:
        print(f"test_glossary: {FAILURES} failure(s)")
        return 1
    print("test_glossary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
