# Upstream sources (read-only reference, not in git)

Cloned into `upstream/` on 2026-11-14 and ignored by git. To recreate:

| Path | Source | Commit |
|---|---|---|
| `upstream/powersdr-2.7.2` | https://github.com/M0LTE/PowerSDR.git | f6aed32a5411798e93ed2d052c4e4ec83967e966 |
| `upstream/powersdr-ke9ns-2.8.0` | https://github.com/ke9ns/PowerSDR-KE9NS-v2.8.0.git | 0a01a9d8d1221060e218de524c47eeb7df0cd278 |
| `upstream/dice.c` | Linux kernel `sound/firewire/dice/dice.c` (snd-dice) | sha256 `04991a5c4a7320cd38d6aa357998a4f48f1eb74aa386d66e0ab09b0f18043465` |

    git clone https://github.com/M0LTE/PowerSDR.git upstream/powersdr-2.7.2 && git -C upstream/powersdr-2.7.2 checkout f6aed32a5411798e93ed2d052c4e4ec83967e966
    git clone https://github.com/ke9ns/PowerSDR-KE9NS-v2.8.0.git upstream/powersdr-ke9ns-2.8.0 && git -C upstream/powersdr-ke9ns-2.8.0 checkout 0a01a9d8d1221060e218de524c47eeb7df0cd278

`tools/tests/test_docs.py` skips its PowerSDR-source checks when `upstream/powersdr-2.7.2` is absent. Downloads in `docs/` and `software/`: see DOWNLOADS.md and MANIFEST.tsv.
