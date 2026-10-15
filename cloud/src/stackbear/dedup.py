"""Remove disfluent repetition from a transcript.

ASR faithfully transcribes hesitation, so output contains things like
「那个那个屏幕那个屏幕」or a restarted phrase「你那一堆同你那一堆同轴线」.
That is what was said, but it reads badly.

The risk in cleaning it is real: Chinese reduplication is often a *word*.
谢谢 / 看看 / 慢慢 / 妈妈 / 哥哥 / 刚刚 are single words, and 研究研究 /
商量商量 are the tentative form of a verb. A naive "collapse any repeat"
would corrupt all of them. So:

  * only units of **two or more characters** are considered - that alone keeps
    every AA form (谢谢, 妈妈, 慢慢, 刚刚, ...) intact, since those repeat one
    character;
  * a protection list covers the remaining legitimate cases (ABAB verbs such as
    研究研究, and emphatic doubles such as 非常非常);
  * the two copies must be *adjacent* - a pause between them (「那个,那个」) is
    left alone;
  * the original text is preserved in the result payload, so cleaning is never
    destructive.
"""

from __future__ import annotations

# 合法叠词:命中则整段保留。以 ABAB 动词与强调式为主(AA 形式已由"≥2 字"排除)。
PROTECTED = frozenset({
    # ABAB 动词(尝试/短暂体)
    "研究研究", "商量商量", "讨论讨论", "休息休息", "打扫打扫", "考虑考虑",
    "活动活动", "了解了解", "学习学习", "练习练习", "检查检查", "整理整理",
    "收拾收拾", "熟悉熟悉", "适应适应", "锻炼锻炼", "放松放松", "照顾照顾",
    "打听打听", "观察观察", "琢磨琢磨", "合计合计", "将就将就", "凑合凑合",
    # 强调式(去一个会丢语气)
    "非常非常", "特别特别", "很多很多", "好久好久", "很少很少", "好多好多",
    "太远太远", "有点有点",
})

# 一次最多看多少字作为重复单元,避免长句自比较。
MAX_UNIT = 20


def _meaningful(unit: str) -> bool:
    """A unit made only of punctuation/space is not a disfluency."""
    return any(ch.isalnum() or "\u4e00" <= ch <= "\u9fff" for ch in unit)


def clean(text: str, protected: frozenset[str] = PROTECTED, max_unit: int = MAX_UNIT) -> str:
    """Collapse adjacent repeated spans into one copy.

    Repeats are removed longest-unit-first and repeatedly, so a restarted phrase
    (`你那一堆同` + `你那一堆同轴线`) collapses the same way a stutter does.
    """
    if not text:
        return text
    changed = True
    while changed:
        changed = False
        limit = min(max_unit, len(text) // 2)
        for unit in range(limit, 1, -1):          # longest unit first
            for i in range(0, len(text) - 2 * unit + 1):
                span = text[i:i + unit]
                if span != text[i + unit:i + 2 * unit]:
                    continue
                if not _meaningful(span):
                    continue
                # 保护名单按"重复后的整体"判断:unit=2 命中「研究」时,要看的是
                # 「研究研究」。早期版本只查 span,于是 ABAB 动词全被砍成了 AB。
                if span + span in protected or span in protected:
                    continue
                # 去掉后一个副本:第一个副本的位置因此保持不变。
                text = text[:i + unit] + text[i + 2 * unit:]
                changed = True
                break
            if changed:
                break
    return text
