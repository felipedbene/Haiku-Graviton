// Host unit test for UI/Haiku/DamageRefinement.h (#622) plus a model of the [C]/[W] present protocol.
// Oracle: exact pixel values and exact rects; the protocol model's oracle is "the screen equals the last frame",
// which does not come from the code under test.
#include "DamageRefinement.h" // extracted from ../ladybird-622.patch by run.sh

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace Ladybird::DamageRefinement;

static int failures = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++failures;                                      \
            printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

struct Frame {
    int w, h;
    size_t pitch; // deliberately != w*4, like BBitmap padding
    std::vector<uint8_t> px;
    Frame(int w_, int h_, size_t pad = 12) : w(w_), h(h_), pitch(size_t(w_) * 4 + pad), px(pitch * h_, 0xEE) { }
    uint32_t get(int x, int y) const { uint32_t v; memcpy(&v, &px[y * pitch + x * 4], 4); return v; }
    void set(int x, int y, uint32_t v) { memcpy(&px[y * pitch + x * 4], &v, 4); }
    uint8_t* data() { return px.data(); }
    uint8_t const* data() const { return px.data(); }
};

static bool same_pixels(Frame const& a, Frame const& b, int* bx = nullptr, int* by = nullptr)
{
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            if (a.get(x, y) != b.get(x, y)) {
                if (bx) { *bx = x; *by = y; }
                return false;
            }
    return true;
}

// Page content: every row distinct and asymmetric left-to-right (CLAUDE.md: no symmetric fixtures).
static uint32_t page_pixel(int x, int page_y) { return 0xff000000u | (uint32_t(page_y * 2654435761u) & 0xffff00u) | uint32_t((x * 7 + page_y) & 0xff); }
static void paint_page(Frame& f, int scroll, int header_rows = 0)
{
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x)
            f.set(x, y, y < header_rows ? (0xff112200u | uint32_t(x & 0xff)) : page_pixel(x, y + scroll));
}

static bool rect_eq(Rect r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.width == w && r.height == h; }

// An overlay scrollbar thumb, as Ladybird paints it: a few columns at the right edge that move with the scroll.
static void paint_thumb(Frame& f, int top, int rows = 50, int width = 8)
{
    for (int y = std::max(0, top); y < std::min(f.h, top + rows); ++y)
        for (int x = f.w - 1 - width; x < f.w - 1; ++x) f.set(x, y, 0xff445566u ^ uint32_t(y - top));
}

static void test_residual_thumb()
{
    int const W = 1024, H = 663;
    Frame old_f(W, H), new_f(W, H);
    paint_page(old_f, 0); paint_thumb(old_f, 10);
    paint_page(new_f, 120); paint_thumb(new_f, 20);
    std::vector<uint64_t> scratch(2 * H);
    VerticalShift s = find_vertical_shift(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, W, H, scratch.data(), 32);
    CHECK(s.dy == 120 && s.top == 70 && s.bottom == 543, "thumb shift dy=%d [%d,%d) want 120 [70,543)", s.dy, s.top, s.bottom);
    Rect r[8]; long area = 0;
    int n = residual_rects(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, W, H, 120, 0, 543, r, 8, &area);
    CHECK(n == 2 && rect_eq(r[0], W - 9, 20, 8, 50) && rect_eq(r[1], 0, 543, W, 120),
        "overlap residual n=%d r0=%d,%d %dx%d r1=%d,%d %dx%d", n, r[0].x, r[0].y, r[0].width, r[0].height, r[1].x, r[1].y, r[1].width, r[1].height);
    CHECK(area == 8L * 50 + long(W) * 120, "overlap area %ld", area);
    long exact_area = 0;
    int m = residual_rects(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, W, H, 120, s.top, s.bottom, r, 8, &exact_area);
    CHECK(m == 2 && rect_eq(r[0], 0, 0, W, 70) && exact_area == long(W) * 70 + long(W) * 120, "exact residual n=%d area %ld", m, exact_area);
    // no blit at all: the residual of an unchanged frame is empty, of a one-pixel change exactly that pixel
    n = residual_rects(old_f.data(), old_f.pitch, old_f.data(), old_f.pitch, W, H, 0, 0, 0, r, 8, &area);
    CHECK(n == 0 && area == 0, "identical residual n=%d", n);
    Frame g = old_f; g.set(1023, 662, 0x01010101);
    n = residual_rects(old_f.data(), old_f.pitch, g.data(), g.pitch, W, H, 0, 0, 0, r, 8, &area);
    CHECK(n == 1 && rect_eq(r[0], 1023, 662, 1, 1), "corner residual");
    // overflow: more separate runs than slots collapse into the last rect, still covering everything
    Frame h2 = old_f;
    for (int k = 0; k < 12; ++k) h2.set(5 + k, 10 + 3 * k, 0x02020202);
    n = residual_rects(old_f.data(), old_f.pitch, h2.data(), h2.pitch, W, H, 0, 0, 0, r, 4, &area);
    CHECK(n == 4 && r[3].y == 19 && r[3].y + r[3].height == 44 && r[3].x == 8 && r[3].x + r[3].width == 17, "overflow last=%d,%d %dx%d", r[3].x, r[3].y, r[3].width, r[3].height);
}

