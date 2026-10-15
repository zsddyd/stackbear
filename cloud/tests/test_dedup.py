"""Disfluency cleanup must remove stutters without touching legitimate
reduplication - the risk that makes this worth testing rather than eyeballing."""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import dedup  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


def test_real_cases_are_cleaned() -> None:
    cases = {
        "那个那个屏幕那个屏幕我们什么时候开始呀": "那个屏幕我们什么时候开始呀",
        "你那一堆同你那一堆同轴线之前在哪放着": "你那一堆同轴线之前在哪放着",
        "那个屏幕那个屏幕": "那个屏幕",
        "我我我我说的是这个": "我我说的是这个",
        "这个这个配色还好": "这个配色还好",
    }
    for raw, want in cases.items():
        got = dedup.clean(raw)
        check(got == want, f"{raw!r} -> {got!r}, want {want!r}")


def test_legitimate_reduplication_is_kept() -> None:
    """Every one of these is a real word or a deliberate form. Corrupting them
    would be worse than leaving a stutter in."""
    keep = [
        # AA words (one character repeated) - excluded by the 2-character minimum
        "谢谢", "看看", "慢慢", "妈妈", "哥哥", "姐姐", "刚刚", "常常", "好好",
        "试试", "走走", "点点", "天天", "夜夜", "人人",
        # AABB / ABAC idioms
        "快快乐乐", "高高兴兴", "来来往往", "大大小小", "平平安安", "干干净净",
        # ABAB verbs and emphasis - protected explicitly
        "研究研究", "商量商量", "讨论讨论", "休息休息", "考虑考虑", "整理整理",
        "非常非常", "特别特别", "很多很多", "好久好久", "好多好多",
    ]
    for word in keep:
        got = dedup.clean(word)
        check(got == word, f"legitimate {word!r} was changed to {got!r}")


def test_embedded_legitimate_word_is_kept() -> None:
    """A real word inside a sentence must survive."""
    s = "谢谢你和妈妈,我们再研究研究"
    check(dedup.clean(s) == s, f"sentence changed: {dedup.clean(s)!r}")


def test_single_character_repeats_survive() -> None:
    # The 2-character minimum means these never qualify, even without protection.
    for word in ("好好", "多多", "少少", "人人"):
        check(dedup.clean(word) == word, f"{word!r} changed")


def test_empty_and_unchanged() -> None:
    check(dedup.clean("") == "", "empty")
    check(dedup.clean("今天天气不错") == "今天天气不错", "no repeats -> unchanged")


def test_punctuation_only_repeats_are_kept() -> None:
    """A repeated run of punctuation is not a disfluency."""
    check(dedup.clean("。。。") == "。。。", "punctuation kept")


def test_repeated_long_phrase() -> None:
    """A restarted sentence collapses the same way a stutter does."""
    s = "我看一下我看一下这个接口之前有没有过"
    got = dedup.clean(s)
    check(got == "我看一下这个接口之前有没有过", f"got {got!r}")


def main() -> int:
    test_real_cases_are_cleaned()
    test_legitimate_reduplication_is_kept()
    test_embedded_legitimate_word_is_kept()
    test_single_character_repeats_survive()
    test_empty_and_unchanged()
    test_punctuation_only_repeats_are_kept()
    test_repeated_long_phrase()
    if FAILURES:
        print(f"test_dedup: {FAILURES} failure(s)")
        return 1
    print("test_dedup: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
