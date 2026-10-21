// Lightweight optical character recognition, written from first principles in
// plain C++ so the editor needs no third-party OCR library and no bundled
// language data. The pipeline follows the classic pre-ML OCR design used by
// Tesseract (see its ICDAR 2007 overview): adaptive binarisation, connected-
// component blob finding, text-line assembly, word segmentation, then per-glyph
// recognition. Recognition is structural (normalised bitmap templates plus
// topological/geometric features) rather than a trained neural model, so it aims
// at clean, machine-printed scans rather than handwriting.
//
// The module deals only with a single 8-bit grayscale page image; the caller
// renders the page (the editor already does) and receives recognised words with
// their pixel bounding boxes. It is deliberately free of Windows/PDFium types so
// it can be exercised by a headless probe.
#pragma once

#include <string>
#include <vector>

namespace ocr {

struct Word
{
  std::wstring text;      // recognised characters, in reading order
  int x = 0, y = 0;       // top-left of the word's bounding box, in pixels
  int w = 0, h = 0;       // bounding-box size in pixels
  int line = 0;           // line index within the page (reading order)
  double confidence = 0;  // 0..1, minimum glyph confidence in the word
};

// The recogniser needs reference glyph shapes. To stay free of any platform
// dependency, the module does not render fonts itself; instead the caller
// installs a hook that renders one character into a 16x16 coverage grid
// (row-major, values 0..255). On Windows this is backed by GDI. Called once,
// lazily, on the first recognition.
using PrototypeRenderFn = bool (*)(wchar_t ch, unsigned char* outGrid);
void SetPrototypeRenderer(PrototypeRenderFn fn);

// Recognise all machine-printed text in an 8-bit grayscale image.
//   pixels : row-major, top-to-bottom, one byte per pixel (0 = black)
//   w, h   : image dimensions in pixels
// Returns words ordered top-to-bottom, left-to-right.
std::vector<Word> RecognizePage(const unsigned char* pixels, int w, int h);

// Convenience: join the recognised words into plain text, one line per text
// line, words separated by single spaces. `pageHeight` is unused but kept so the
// caller's call-site reads naturally if line breaks are later added.
std::wstring ToPlainText(const std::vector<Word>& words);

}  // namespace ocr