static void test_copy_changed()
{
    Frame old_f(1024, 663), new_f(1024, 663);
    paint_page(old_f, 0); paint_page(new_f, 0);
    // a "glyph": 7x12 at (129,186), full-viewport damage reported
    for (int y = 186; y < 198; ++y)
        for (int x = 129; x < 136; ++x)
            new_f.set(x, y, 0xff000000u | uint32_t(x * y));
    Rect r = copy_changed(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, { 0, 0, 1024, 663 });
    CHECK(rect_eq(r, 129, 186, 7, 12), "got %d,%d %dx%d", r.x, r.y, r.width, r.height);
    CHECK(same_pixels(old_f, new_f), "destination not updated");
    // second identical present: nothing changed
    r = copy_changed(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, { 0, 0, 1024, 663 });
    CHECK(r.is_empty(), "identical frame reported %dx%d", r.width, r.height);
    // single pixel at the extreme corners (column/row boundary off-by-ones)
    new_f.set(1023, 662, 0x01020304);
    r = copy_changed(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, { 0, 0, 1024, 663 });
    CHECK(rect_eq(r, 1023, 662, 1, 1), "corner got %d,%d %dx%d", r.x, r.y, r.width, r.height);
    new_f.set(0, 0, 0x01020305);
    r = copy_changed(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, { 0, 0, 1024, 663 });
    CHECK(rect_eq(r, 0, 0, 1, 1), "origin got %d,%d %dx%d", r.x, r.y, r.width, r.height);
    // partial-row damage: a change outside the reported rect is NOT copied (we trust the reported rect's bounds)
    new_f.set(500, 300, 0x0a0b0c0d); // inside
    new_f.set(10, 300, 0x0a0b0c0e);  // outside
    r = copy_changed(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, { 400, 290, 200, 20 });
    CHECK(rect_eq(r, 500, 300, 1, 1), "partial got %d,%d %dx%d", r.x, r.y, r.width, r.height);
    CHECK(old_f.get(10, 300) != 0x0a0b0c0eu, "copied outside the rect");
    // alpha-only change is reported (conservative)
    new_f.set(600, 400, new_f.get(600, 400) ^ 0xff000000u);
    r = copy_changed(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, { 0, 0, 1024, 663 });
    CHECK(rect_eq(r, 10, 300, 591, 101), "alpha got %d,%d %dx%d", r.x, r.y, r.width, r.height);
    // padding bytes past the row are never touched
    CHECK(old_f.px[0 * old_f.pitch + 1024 * 4] == 0xEE, "padding written");
}

