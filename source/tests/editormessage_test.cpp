// The editor's own message panel (2026-11-21): wrapping, pages, geometry, which keys close it, and the open/close state.
#include "editormessage.h"
#include <cmath>
#include <cstdio>
#include <sstream>
using namespace editormessage;
static std::string Words(const std::string& s) { std::istringstream in(s); std::string w, o; while (in >> w) o += (o.empty() ? "" : " ") + w; return o; }
static std::string JoinPages(const std::vector<std::vector<std::string>>& pages) { std::string o; for (const auto& p : pages) for (const auto& l : p) o += l + " "; return Words(o); }
int main() {
    int total = 0, failed = 0;
    auto check = [&](bool good, const char* name) { ++total; if (!good) { ++failed; std::printf("FAIL %s\n", name); } };

    // Wrapping.
    auto w = Wrap("aaa bbb ccc", 7); check(w.size() == 2 && w[0] == "aaa bbb" && w[1] == "ccc", "wrap at word");
    w = Wrap("one\n\ntwo", 20); check(w.size() == 3 && w[0] == "one" && w[1].empty() && w[2] == "two", "paragraph gap kept");
    w = Wrap("abcdefghijklmnopqrstuvwxyz", 10); check(w.size() == 3 && w[0] == "abcdefghij" && w[2] == "uvwxyz", "long word cut");
    w = Wrap("", 10); check(w.size() == 1 && w[0].empty(), "empty text is one empty line");
    w = Wrap("  spaced   out  ", 30); check(w.size() == 1 && w[0] == "spaced out", "spaces squeezed");
    for (int per : { 8, 20, 40, 64, 80 }) {
        bool fits = true, same = true;
        for (const char* t : { kFirstBody, kReadmeBody }) {
            const auto lines = Wrap(t, per);
            for (const auto& l : lines) if (int(l.size()) > per) fits = false;
            std::string joined; for (const auto& l : lines) joined += l + " ";
            if (per >= 12 && Words(joined) != Words(t)) same = false;  // below 12 a long word is cut, so words differ by design
        }
        check(fits, "no line longer than the width"); check(same, "no word lost");
    }

    // Pagination.
    std::vector<std::string> lines; for (int i = 0; i < 25; ++i) lines.push_back(i % 7 == 6 ? "" : "line" + std::to_string(i));
    auto pages = Paginate(lines, 10);
    check(pages.size() >= 3, "25 lines make 3 pages or more");
    bool edges = true, size = true; for (const auto& p : pages) { if (p.empty() || p.front().empty() || p.back().empty()) edges = false; if (p.size() > 10) size = false; }
    check(edges, "no page starts or ends empty"); check(size, "page size respected");
    check(Paginate({}, 5).size() == 1 && Paginate({}, 5)[0].empty(), "no text: one empty page");
    check(Paginate({ "", "" }, 5).size() == 1, "only blanks: one empty page");
    {   // all text survives pagination, at the real panel sizes
        bool all = true;
        for (double sw : { 800.0, 1280.0, 1920.0 }) for (double sh : { 480.0, 720.0, 1080.0 }) {
            const auto g = Layout(sw, sh, 0);
            for (const char* t : { kFirstBody, kReadmeBody }) {
                const auto pg = Paginate(Wrap(t, g.perLine), g.perPage);
                if (JoinPages(pg) != Words(t)) all = false;
                for (const auto& p : pg) if (int(p.size()) > g.perPage) all = false;
            }
        }
        check(all, "both texts complete at every screen size");
    }

    // Geometry: centred, inside the screen, buttons inside the panel, the text fits the panel.
    {
        bool ok = true;
        for (double sw : { 800.0, 1280.0, 1920.0 }) for (double sh : { 480.0, 720.0, 1080.0 }) for (int n : { 1, 5, 99 }) {
            const auto g = Layout(sw, sh, n);
            if (g.panel.x < 0 || g.panel.y < 0 || g.panel.x + g.panel.w > sw + 0.01 || g.panel.y + g.panel.h > sh + 0.01) ok = false;
            if (std::abs((g.panel.x + g.panel.w / 2) - sw / 2) > 0.51 || std::abs((g.panel.y + g.panel.h / 2) - sh / 2) > 0.51) ok = false;
            for (const Rect* r : { &g.ok, &g.prev, &g.next }) if (r->x < g.panel.x || r->y < g.panel.y || r->x + r->w > g.panel.x + g.panel.w || r->y + r->h > g.panel.y + g.panel.h) ok = false;
            if (g.ok.x < g.next.x + g.next.w) ok = false;  // OK never overlaps the page buttons
            if (g.perLine * kLetterPx > g.textW + 0.01) ok = false;
        }
        check(ok, "geometry fits every screen");
    }

    // Keys: which close, which do not.
    auto opened = [&] { Panel p; p.Show("T", kFirstBody, 40, 8); return p; };
    for (int k = 0; k < 7; ++k) {
        Panel p = opened(); Input in;
        switch (k) { case 0: in.e = true; break; case 1: in.enter = true; break; case 2: in.space = true; break; case 3: in.escape = true; break;
                     case 4: in.padAccept = true; break; case 5: in.okClicked = true; break; default: in.padBack = true; }
        const auto r = p.Update(in);
        check(r.closed && r.swallowed && !p.IsOpen(), "each close key closes");
    }
    {   Panel p = opened(); const auto r = p.Update(Input{}); check(r.swallowed && !r.closed && p.IsOpen(), "idle or any other key: still open, swallowed, not closed"); }
    {   Panel p; Input in; in.e = true; const auto r = p.Update(in); check(!r.swallowed && !r.closed, "closed panel: swallows nothing, closes nothing"); }
    {   // exactly once
        Panel p = opened(); Input in; in.e = true; in.enter = true; in.okClicked = true;
        int closes = 0; for (int i = 0; i < 5; ++i) if (p.Update(in).closed) ++closes;
        check(closes == 1 && p.Closed() == 1 && p.Opened() == 1, "several keys, several frames: closes exactly once");
    }
    {   // pages
        Panel p; p.Show("T", kReadmeBody, 20, 6); const int n = p.PageCount();
        check(n >= 3, "narrow readme has several pages");
        Input next; next.nextPage = true; Input prev; prev.prevPage = true;
        check(p.Update(prev).paged == false && p.Page() == 0, "cannot go before page 1");
        for (int i = 0; i < n + 3; ++i) p.Update(next);
        check(p.Page() == n - 1 && p.IsOpen(), "cannot go past the last page; paging never closes");
        check(p.Update(prev).paged && p.Page() == n - 2, "back one page");
        Input both; both.nextPage = true; both.e = true; check(p.Update(both).closed, "close wins over a page key");
        Panel one = opened(); one.Show("T", "short", 40, 8); check(one.PageCount() == 1, "short text is one page");
    }
    {   // rewrap keeps the page in range
        Panel p; p.Show("T", kReadmeBody, 20, 6); Input next; next.nextPage = true; for (int i = 0; i < 10; ++i) p.Update(next);
        p.Rewrap(80, 30); check(p.Page() < p.PageCount() && p.Page() >= 0, "rewrap clamps the page");
    }

    // State machine: once per trigger, reopen through Readme, reset on world change / editor close.
    {
        Panel p; FirstTimer t; int opens = 0, pressed = 0;
        for (int f = 0; f < 200; ++f) if (t.Tick(true, true)) { p.Show(kFirstTitle, kFirstBody, 40, 8); ++opens; }
        check(opens == 1 && p.IsOpen() && p.Opened() == 1, "first-time message opens once per editor opening");
        Input in; in.e = true; p.Update(in); (void)pressed;
        for (int f = 0; f < 200; ++f) if (t.Tick(true, true)) ++opens;
        check(opens == 1, "still once after it was closed");
        t.Tick(false, true);  // editor closed
        for (int f = 0; f < 100; ++f) if (t.Tick(true, true)) ++opens;
        check(opens == 2, "a new editor opening asks again (the seen flag decides in routeedit.cpp)");
        FirstTimer u; int fired = 0; for (int f = 0; f < 100; ++f) if (u.Tick(true, f % 2 == 0)) ++fired;
        check(fired == 1, "unfocused frames do not count, still once");
        FirstTimer v; int early = 0; for (int f = 0; f < kFirstDelayFrames - 1; ++f) if (v.Tick(true, true)) ++early;
        check(early == 0 && v.Tick(true, true), "fires on frame 30 exactly");
        FirstTimer off; for (int f = 0; f < 29; ++f) off.Tick(true, true); off.Tick(false, true);
        int again = 0; for (int f = 0; f < 29; ++f) if (off.Tick(true, true)) ++again; check(again == 0 && off.Tick(true, true), "editor off restarts the count");
    }
    {
        Panel p; p.Show(kReadmeTitle, kReadmeBody, 40, 8);  // the Readme button
        check(p.IsOpen() && p.Opened() == 1, "readme opens");
        p.Show(kReadmeTitle, kReadmeBody, 40, 8); check(p.Opened() == 1 && p.Page() == 0, "pressing Readme while open is still one open panel");
        check(p.Reset() && !p.IsOpen(), "reset (world change / editor closed) closes it");
        check(!p.Reset(), "reset on a closed panel does nothing");
        check(!p.Update(Input{}).swallowed, "after reset nothing is swallowed: input is not left blocked");
        p.Show(kFirstTitle, kFirstBody, 40, 8); check(p.IsOpen() && p.Opened() == 2 && p.Closed() == 0, "reopen after reset");
        Input in; in.padAccept = true; check(p.Update(in).closed && p.Closed() == 1, "reopened panel closes once again");
    }
    // Same words as the native box had.
    check(std::string(kFirstTitle) == "Traffic Manager" && std::string(kReadmeTitle) == "Traffic Manager - Readme", "titles unchanged");
    check(std::string(kFirstBody).rfind("You can manage traffic routes for this planet from photo mode.", 0) == 0 && std::string(kReadmeBody).rfind("Press F8 to open the Traffic Manager", 0) == 0, "texts unchanged");

    std::printf("editormessage_test: %d checks, %d failed\n", total, failed);
    return failed ? 1 : 0;
}
