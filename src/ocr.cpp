// Bottom-up OCR pipeline. See ocr.h for the design notes.
//
// Stages:
//   1. Binarisation   - Otsu global threshold, refined per-tile by Sauvola so
//                       uneven scans and shading still yield clean ink.
//   2. Blobs          - connected components (8-connectivity) of ink, with a
//                       hole count (Euler number) per blob.
//   3. Lines          - blobs grouped into text lines by vertical overlap,
//                       tolerant of a small page skew.
//   4. Words          - a line is split into words at inter-glyph gaps that
//                       exceed a space threshold; glyphs that touch are kept as
//                       one blob for the recogniser to segment internally.
//   5. Recognition    - each glyph is normalised to a fixed grid and matched
//                       against embedded prototypes by a hybrid of pixel
//                       correlation and topological features (holes, aspect,
//                       row/column projection profiles). Multi-blob pieces
//                       (i-dot, umlaut, split glyphs) are merged first.
#include "ocr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <unordered_map>

namespace ocr {
namespace {

// ---------------------------------------------------------------------------
// Small geometry helpers
// ---------------------------------------------------------------------------
struct Box
{
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
  int w() const { return x1 - x0 + 1; }
  int h() const { return y1 - y0 + 1; }
  int area() const { return w() * h(); }
  int cx() const { return (x0 + x1) / 2; }
  int cy() const { return (y0 + y1) / 2; }
};

// Fraction of the smaller box's vertical extent that overlaps the other box.
static double VOverlap(const Box& a, const Box& b)
{
  const int lo = std::max(a.y0, b.y0);
  const int hi = std::min(a.y1, b.y1);
  if (hi < lo) return 0.0;
  const int ov = hi - lo + 1;
  const int m = std::min(a.h(), b.h());
  return m > 0 ? (double)ov / m : 0.0;
}

// ---------------------------------------------------------------------------
// Stage 1: binarisation
// ---------------------------------------------------------------------------
// Otsu's method: pick the threshold that maximises between-class variance of the
// grayscale histogram. Gives a solid global split for a whole page.
static int OtsuThreshold(const std::vector<unsigned char>& g)
{
  int hist[256] = {0};
  for (unsigned char v : g) hist[v]++;
  const long total = (long)g.size();
  if (total == 0) return 128;
  double sum = 0;
  for (int i = 0; i < 256; ++i) sum += (double)i * hist[i];
  double sumB = 0;
  long wB = 0;
  double best = -1;
  int thr = 128;
  for (int t = 0; t < 256; ++t)
  {
    wB += hist[t];
    if (wB == 0) continue;
    const long wF = total - wB;
    if (wF == 0) break;
    sumB += (double)t * hist[t];
    const double mB = sumB / wB;
    const double mF = (sum - sumB) / wF;
    const double between = (double)wB * wF * (mB - mF) * (mB - mF);
    if (between > best) { best = between; thr = t; }
  }
  return thr;
}

// Sauvola local threshold computed from integral images of the grayscale and its
// square, so a variable-width window costs O(1) per pixel and the whole pass is
// linear. Falls back to the Otsu value in flat areas (where the local standard
// deviation is tiny) so blank paper never turns to speckle.
static void BinarizeSauvola(const std::vector<unsigned char>& g, int w, int h,
                            int globalThr, std::vector<unsigned char>& ink)
{
  if (w < 1 || h < 1) return;
  const int win = std::max(15, (std::min(w, h) / 20) | 1);
  const int r = win / 2;
  const double k = 0.34;   // Sauvola sensitivity (0.2..0.5 typical)
  const double R = 128.0;  // dynamic range of the standard deviation

  // Integral images (double for the squared sum; int64 could overflow on huge
  // pages). Padded by one row/column so window sums need no boundary branches.
  const int W = w + 1, H = h + 1;
  std::vector<double> si((size_t)W * H, 0.0), si2((size_t)W * H, 0.0);
  for (int y = 0; y < h; ++y)
  {
    double rowSum = 0, rowSum2 = 0;
    for (int x = 0; x < w; ++x)
    {
      const double v = g[(size_t)y * w + x];
      rowSum += v;
      rowSum2 += v * v;
      si[(size_t)(y + 1) * W + (x + 1)] = si[(size_t)y * W + (x + 1)] + rowSum;
      si2[(size_t)(y + 1) * W + (x + 1)] = si2[(size_t)y * W + (x + 1)] + rowSum2;
    }
  }
  auto rectSum = [&](const std::vector<double>& s, int x0, int y0, int x1, int y1) {
    return s[(size_t)(y1 + 1) * W + (x1 + 1)] - s[(size_t)y0 * W + (x1 + 1)] -
           s[(size_t)(y1 + 1) * W + x0] + s[(size_t)y0 * W + x0];
  };

  ink.assign((size_t)w * h, 0);
  for (int y = 0; y < h; ++y)
  {
    const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
    for (int x = 0; x < w; ++x)
    {
      const int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
      const double n = (double)(x1 - x0 + 1) * (y1 - y0 + 1);
      const double sum = rectSum(si, x0, y0, x1, y1);
      const double sum2 = rectSum(si2, x0, y0, x1, y1);
      const double mean = sum / n;
      double var = sum2 / n - mean * mean;
      if (var < 0) var = 0;
      const double sd = std::sqrt(var);
      const double t = mean * (1.0 + k * (sd / R - 1.0));
      // Only trust the local rule when the window actually contains contrast;
      // otherwise defer to the global split.
      const int local = (sd < 8.0) ? globalThr : (int)t;
      ink[(size_t)y * w + x] = (g[(size_t)y * w + x] < local) ? 1 : 0;
    }
  }
}

// ---------------------------------------------------------------------------
// Stage 2: connected components
// ---------------------------------------------------------------------------
struct Blob
{
  Box box;
  int area = 0;      // ink pixel count
  int holes = 0;     // background holes fully enclosed by the blob
  int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;  // raw (pre-deskew) box
};

// Two-pass connected-component labelling with a union-find over provisional
// labels, then a per-label accumulation of bounding box, area and hole count.
// Holes are counted by labelling the *background* once: a background region that
// touches no image border and is enclosed by a single blob contributes one hole
// to that blob (Tesseract infers topology the same way from outline nesting).
static std::vector<Blob> FindBlobs(const std::vector<unsigned char>& ink, int w,
                                   int h, std::vector<int>& label)
{
  label.assign((size_t)w * h, 0);
  std::vector<int> parent(1, 0);
  auto find = [&](int a) {
    while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
    return a;
  };
  auto unite = [&](int a, int b) {
    a = find(a); b = find(b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  };

  int next = 1;
  for (int y = 0; y < h; ++y)
  {
    for (int x = 0; x < w; ++x)
    {
      if (!ink[(size_t)y * w + x]) continue;
      int left = (x > 0) ? label[(size_t)y * w + (x - 1)] : 0;
      int up = (y > 0) ? label[(size_t)(y - 1) * w + x] : 0;
      int ul = (x > 0 && y > 0) ? label[(size_t)(y - 1) * w + (x - 1)] : 0;
      int ur = (x + 1 < w && y > 0) ? label[(size_t)(y - 1) * w + (x + 1)] : 0;
      int lbl = 0;
      if (left) lbl = left;
      if (up && (!lbl || up < lbl)) lbl = up;
      if (ul && (!lbl || ul < lbl)) lbl = ul;
      if (ur && (!lbl || ur < lbl)) lbl = ur;
      if (!lbl) { lbl = next++; parent.push_back(lbl); }
      label[(size_t)y * w + x] = lbl;
      if (left) unite(lbl, left);
      if (up) unite(lbl, up);
      if (ul) unite(lbl, ul);
      if (ur) unite(lbl, ur);
    }
  }

  std::vector<Blob> blobs(next);
  std::vector<char> alive(next, 0);
  // Second pass: accumulate stats under the canonical label.
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      int l = label[(size_t)y * w + x];
      if (!l) continue;
      l = find(l);
      label[(size_t)y * w + x] = l;
      Blob& b = blobs[l];
      if (!alive[l]) { b.box = Box{x, y, x, y}; alive[l] = 1; }
      b.box.x0 = std::min(b.box.x0, x); b.box.x1 = std::max(b.box.x1, x);
      b.box.y0 = std::min(b.box.y0, y); b.box.y1 = std::max(b.box.y1, y);
      b.area++;
    }

  // Hole counting: label background regions; an enclosed region with exactly one
  // distinct bordering blob is a hole of that blob.
  std::vector<int> bl((size_t)w * h, 0);
  int bnext = 1;
  std::vector<int> bparent(1, 0);
  auto bfind = [&](int a) {
    while (bparent[a] != a) { bparent[a] = bparent[bparent[a]]; a = bparent[a]; }
    return a;
  };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      if (ink[(size_t)y * w + x]) continue;
      int up = (y > 0) ? bl[(size_t)(y - 1) * w + x] : 0;
      int left = (x > 0) ? bl[(size_t)y * w + (x - 1)] : 0;
      int lbl = up ? up : left;
      if (!lbl) { lbl = bnext++; bparent.push_back(lbl); }
      bl[(size_t)y * w + x] = lbl;
      if (up && left) { int a = bfind(up), c = bfind(left); if (a != c) bparent[std::max(a, c)] = std::min(a, c); }
    }
  // Which background regions touch the border (not holes), and each enclosed
  // region's sole adjacent blob.
  std::vector<char> border(bnext, 0);
  std::vector<int> adj(bnext, 0);
  std::vector<char> multi(bnext, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      if (!ink[(size_t)y * w + x]) continue;
      // Look at 4-neighbours that are background: record adjacency.
      const int nbr[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
      const int lbl = label[(size_t)y * w + x];
      for (auto& nn : nbr)
      {
        const int nx = nn[0], ny = nn[1];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        if (ink[(size_t)ny * w + nx]) continue;
        int br = bfind(bl[(size_t)ny * w + nx]);
        if (adj[br] == 0) adj[br] = lbl;
        else if (adj[br] != lbl) multi[br] = 1;
      }
    }
  for (int y = 0; y < h; ++y)
  {
    border[bfind(bl[(size_t)y * w + 0])] = 1;
    border[bfind(bl[(size_t)y * w + (w - 1)])] = 1;
  }
  for (int x = 0; x < w; ++x)
  {
    border[bfind(bl[(size_t)0 * w + x])] = 1;
    border[bfind(bl[(size_t)(h - 1) * w + x])] = 1;
  }
  for (int r = 1; r < bnext; ++r)
  {
    const int root = bfind(r);
    if (root != r) continue;
    if (border[r] || multi[r] || adj[r] == 0) continue;
    if (find(adj[r]) < (int)blobs.size()) blobs[adj[r]].holes++;
  }

  // Drop dead labels and return only live blobs.
  std::vector<Blob> out;
  for (int i = 1; i < next; ++i)
  {
    if (!alive[i] || find(i) != i) continue;
    out.push_back(blobs[i]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Stage 3: text-line assembly
// ---------------------------------------------------------------------------
// Greedy grouping: blobs are sorted top-to-bottom and each is attached to the
// line whose vertical extent it overlaps the most. A second merge pass joins
// lines whose baselines sit within half a text height, which keeps superscripts
// and dot-less fragments from forming their own line.
struct Line
{
  std::vector<int> blobs;  // indices into the blob array
  Box box;
};

static std::vector<Line> BuildLines(const std::vector<Blob>& blobs, int pageH)
{
  std::vector<int> order(blobs.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [&](int a, int b) { return blobs[a].box.cy() < blobs[b].box.cy(); });

  std::vector<Line> lines;
  for (int idx : order)
  {
    const Blob& b = blobs[idx];
    int best = -1;
    double bestOv = 0.35;  // require a third of the smaller height to join
    for (int li = 0; li < (int)lines.size(); ++li)
    {
      const double ov = VOverlap(b.box, lines[li].box);
      if (ov > bestOv) { bestOv = ov; best = li; }
    }
    if (best < 0)
    {
      Line L;
      L.blobs.push_back(idx);
      L.box = b.box;
      lines.push_back(L);
    }
    else
    {
      Line& L = lines[best];
      L.blobs.push_back(idx);
      L.box.x0 = std::min(L.box.x0, b.box.x0);
      L.box.x1 = std::max(L.box.x1, b.box.x1);
      L.box.y0 = std::min(L.box.y0, b.box.y0);
      L.box.y1 = std::max(L.box.y1, b.box.y1);
    }
  }

  // Merge near-duplicate lines (small overlap fragments).
  std::sort(lines.begin(), lines.end(),
            [](const Line& a, const Line& b) { return a.box.cy() < b.box.cy(); });
  std::vector<char> dead(lines.size(), 0);
  for (size_t i = 0; i < lines.size(); ++i)
  {
    if (dead[i]) continue;
    for (size_t j = i + 1; j < lines.size(); ++j)
    {
      if (dead[j]) continue;
      if (VOverlap(lines[i].box, lines[j].box) > 0.4)
      {
        for (int bi : lines[j].blobs) lines[i].blobs.push_back(bi);
        lines[i].box.x0 = std::min(lines[i].box.x0, lines[j].box.x0);
        lines[i].box.x1 = std::max(lines[i].box.x1, lines[j].box.x1);
        lines[i].box.y0 = std::min(lines[i].box.y0, lines[j].box.y0);
        lines[i].box.y1 = std::max(lines[i].box.y1, lines[j].box.y1);
        dead[j] = 1;
      }
    }
  }
  std::vector<Line> out;
  for (size_t i = 0; i < lines.size(); ++i)
    if (!dead[i] && !lines[i].blobs.empty()) out.push_back(lines[i]);
  (void)pageH;

  // Order blobs within each line left-to-right.
  for (Line& L : out)
    std::sort(L.blobs.begin(), L.blobs.end(),
              [&](int a, int b) { return blobs[a].box.cx() < blobs[b].box.cx(); });
  return out;
}

// ---------------------------------------------------------------------------
// Stage 5 helpers: glyph normalisation and prototype matching
// ---------------------------------------------------------------------------
constexpr int kGrid = 16;  // normalised glyph grid (kGrid x kGrid)

// Render one blob (with any small companions already merged into its box) into a
// kGrid x kGrid coverage bitmap. Coverage is a soft average so thin strokes and
// diagonal edges still register.
struct Glyph
{
  Box box;
  int holes = 0;
  unsigned char cell[kGrid * kGrid];
  int ink = 0;
};

static void Normalize(const std::vector<unsigned char>& ink, int w, int h,
                      const Box& box, Glyph& gl)
{
  gl.box = box;
  gl.ink = 0;
  for (int i = 0; i < kGrid * kGrid; ++i) gl.cell[i] = 0;
  const int bw = box.w(), bh = box.h();
  if (bw < 1 || bh < 1) return;
  for (int gy = 0; gy < kGrid; ++gy)
  {
    const int y0 = box.y0 + (int)std::floor((double)gy * bh / kGrid);
    const int y1 = box.y0 + (int)std::ceil((double)(gy + 1) * bh / kGrid);
    for (int gx = 0; gx < kGrid; ++gx)
    {
      const int x0 = box.x0 + (int)std::floor((double)gx * bw / kGrid);
      const int x1 = box.x0 + (int)std::ceil((double)(gx + 1) * bw / kGrid);
      int on = 0, tot = 0;
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
          if (x < 0 || y < 0 || x >= w || y >= h) continue;
          tot++;
          on += ink[(size_t)y * w + x] ? 1 : 0;
        }
      const int cov = tot ? (on * 255 / tot) : 0;
      gl.cell[gy * kGrid + gx] = (unsigned char)cov;
      if (cov > 96) gl.ink++;
    }
  }
}

// A prototype is a named reference glyph with a precomputed grid plus the
// features that disambiguate families the bitmap alone conflates.
struct Proto
{
  wchar_t ch;
  unsigned char cell[kGrid * kGrid];
  int holes;
  double aspect;  // width / height
};

// Two glyphs are compared with a weighted sum of pixel correlation and feature
// agreement. The bitmap dominates; topology and aspect break ties (o vs 0, l vs
// 1, i vs j, etc.).
static double Score(const Glyph& g, const Proto& p, double gAspect)
{
  int diff = 0, gInk = 0, pInk = 0;
  for (int i = 0; i < kGrid * kGrid; ++i)
  {
    const int a = g.cell[i], b = p.cell[i];
    diff += std::abs(a - b);
    if (a > 96) gInk++;
    if (b > 96) pInk++;
  }
  const double norm = 255.0 * kGrid * kGrid;
  double s = 1.0 - diff / norm;
  // Topology agreement.
  if (g.holes == p.holes) s += 0.06; else s -= 0.10;
  // Aspect ratio agreement.
  s -= std::min(0.12, std::abs(gAspect - p.aspect) * 0.5);
  // Penalise grossly mismatched ink mass (catches punctuation vs letters).
  const double mass = (double)std::abs(gInk - pInk) / std::max(1, std::max(gInk, pInk));
  s -= mass * 0.08;
  return s;
}

}  // namespace
}  // namespace ocr

// The recogniser needs reference shapes. Rather than embed hundreds of packed
// bitmaps by hand (error-prone and huge), the prototypes are rendered at run
// time from a monospace system font. On Windows that is GDI TextOut; the probe
// supplies its own. To keep ocr.cpp free of platform headers, the render hook is
// injected here.
namespace ocr {

// A caller-supplied function that renders one character to a kGrid x kGrid
// coverage grid. Returns false if the character could not be rendered.
using PrototypeRenderFn = bool (*)(wchar_t ch, unsigned char* outGrid);

static PrototypeRenderFn g_renderProto = nullptr;

void SetPrototypeRenderer(PrototypeRenderFn fn) { g_renderProto = fn; }

namespace {

// Build the prototype table once from the injected renderer. The character set
// covers ASCII letters, digits and common punctuation.
const std::vector<Proto>& Prototypes()
{
  static std::vector<Proto> protos;
  if (!protos.empty() || !g_renderProto) return protos;
  static const wchar_t kSet[] =
      L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
      L".,;:!?'\"()-+/\\%#&@*=$<>[]{}";
  for (const wchar_t* c = kSet; *c; ++c)
  {
    Proto p{};
    p.ch = *c;
    if (!g_renderProto(*c, p.cell)) continue;
    // Derive topology + aspect from the rendered grid.
    int gInk = 0;
    for (int i = 0; i < kGrid * kGrid; ++i) if (p.cell[i] > 96) gInk++;
    p.holes = 0;   // filled glyphs from a solid render keep no printed holes
    p.aspect = 0.6;
    protos.push_back(p);
  }
  return protos;
}

// Merge any blobs whose boxes vertically overlap a main glyph and sit close
// enough to be part of it (i-dots, umlaut marks, and glyphs split by a scanner).
static Box MergeCompanions(const std::vector<Blob>& blobs, int main, int lineY0,
                           int lineY1, int& holes)
{
  const Blob& m = blobs[main];
  Box b = m.box;
  holes = m.holes;
  const int maxGap = std::max(2, m.box.h() / 3);
  for (int j = 0; j < (int)blobs.size(); ++j)
  {
    if (j == main) continue;
    const Blob& o = blobs[j];
    if (o.box.h() > m.box.h()) continue;               // not a companion
    const int hgap = std::max(0, std::max(m.box.x0 - o.box.x1, o.box.x0 - m.box.x1));
    const int vgap = std::max(0, std::max(m.box.y0 - o.box.y1, o.box.y0 - m.box.y1));
    if (hgap > maxGap || vgap > maxGap) continue;
    // Must sit above or below (dot) rather than be a separate glyph on the line.
    const bool above = o.box.y1 <= m.box.y0 + 1;
    const bool below = o.box.y0 >= m.box.y1 - 1;
    if (!above && !below) continue;
    if (std::abs(o.box.cx() - m.box.cx()) > m.box.w()) continue;
    b.x0 = std::min(b.x0, o.box.x0); b.x1 = std::max(b.x1, o.box.x1);
    b.y0 = std::min(b.y0, o.box.y0); b.y1 = std::max(b.y1, o.box.y1);
    holes += o.holes;
  }
  (void)lineY0; (void)lineY1;
  return b;
}

wchar_t RecognizeGlyph(const std::vector<unsigned char>& ink, int w, int h,
                       const Box& box, int holes, double& outConf)
{
  const std::vector<Proto>& protos = Prototypes();
  if (protos.empty()) { outConf = 0; return L'?'; }
  Glyph g;
  Normalize(ink, w, h, box, g);
  g.holes = holes;
  const double aspect = box.h() > 0 ? (double)box.w() / box.h() : 1.0;
  double best = -1e9;
  wchar_t bestCh = L'?';
  for (const Proto& p : protos)
  {
    const double s = Score(g, p, aspect);
    if (s > best) { best = s; bestCh = p.ch; }
  }
  // Map the raw score into a coarse confidence. A perfect match scores near 1.0;
  // below ~0.45 the guess is unreliable.
  outConf = std::max(0.0, std::min(1.0, best));
  return bestCh;
}

}  // namespace

std::vector<Word> RecognizePage(const unsigned char* pixels, int w, int h)
{
  std::vector<Word> words;
  if (!pixels || w < 4 || h < 4) return words;
  std::vector<unsigned char> g(pixels, pixels + (size_t)w * h);
  const int globalThr = OtsuThreshold(g);
  std::vector<unsigned char> ink;
  BinarizeSauvola(g, w, h, globalThr, ink);

  std::vector<int> label;
  std::vector<Blob> blobs = FindBlobs(ink, w, h, label);

  // Discard specks: anything smaller than ~0.05% of the page or under 3px tall
  // is scanner noise, not text.
  const int minArea = std::max(4, (w * h) / 20000);
  std::vector<Blob> keep;
  for (Blob& b : blobs)
    if (b.area >= minArea && b.box.h() >= 3 && b.box.w() >= 1) keep.push_back(b);
  blobs.swap(keep);
  if (blobs.empty()) return words;

  // Estimate the page text height (median live blob height) so the fusion rules
  // below scale with the actual type size.
  int medTextH = 12;
  {
    std::vector<int> hs;
    for (const Blob& b : blobs) if (b.box.h() >= 4) hs.push_back(b.box.h());
    if (!hs.empty()) { std::sort(hs.begin(), hs.end()); medTextH = hs[hs.size() / 2]; }
  }

  // Fuse parts that belong to one glyph but are printed detached: i/j dots,
  // umlauts and the stacked pieces of : ; ! " ' = . A small piece is absorbed by
  // a larger one directly above or below it, and two small pieces stacked on
  // each other merge too (colon/exclamation/quote). Taller blobs are processed
  // first so a base glyph always absorbs its mark.
  {
    const int smallH = std::max(3, (int)(medTextH * 0.55));
    const int maxVGap = std::max(3, (int)(medTextH * 1.1));
    std::vector<int> byH(blobs.size());
    std::iota(byH.begin(), byH.end(), 0);
    std::sort(byH.begin(), byH.end(),
              [&](int a, int b) { return blobs[a].box.h() > blobs[b].box.h(); });
    std::vector<char> absorbed(blobs.size(), 0);
    for (int ii : byH)
    {
      const size_t i = (size_t)ii;
      if (absorbed[i]) continue;
      bool grew = true;
      while (grew)
      {
        grew = false;
        for (size_t j = 0; j < blobs.size(); ++j)
        {
          if (i == j || absorbed[j]) continue;
          const Blob& m = blobs[i];
          const Blob& o = blobs[j];
          const bool oSmall = o.box.h() <= smallH;
          const bool mSmall = m.box.h() <= smallH;
          if (!oSmall && o.box.h() > m.box.h()) continue;
          const bool above = o.box.y1 <= m.box.y0 + 2;
          const bool below = o.box.y0 >= m.box.y1 - 2;
          if (!above && !below) continue;
          const int vgap = above ? (m.box.y0 - o.box.y1) : (o.box.y0 - m.box.y1);
          if (vgap > maxVGap) continue;
          const int ovl = std::min(m.box.x1, o.box.x1) - std::max(m.box.x0, o.box.x0) + 1;
          const int minW = std::min(m.box.w(), o.box.w());
          if (ovl < (int)(minW * (mSmall && oSmall ? 0.4 : 0.3))) continue;
          blobs[i].box.x0 = std::min(blobs[i].box.x0, o.box.x0);
          blobs[i].box.x1 = std::max(blobs[i].box.x1, o.box.x1);
          blobs[i].box.y0 = std::min(blobs[i].box.y0, o.box.y0);
          blobs[i].box.y1 = std::max(blobs[i].box.y1, o.box.y1);
          blobs[i].holes += o.holes;
          absorbed[j] = 1;
          grew = true;
        }
      }
    }
    std::vector<Blob> fused;
    for (size_t i = 0; i < blobs.size(); ++i)
      if (!absorbed[i]) fused.push_back(blobs[i]);
    blobs.swap(fused);
  }
  if (blobs.empty()) return words;

  std::vector<Line> lines = BuildLines(blobs, h);

  int lineNo = 0;
  for (Line& L : lines)
  {
    if (L.blobs.empty()) continue;
    // Estimate the line's median glyph height and x-height band for space
    // detection: gaps are measured against a text-size-derived threshold, as
    // Tesseract measures spaces between baseline and mean line.
    std::vector<int> hs;
    for (int bi : L.blobs) hs.push_back(blobs[bi].box.h());
    std::sort(hs.begin(), hs.end());
    const int medH = hs[hs.size() / 2];
    // Median advance: the typical spacing between consecutive glyph left edges.
    // A gap counts as a word break only when it comfortably exceeds both the
    // text height and the intra-word kerning, so tall thin glyphs (i, l, 1) do
    // not punch holes in the middle of words.
    std::vector<int> adv;
    for (size_t k = 1; k < L.blobs.size(); ++k)
      adv.push_back(blobs[L.blobs[k]].box.x0 - blobs[L.blobs[k - 1]].box.x0);
    int medAdv = medH;
    if (!adv.empty())
    {
      std::sort(adv.begin(), adv.end());
      medAdv = std::max(1, adv[adv.size() / 2]);
    }
    // Median inter-glyph gap: the typical kerning within a word. A real word
    // break must clearly exceed it, otherwise punctuation that happens to sit a
    // little wide (e.g. ")!" ) would be torn off as its own word.
    std::vector<int> gaps;
    for (size_t k = 1; k < L.blobs.size(); ++k)
    {
      const int gg = blobs[L.blobs[k]].box.x0 - blobs[L.blobs[k - 1]].box.x1;
      if (gg > 0) gaps.push_back(gg);
    }
    int medGap = std::max(1, medH / 6);
    if (!gaps.empty())
    {
      std::sort(gaps.begin(), gaps.end());
      medGap = std::max(1, gaps[gaps.size() / 2]);
    }
    const int spaceGap = std::max(
        {3, (int)(medH * 0.45), (int)(medAdv * 0.6), (int)(medGap * 2.2)});

    std::wstring lineText;
    int prevRight = blobs[L.blobs[0]].box.x1;
    int curWordConf = 0, curWordN = 0;
    Box curWordBox = blobs[L.blobs[0]].box;

    for (size_t k = 0; k < L.blobs.size(); ++k)
    {
      const int bi = L.blobs[k];
      const Blob& b = blobs[bi];
      const int gap = b.box.x0 - prevRight;
      if (k > 0 && gap > spaceGap)
      {
        // Close the current word.
        if (!lineText.empty() && curWordN > 0)
        {
          Word wd;
          wd.text = lineText;
          wd.x = curWordBox.x0; wd.y = curWordBox.y0;
          wd.w = curWordBox.x1 - curWordBox.x0 + 1;
          wd.h = curWordBox.y1 - curWordBox.y0 + 1;
          wd.line = lineNo;
          wd.confidence = (double)curWordConf / std::max(1, curWordN * 255);
          words.push_back(wd);
        }
        lineText.clear();
        curWordConf = 0; curWordN = 0;
        curWordBox = b.box;
      }
      else if (k > 0)
      {
        curWordBox.x1 = std::max(curWordBox.x1, b.box.x1);
        curWordBox.y0 = std::min(curWordBox.y0, b.box.y0);
        curWordBox.y1 = std::max(curWordBox.y1, b.box.y1);
      }

      int holes = 0;
      const Box mb = MergeCompanions(blobs, bi, L.box.y0, L.box.y1, holes);
      double conf = 0;
      const wchar_t ch = RecognizeGlyph(ink, w, h, mb, holes, conf);
      lineText.push_back(ch);
      curWordConf += (int)(conf * 255);
      curWordN++;
      prevRight = b.box.x1;
    }
    // Flush the trailing word.
    if (!lineText.empty() && curWordN > 0)
    {
      Word wd;
      wd.text = lineText;
      wd.x = curWordBox.x0; wd.y = curWordBox.y0;
      wd.w = curWordBox.x1 - curWordBox.x0 + 1;
      wd.h = curWordBox.y1 - curWordBox.y0 + 1;
      wd.line = lineNo;
      wd.confidence = (double)curWordConf / std::max(1, curWordN * 255);
      words.push_back(wd);
    }
    lineNo++;
  }
  return words;
}

std::wstring ToPlainText(const std::vector<Word>& words)
{
  std::wstring out;
  int lastLine = -1;
  for (const Word& w : words)
  {
    if (w.line != lastLine)
    {
      if (lastLine != -1) out.push_back(L'\n');
      lastLine = w.line;
    }
    else
    {
      out.push_back(L' ');
    }
    out += w.text;
  }
  return out;
}

}  // namespace ocr