static void test_shift_basic(int dy, int header)
{
    int const W = 1024, H = 663;
    Frame old_f(W, H), new_f(W, H);
    paint_page(old_f, 1000, header);
    paint_page(new_f, 1000 + dy, header);
    std::vector<uint64_t> scratch(2 * H);
    VerticalShift s = find_vertical_shift(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, W, H, scratch.data(), 32);
    int exp_top = dy > 0 ? header : header - dy; // for dy<0 rows [header, header-dy) are exposed
    int exp_bottom = dy > 0 ? H - dy : H;
    if (dy > 0 && header > 0) exp_top = header;
    CHECK(s.dy == dy && s.top == exp_top && s.bottom == exp_bottom, "dy=%d header=%d: got dy=%d [%d,%d) want [%d,%d)", dy, header, s.dy, s.top, s.bottom, exp_top, exp_bottom);
    Rect r = copy_changed(old_f.data(), old_f.pitch, new_f.data(), new_f.pitch, { 0, 0, W, H }, s.top, s.bottom);
    int ey = dy > 0 ? H - dy : header, eh = dy > 0 ? dy : -dy;
    CHECK(rect_eq(r, 0, ey, W, eh), "dy=%d header=%d: damage %d,%d %dx%d want 0,%d %dx%d", dy, header, r.x, r.y, r.width, r.height, ey, W, eh);
}

static void test_shift_negative()
{
    int const W = 64, H = 200;
    std::vector<uint64_t> scratch(2 * H);
    // blank page: no shift may be claimed, and none needed
    Frame a(W, H), b(W, H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { a.set(x, y, 0xffffffff); b.set(x, y, 0xffffffff); }
    VerticalShift s = find_vertical_shift(a.data(), a.pitch, b.data(), b.pitch, W, H, scratch.data(), 8);
    CHECK(!s.is_valid(), "blank page shift dy=%d", s.dy);
    // unrelated pages: no verified band
    paint_page(a, 0); for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) b.set(x, y, 0xff000000u | uint32_t(rand()));
    s = find_vertical_shift(a.data(), a.pitch, b.data(), b.pitch, W, H, scratch.data(), 8);
    CHECK(!s.is_valid(), "random page shift dy=%d", s.dy);
    // forced hash collision cannot produce a band: content that is equal in hash only is impossible to build, so
    // instead check that a band is cut exactly at a one-pixel difference inside it
    paint_page(a, 0); paint_page(b, 30);
    b.set(5, 100, 0x12345678);
    s = find_vertical_shift(a.data(), a.pitch, b.data(), b.pitch, W, H, scratch.data(), 8);
    CHECK(s.dy == 30 && s.top == 0 && s.bottom == 100, "cut band: dy=%d [%d,%d)", s.dy, s.top, s.bottom);
    Rect r = copy_changed(a.data(), a.pitch, b.data(), b.pitch, { 0, 0, W, H }, s.top, s.bottom);
    CHECK(rect_eq(r, 0, 100, W, 100), "cut band damage %d,%d %dx%d", r.x, r.y, r.width, r.height);
}

// ---- Protocol model: [C] present_front_bitmap, [W] kMsgFrameReady + Draw, app_server CopyContents -------------------
struct Model {
    int W, H, min_rows;
    Frame bitmap, screen;
    std::vector<uint8_t> dirty; // app_server dirty region, per pixel
    // PresentSurface
    bool have_damage = false; Rect damage {};
    bool scroll_pending = false; int dy = 0, top = 0, bottom = 0;
    bool have_painted = false; Rect painted {};
    std::vector<uint64_t> scratch;
    bool mutate_no_stale = false, mutate_no_seed = false;

    Model(int w, int h, int m) : W(w), H(h), min_rows(m), bitmap(w, h), screen(w, h), dirty(size_t(w) * h, 0), scratch(2 * h) { }

