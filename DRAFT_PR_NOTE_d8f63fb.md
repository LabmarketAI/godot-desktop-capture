# Draft PR Note - d8f63fb

## Title
Normalize whitespace in build and desktop capture sources

## Commit
- SHA: d8f63fb
- Repository: LabmarketAI/godot-desktop-capture
- Branch: main

## Summary
This change normalizes formatting/whitespace in shell script and C++ source without functional behavior changes.

## Files Changed
- build_local.sh
- src/desktop_capture_texture.cpp

## Risk Assessment
Very low. Verified as whitespace-only when ignoring whitespace in git diff.

## Validation
- `git show -w --stat d8f63fb` returns no changed-file stats.
- Token-level review did not reveal functional edits.

## CI Status (as of 2026-03-13)
- Linux Release x86_64: success
- Linux Debug x86_64: success
- Windows Release x86_64: failure
- Windows Debug x86_64: failure
- Package release zip: skipped
- Workflow URL: https://github.com/LabmarketAI/godot-desktop-capture/actions/runs/23076681920

## Follow-up Actions
1. Triage failing Windows jobs and capture first failing step/log.
2. Confirm line-ending/formatting assumptions in Windows CI shell/toolchain.
3. Re-run workflow after remediation.
