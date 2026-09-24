# Stitchup PDF Editor

Portable Windows PDF editor + viewer. Nitro Pro v7-style layout: toolbar, page
thumbnails, page navigation, zoom, and editing (add/delete/rotate/reorder/import
pages, save, save-as).

## Technology

- UI: Native Win32 API (single window, custom toolbar, thumbnails panel,
  canvas, status bar, splitter), DPI aware
- PDF engine: **PDFium** (Chromium, BSD-3-Clause), prebuilt release
  `chromium/8057` for Windows x64
- Build: Microsoft C++ (MSVC), CMake + NMake; `/MT` static CRT, `/W4 /WX`
- Portable package: `Stitchup.exe` + `pdfium.dll` in one folder, no install

## Build

```
call Build.cmd
```

Produces `dist\Stitchup.exe` + `dist\pdfium.dll`.

## Run

- `Stitchup.exe` - launch the editor GUI
- `Stitchup.exe file.pdf` - open a PDF at startup
- `Stitchup.exe --self-test` - headless self-test, writes `test_result.txt`
- `Stitchup.exe --demo out.pdf [pages]` - generate a sample multi-page PDF

## Features

- Nitro Pro v7-style shell: ribbon (Home/View/Tools tabs with groups: Document,
  Pages, Zoom, Navigate, Panes), left navigation pane (Page Thumbnails,
  Bookmarks with outline tree), splitter, canvas, status bar
- Multi-document tabs: open several files at once and switch between them
  (Ctrl+Tab / Ctrl+Shift+Tab, New Tab Ctrl+T, Close Tab Ctrl+Shift+F4, or click
  the strip at the top of the window); each tab keeps its own page, zoom and
  scroll position
- View: two-page side-by-side spread (`F5` or View > Two-Page Spread) and a
  toggleable left navigation pane (`F8` or View > Sidebar) for extra canvas
  room; pages are centered horizontally in the main display
- File menu: New, Open, Save (atomic: temp file + replace), Save As, Import PDF
- Edit: rotate page CW/CCW, delete page, add blank page (Letter/A4), drag pages
  in the thumbnails panel to reorder
- View: zoom in/out, 100%, fit width, fit page, previous/next page, and a
  Dark Mode toggle (View > Dark Mode, `Ctrl+D`, remembered between runs)
- Modern flat UI: flat ribbon (hover/pressed states, accent brand strip,
  labelled group cards), Segoe UI text, embedded multi-size app icon, and a
  light/dark color theme
- Bookmarks pane lists the PDF outline (nested); clicking a bookmark jumps to
  its page
- Annotate (Home > Annotate group or Edit > Annotate menu): insert highlight,
  underline, sticky note, text box, shape and stamp annotations on the current
  page; annotations persist through Save and keep their rendered appearance
- Links: clicking an internal link jumps to its destination page; clicking a
  URI link opens the URL in the default browser (hand cursor over link areas)
- Content object editing (Edit > Select / Move Content Object, or the Home >
  Edit group Select tool): click any content object — text run, vector path or
  image — on the current page to select it (accent bounding box + handles), then
  drag to move it, Ctrl+Del (or Menu > Delete Selected Object) to remove it,
  RecColor to recolor the fill, or Edit Text (double-click) to replace the text
  run; edits persist through Save and reload (4 object tests added)
- Page management (Home > Pages): Extract the current page into its own PDF,
  Split a document into one file per page, and Auto-Crop the current page to
  its content (white margins trimmed via an ink-bounding-box render pass)
- Export Text (File > Export Text): saves every page's extractable text as a
  UTF-8 (BOM) .txt file
- Export CSV (File > Export CSV): writes a UTF-8 (BOM) `.csv` with one
  `Page,Width (pt),Height (pt),Text chars,Annotations` row per page
- Watermark (File > Watermark): stamp every page with custom text in filler
  gray at 50% opacity. The dialog prefills "Confidential", with a 16/24/36/
  48/64/96 pt size selector and Center (diagonal, 45 degrees) / Top center /
  Tiled placements; the document is saved in place and reloaded
- Security: password-protected PDFs are detected and unlocked through a modal
  password dialog (up to 3 attempts, then an error box); wrong passwords are
  rejected by PDFium
- Security: Save As Encrypted (File > Save As Encrypted...) writes a
  password-protected copy of the open document (PDF revision 3 / 128-bit RC4
  standard security handler, user + optional owner password). The saved file
  re-opens against the bundled PDFium only with the correct password; the open
  document is left unchanged
- Drag & drop a PDF onto the window to open it; open via command-line argument
- Shortcuts: Ctrl+N/O/S, Ctrl+Shift+S, Ctrl+R (rotate), Ctrl+[/Ctrl+], Delete,
  Ctrl+0/1/2 (100%/fit width/fit page), Ctrl+D (dark mode), PgUp/PgDn,
  Ctrl+wheel to zoom, F5 (two-page spread), F8 (toggle sidebar), Ctrl+T
  (new tab), Ctrl+Tab/Ctrl+Shift+Tab (next/previous tab)

## Third-party components

| Component | Version | License        |
|-----------|---------|----------------|
| PDFium    | chromium/8057 | BSD-3-Clause (see `third_party/pdfium/LICENSE`) |

`pdfium.dll` is the unmodified binary release from the `pdfium-binaries`
project (github.com/bblanchon/pdfium-binaries). Non-GPL build.

## Known limits / roadmap

- PDFium is currently a DLL (portable folder, not single exe). A static
  single-exe build is planned but first requires building PDFium with the
  static CRT from source (the prebuilt import lib is not static).
- Shell + annotations + link activation + page management (extract/split/
  auto-crop) + text export + CSV export + password-unlock + watermarks +
  encrypt-on-save are done. Rendering, page model,
  editing operations, ribbon navigation, the bookmarks pane, annotation
  creation/persistence, link navigation and page extract/split/crop are
  functional.
  Note: link *creation* (drawing new links) requires a newer PDFium runtime
  than the bundled `pdfium.dll`, which exposes only link reading.
- Tools tab currently shows a placeholder caption until the advanced feature
  sets land.