    static Rect unite(bool have, Rect a, Rect b)
    {
        if (!have || a.is_empty()) return b;
        if (b.is_empty()) return a;
        int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
        int x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
        return { x0, y0, x1 - x0, y1 - y0 };
    }
    void invalidate(Rect r)
    {
        for (int y = std::max(0, r.y); y < std::min(H, r.y + r.height); ++y)
            for (int x = std::max(0, r.x); x < std::min(W, r.x + r.width); ++x) dirty[y * W + x] = 1;
    }
    static constexpr int kMax = 8;
    Rect rects[kMax]; int rect_count = 0; // -1: overflowed, use damage
    bool mutate_residual_ignores_band = false;
    void add_damage(Rect r)
    {
        if (r.is_empty()) return;
        damage = unite(have_damage, damage, r); have_damage = true;
        if (rect_count >= 0 && rect_count < kMax) rects[rect_count++] = r; else rect_count = -1;
    }
    void present(Frame const& f, Rect reported)
    {
        VerticalShift s {};
        bool full = reported.x == 0 && reported.y == 0 && reported.width == W && reported.height == H;
        if (full && !scroll_pending && H > 2 * min_rows)
            s = find_vertical_shift(bitmap.data(), bitmap.pitch, f.data(), f.pitch, W, H, scratch.data(), min_rows);
        if (s.is_valid()) {
            int ot = s.dy < 0 ? -s.dy : 0, ob = s.dy > 0 ? H - s.dy : H;
            Rect ex[kMax], ov[kMax]; long ea = 0, oa = 0;
            int en = residual_rects(bitmap.data(), bitmap.pitch, f.data(), f.pitch, W, H, s.dy, s.top, s.bottom, ex, kMax, &ea);
            int on = mutate_residual_ignores_band
                ? residual_rects(bitmap.data(), bitmap.pitch, f.data(), f.pitch, W, H, s.dy, ot, ob, ov, kMax, &oa) * 0 + residual_rects(bitmap.data(), bitmap.pitch, f.data(), f.pitch, W, H, s.dy, 0, 0, ov, kMax, &oa) * 0
                : residual_rects(bitmap.data(), bitmap.pitch, f.data(), f.pitch, W, H, s.dy, ot, ob, ov, kMax, &oa);
            if (mutate_residual_ignores_band) { on = 1; ov[0] = { 0, ob, W, H - ob }; if (s.dy < 0) ov[0] = { 0, 0, W, ot }; oa = 0; }
            bool use_ov = oa < ea;
            have_painted = have_damage && !mutate_no_seed; painted = damage;
            scroll_pending = true; dy = s.dy; top = use_ov ? ot : s.top; bottom = use_ov ? ob : s.bottom;
            for (int y = 0; y < H; ++y) memcpy(&bitmap.px[y * bitmap.pitch], &f.px[y * f.pitch], size_t(W) * 4);
            Rect* rr = use_ov ? ov : ex;
            for (int i = 0, n = use_ov ? on : en; i < n; ++i) add_damage(rr[i]);
        } else {
            Rect c = copy_changed(bitmap.data(), bitmap.pitch, f.data(), f.pitch, reported);
            add_damage(c);
        }
    }
    void frame_ready()
    {
        Rect d = damage; bool hd = have_damage; have_damage = false;
        int rc = rect_count; Rect rs[kMax]; for (int i = 0; i < rc; ++i) rs[i] = rects[i]; rect_count = 0;
        if (scroll_pending) {
            scroll_pending = false;
            // app_server CopyContents: dirty source pixels are not copied but re-dirtied at the destination
            Frame before = screen; auto dirty_before = dirty;
            for (int y = top; y < bottom; ++y)
                for (int x = 0; x < W; ++x) {
                    int sy = y + dy;
                    if (dirty_before[sy * W + x]) dirty[y * W + x] = 1;
                    else screen.set(x, y, before.get(x, sy));
                }
            if (have_painted && !mutate_no_stale) {
                Rect m { painted.x, painted.y - dy, painted.width, painted.height };
                int y0 = std::max(m.y, top), y1 = std::min(m.y + m.height, bottom);
                if (y1 > y0) invalidate({ m.x, y0, m.width, y1 - y0 });
            }
            have_painted = false;
        }
        if (rc > 0) { for (int i = 0; i < rc; ++i) invalidate(rs[i]); }
        else if (hd) invalidate(d);
    }
    void draw(Rect r) // an update (or expose) of r; paints from the bitmap, records while a blit is pending
    {
        for (int y = std::max(0, r.y); y < std::min(H, r.y + r.height); ++y)
            for (int x = std::max(0, r.x); x < std::min(W, r.x + r.width); ++x) { screen.set(x, y, bitmap.get(x, y)); dirty[y * W + x] = 0; }
        if (scroll_pending) { painted = unite(have_painted, painted, r); have_painted = true; }
    }
    void draw_dirty()
    {
        int x0 = W, y0 = H, x1 = -1, y1 = -1;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) if (dirty[y * W + x]) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
        if (x1 >= 0) draw({ x0, y0, x1 - x0 + 1, y1 - y0 + 1 });
    }
    long settle_and_bytes()
    {
        frame_ready();
        long n = 0; for (auto d : dirty) n += d;
        draw_dirty();
        return n;
    }
};

