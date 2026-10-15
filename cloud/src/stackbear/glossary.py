"""Domain vocabulary correction applied to ASR output.

A recogniser trained on general speech mishears the words a specific person
actually uses: colleague names, product names, internal jargon. Two ways to fix
that:

  * 热词/hotwords - the decoder is biased during recognition. sherpa-onnx's
    SenseVoice exposes this only as a homophone replacer built from a general
    pinyin table (`hr_dict_dir`/`hr_lexicon`/`hr_rule_fsts`); measured on a real
    recording it changed nothing, because that table does not contain the
    domain's proper nouns.
  * a glossary applied afterwards - explicit, auditable, and independent of the
    engine (it works for the cloud providers too).

This module is the second approach. Rules are plain text so the vocabulary can
grow without touching code:

    # comment
    运哥 => 云哥
    同轴县 => 同轴线
    LE => C798-ASE

Longer wrong-forms are applied first so a shorter rule cannot damage a longer
match, and a replacement never re-scans its own output (otherwise a rule could
chain in a way nobody intended).

An all-ASCII wrong-form (a product code such as `LE`) additionally requires
ASCII boundaries, so it cannot corrupt a longer Latin word: `LE => C798-ASE`
rewrites "对LE的屏幕" but leaves "LEVEL" and "file-led" alone.
"""

from __future__ import annotations

import os
import pathlib
import re

# 默认词表放在仓库的 cloud/glossary.txt;可用 STACKBEAR_GLOSSARY 覆盖。
DEFAULT_PATH = pathlib.Path(__file__).resolve().parents[2] / "glossary.txt"


def load(path: pathlib.Path | str | None = None) -> list[tuple[str, str]]:
    """Read `wrong => right` rules, longest wrong-form first."""
    target = pathlib.Path(path) if path else pathlib.Path(
        os.environ.get("STACKBEAR_GLOSSARY", DEFAULT_PATH))
    rules: list[tuple[str, str]] = []
    try:
        text = target.read_text(encoding="utf-8")
    except OSError:
        return rules
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=>" not in line:
            continue
        wrong, right = (part.strip() for part in line.split("=>", 1))
        if wrong and wrong != right:
            rules.append((wrong, right))
    rules.sort(key=lambda r: len(r[0]), reverse=True)
    return rules


def apply(text: str, rules: list[tuple[str, str]] | None = None) -> str:
    """Apply the glossary. Replaced text is not rescanned."""
    if not text:
        return text
    if rules is None:
        rules = load()
    if not rules:
        return text
    # 用一个正则一次扫过,替换结果不参与后续匹配 —— 避免规则串联出意料之外的词。
    #
    # 纯 ASCII 的错形(如产品代号 "LE")加 ASCII 边界:否则 "LE" 会命中 "LEVEL" /
    # "file-led" 里的子串。中文错形不需要边界,中文语境里没有"词"的边界概念,
    # 而且加边界会漏掉"对LE的"这种前后紧贴汉字的写法。
    pieces = []
    for wrong, _ in rules:
        escaped = re.escape(wrong)
        if wrong.isascii() and wrong.isalnum():
            pieces.append(rf"(?<![A-Za-z0-9]){escaped}(?![A-Za-z0-9])")
        else:
            pieces.append(escaped)
    pattern = re.compile("|".join(pieces))
    mapping = dict(rules)
    return pattern.sub(lambda m: mapping[m.group(0)], text)
