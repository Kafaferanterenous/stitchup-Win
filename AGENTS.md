# AGENTS.md - Stitchup PDF Editor

Windows PDF viewer/editor, single-file Win32 app (`src\app.cpp`) built on PDFium.

## Build & verify
- Build with `cmd /c Build.cmd` -> outputs `dist\Stitchup.exe` + `dist\pdfium.dll`.
- Build.cmd runs the `--self-test` afterwards; results land in `test_result.txt`.
- Work is only finished when `test_result.txt` ends in `SUMMARY N passed, 0 failed`.
- Build runs with warnings-as-errors; unused locals etc. will fail the build.

## Daily backup (user requirement)
- **At the start of every session/workday (from 2026-10-01 onward), run `backup.cmd`
  BEFORE making any changes.**
- It zips the current source (src/, resources/, third_party/, build scripts, docs)
  together with the compiled version (dist\Stitchup.exe, dist\pdfium.dll) into
  `archive\<yyyy_MM_dd> - <HH_mm>_Stitchup.zip`, e.g. `2026_09_29 - 12_00_Stitchup.zip`.
- Confirm the zip exists and note its name in the session summary before proceeding.

## Archive / cleanup convention
- Anything no longer required is **zip-compressed into `archive\`** (dated name) and
  then removed from its original location - never deleted outright.
- `archive\` is gitignored (backups, not source).