static int run_protocol_fuzz(unsigned seed, int iterations, bool no_stale, bool no_seed, bool quiet, bool ignore_band = false)
{
    std::mt19937 rng(seed);
    int const W = 48, H = 160;
    int bad = 0;
    for (int it = 0; it < iterations; ++it) {
        Model m(W, H, 8);
        m.mutate_no_stale = no_stale; m.mutate_no_seed = no_seed; m.mutate_residual_ignores_band = ignore_band;
        int scroll = 500, header = int(rng() % 3) * 10;
        bool thumb = rng() % 2;
        auto thumb_top = [&](int sc) { return header + (sc % 400) / 4; };
        Frame f(W, H); paint_page(f, scroll, header); if (thumb) paint_thumb(f, thumb_top(scroll), 20, 3);
        m.present(f, { 0, 0, W, H }); // first frame: bitmap was 0xEE, screen 0xEE
        m.settle_and_bytes();
        for (int step = 0; step < 12; ++step) {
            int op = int(rng() % 4);
            if (op <= 1) {
                // new frame: scroll (maybe clamped) and/or an in-place edit, reported as full or partial damage
                int d = int(rng() % 5) == 0 ? 0 : int(rng() % 81) - 40;
                scroll += d;
                paint_page(f, scroll, header); if (thumb) paint_thumb(f, thumb_top(scroll), 20, 3);
                int ex = int(rng() % W), ey = int(rng() % H);
                if (rng() % 2) for (int y = ey; y < std::min(H, ey + 6); ++y) for (int x = ex; x < std::min(W, ex + 5); ++x) f.set(x, y, 0xff00ff00u ^ uint32_t(step * 77 + x));
                Rect rep = (d != 0 || rng() % 2) ? Rect { 0, 0, W, H } : Rect { std::max(0, ex - 2), std::max(0, ey - 2), std::min(W - std::max(0, ex - 2), 10), std::min(H - std::max(0, ey - 2), 10) };
                if (d == 0 && rep.width != W) {
                    // a partial report must cover what changed (Ladybird's contract); repaint the base page unchanged
                    paint_page(f, scroll, header); if (thumb) paint_thumb(f, thumb_top(scroll), 20, 3);
                    for (int y = rep.y + 1; y < rep.y + rep.height - 1; ++y) for (int x = rep.x + 1; x < rep.x + rep.width - 1; ++x) f.set(x, y, 0xff0000ffu ^ uint32_t(step));
                    // and the bitmap outside rep must already equal f: only true if f matches the bitmap there
                    bool ok = true;
                    for (int y = 0; y < H && ok; ++y) for (int x = 0; x < W; ++x) {
                        bool in = x >= rep.x && x < rep.x + rep.width && y >= rep.y && y < rep.y + rep.height;
                        if (!in && f.get(x, y) != m.bitmap.get(x, y)) { ok = false; break; }
                    }
                    if (!ok) rep = { 0, 0, W, H };
                }
                m.present(f, rep);
            } else if (op == 2) {
                m.frame_ready();
            } else {
                // an Update or an expose at an arbitrary time
                if (rng() % 2) m.draw_dirty();
                else { int y = int(rng() % H), x = int(rng() % W); m.draw({ x, y, int(rng() % 20) + 1, int(rng() % 40) + 1 }); }
            }
        }
        m.settle_and_bytes();
        m.draw_dirty();
        int bx = 0, by = 0;
        if (!same_pixels(m.screen, f, &bx, &by)) {
            ++bad;
            if (!quiet && bad <= 3) printf("  protocol seed %u iter %d: screen differs at (%d,%d) %08x vs %08x\n", seed, it, bx, by, m.screen.get(bx, by), f.get(bx, by));
        }
    }
    return bad;
}

