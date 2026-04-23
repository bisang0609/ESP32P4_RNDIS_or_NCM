# AI Handoff

Purpose: Keep Codex/AI work continuous across machines and chat sessions.

## 1) Session Meta
- Project: ESP32P4_RNDIS_or_NCM
- Last updated (KST): 2026-04-23 23:13
- Owner: bisan
- Branch: YTMD
- HEAD commit: 53cac47 (before save commit)
- Build profile / Target: ESP-IDF v6.0 / esp32p4

## 2) Current Goal
- Final goal for this task: Stabilize YTMD UI typography and runtime behavior while keeping build/flash workflow practical.
- Current stage (one line): Title/artist font size tuned and validated by build; savepoint commit requested.

## 3) Done (This Session)
- [x] Updated runtime TTF sizes to title=34, artist/next=22.
- [x] Updated UI layout sizes/positions for larger text in `main/ui/screens.c`.
- [x] Enabled infinite repeat for label scroll animation template (`lv_anim_set_repeat_count(&anim, -1)`).
- [x] Kept LittleFS flash behavior toggle in `main/CMakeLists.txt` (default: do not auto-flash storage on `idf.py flash`).
- [x] Build verified with `idf.py build`.

## 4) In Progress / Next
- In progress: On-device visual/runtime verification after latest typography/layout updates.
- Next top priority: Flash and monitor on target board, confirm no clipping and expected scroll behavior.
- Next secondary priority: If needed, tune scroll speed/delay for readability.

## 5) Decisions (Why)
- Decision 1:
  - Reason: Use larger fonts (34/22) for readability on target display.
  - Alternative: Keep 26/16, but readability was weaker.
- Decision 2:
  - Reason: Set scroll animation repeat to infinite so long text does not stop after one cycle.
  - Alternative: Use default repeat count from template (resulted in one-cycle stop).

## 6) Blockers / Risks
- Blocker: None.
- Risk: If USB role changes during runtime, serial monitor can disconnect (ClearCommError on host side).
- Needed info or access: Stable serial/UART logging path when USB interface re-enumerates.

## 7) Changed Files
- `main/CMakeLists.txt` - LittleFS flash toggle comments in Korean, default non-`FLASH_IN_PROJECT` line enabled.
- `main/ui_display.c` - Runtime TTF sizes to 34/22; font chain uses runtime TTF as base; debug-only helpers wrapped by `FONT_LOAD_BIN_FALLBACK`.
- `main/ui/screens.c` - Title/artist/next label sizes and positions adjusted; animation repeat set to infinite.
- `sdkconfig` - Enabled `CONFIG_LV_FONT_MONTSERRAT_16` and `CONFIG_LV_FONT_MONTSERRAT_34`.
- `AI_HANDOFF.md` - Updated session snapshot.

## 8) Commands Used
```bash
# build
idf.py build

# flash (app only)
idf.py -p <PORT> app-flash

# monitor
idf.py -p <PORT> monitor
```

## 9) Verification
- Build result: PASS (`idf.py build`, 2026-04-23).
- Runtime check: Not completed in this session after final layout/font edits.
- Not verified yet: On-device visual clipping and scroll UX under real metadata strings.

## 10) Next Session Prompt (Copy/Paste)
Continue from this handoff.
- Goal: Run on-device validation for updated title/artist/next typography and scroll behavior.
- Current state: Local changes are prepared and committed as a savepoint on `YTMD`.
- Blocked point: Need board-level runtime confirmation.
- Priority: (1) app-flash + monitor (2) verify clipping/scroll (3) fine-tune speed or dimensions only if needed.
- Related files: `main/ui/screens.c`, `main/ui_display.c`, `main/player_ui.c`, `main/CMakeLists.txt`, `sdkconfig`

## 11) Notes
- If monitor disconnects with `ClearCommError`, re-check COM port and reconnect monitor after device re-enumeration.

---

## Quick End-of-Session Checklist
- [x] Update branch and HEAD commit
- [x] Update Done and In Progress
- [x] Update Changed Files
- [x] Fill Next Session Prompt