static void test_protocol_bytes()
{
    // A clean 120 px notch on a 1024x663 view: the window repaints exactly the exposed strip.
    int const W = 1024, H = 663;
    Model m(W, H, 32);
    Frame f(W, H); paint_page(f, 0);
    m.present(f, { 0, 0, W, H }); m.settle_and_bytes();
    paint_page(f, 120);
    m.present(f, { 0, 0, W, H });
    CHECK(m.scroll_pending && m.dy == 120 && m.top == 0 && m.bottom == 543, "notch: pending=%d dy=%d [%d,%d)", m.scroll_pending, m.dy, m.top, m.bottom);
    long painted = m.settle_and_bytes();
    CHECK(painted == long(W) * 120, "notch repainted %ld pixels, want %ld", painted, long(W) * 120);
    CHECK(same_pixels(m.screen, f), "notch screen wrong");
    // A keystroke under full-viewport damage: exactly the glyph.
    for (int y = 186; y < 212; ++y) for (int x = 129; x < 140; ++x) f.set(x, y, 0xff000000u | uint32_t(y));
    m.present(f, { 0, 0, W, H });
    CHECK(!m.scroll_pending, "keystroke claimed a scroll");
    painted = m.settle_and_bytes();
    CHECK(painted == 11 * 26, "keystroke repainted %ld pixels, want %d", painted, 11 * 26);
    CHECK(same_pixels(m.screen, f), "keystroke screen wrong");
    // A notch with an overlay scrollbar thumb (Ladybird's): blit through it, repaint the thumb and the strip.
    Model t(W, H, 32);
    Frame g(W, H); paint_page(g, 0); paint_thumb(g, 10);
    t.present(g, { 0, 0, W, H }); t.settle_and_bytes();
    paint_page(g, 120); paint_thumb(g, 20);
    t.present(g, { 0, 0, W, H });
    CHECK(t.scroll_pending && t.top == 0 && t.bottom == 543, "thumb notch band [%d,%d)", t.top, t.bottom);
    painted = t.settle_and_bytes();
    CHECK(painted == 8L * 50 + long(W) * 120, "thumb notch repainted %ld pixels, want %ld", painted, 8L * 50 + long(W) * 120);
    CHECK(same_pixels(t.screen, g), "thumb notch screen wrong");
}

int main(int argc, char**)
{
    test_copy_changed();
    test_shift_basic(120, 0);
    test_shift_basic(-120, 0);
    test_shift_basic(37, 0);   // clamped at the end of the page
    test_shift_basic(120, 50); // fixed header that does not scroll
    test_shift_basic(-120, 50);
    test_shift_negative();
    test_residual_thumb();
    test_protocol_bytes();
    int bad = run_protocol_fuzz(1, 400, false, false, false);
    CHECK(bad == 0, "protocol model: %d/400 runs left a wrong pixel", bad);
    // Mutation arms: the stale-paint replay and the pending-damage seed must each be necessary.
    if (argc > 1) {
        int a = run_protocol_fuzz(1, 400, true, false, true);
        int b = run_protocol_fuzz(1, 400, false, true, true);
        int c = run_protocol_fuzz(1, 400, false, false, true, true);
        printf("mutation no-stale-replay: %d/400 wrong; no-pending-damage-seed: %d/400 wrong; residual-ignores-band: %d/400 wrong\n", a, b, c);
        if (a == 0 || b == 0 || c == 0) { printf("FAIL: a protocol mutation survived\n"); ++failures; }
    }
    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
