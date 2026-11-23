#pragma once
// The F7 / photo-mode editor's own message panel (2026-11-21, the user: the game's native message box in photo mode split the mouse into two
// pointers and E did not close it). Pure logic only: text wrapping, pagination, the panel's geometry, the keys that close it and the
// open/close state. routeedit.cpp draws it in the dock's own overlay and feeds it the editor's own key reads; nothing here touches the game.
//
// Why the native box failed (see the board entry of 2026-11-21): while the editor is open, inputgate (photoeditorinput::Block) marks EVERY
// key and button event as blocked for the game, so E never reached the box's menu; and the box is a game menu, so the game also drew its
// own arrow next to the editor's pointer. The panel below is drawn by the editor, so there is one pointer and one key path.
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace editormessage {

// The two texts, as they were in the native box (same words).
inline constexpr const char* kFirstTitle = "Traffic Manager";
inline constexpr const char* kFirstBody =
    "You can manage traffic routes for this planet from photo mode. The routes you create decide where vehicles fly, and traffic is generated along them.\n\n"
    "Press F8 to open the Traffic Manager in photo mode, then create or edit a route. F8 again takes you back to the game.\n\n"
    "Autopilot near a traffic zone will follow the same path.";
inline constexpr const char* kReadmeTitle = "Traffic Manager - Readme";
inline constexpr const char* kReadmeBody =
    "Press F8 to open the Traffic Manager (it opens photo mode for you); press F8 again to close it and leave photo mode. Hold right mouse (controller: left stick click) to move the camera. Select a route in the view or in the route list.\n\n"
    "Use New route to make a route, then the point and shape tools to draw its path. Select an existing route to edit it. Set its height, speed, lanes and vehicle count; use Time rules for its schedule.\n\n"
    "Save (Ctrl+S) applies route edits; Cancel discards unsaved edits. Red structure warnings do not block routes. Use Test route to check clearance.\n\n"
    "City controls local traffic and density. Prepare traffic applies the current setup. Finish or cancel your route edits and leave combat first.\n\n"
    "Save your game to keep its routes, groups and traffic settings. Loading an older game save restores its setup. Other playthroughs start with installed defaults.\n\n"
    "Autopilot near a traffic zone follows its path. Reopen this Readme whenever you need it.";

// Script mode (2026-11-22, the user): shown every time it is switched on, in the same panel as the first-time message.
inline constexpr const char* kScriptTitle = "Script mode";
inline constexpr const char* kScriptBody =
    "Script mode is on. Vehicles follow the route path with no object detection: they do not look for walls, railings or objects.\n\n"
    "The only obstacles they know are the Hazard boxes you place, other vehicles (from their known positions, kept apart by their own size) and people. "
    "Place a Hazard box (Hazards tool) wherever something solid is in the way.\n\n"
    "Use this mode only when a road you built does not work well with the normal autopilot. Press Script mode again to turn it off.";

// The first-time message appears this many editor frames after the editor opens (as the native box: 30 frames, half a second).
inline constexpr int kFirstDelayFrames = 30;

// ---- text ----
// Words wrapped to `perLine` letters; "\n" ends a paragraph and "\n\n" leaves one empty line; a word longer than a line is cut.
inline std::vector<std::string> Wrap(const std::string& text, int perLine) {
    perLine = std::max(8, perLine);
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', start);
        const std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (para.empty()) out.emplace_back();
        else {
            std::string line;
            std::size_t i = 0;
            while (i < para.size()) {
                while (i < para.size() && para[i] == ' ') ++i;
                if (i >= para.size()) break;
                std::size_t j = para.find(' ', i);
                if (j == std::string::npos) j = para.size();
                std::string word = para.substr(i, j - i);
                i = j;
                while (int(word.size()) > perLine) {  // a word longer than a line
                    if (!line.empty()) { out.push_back(line); line.clear(); }
                    out.push_back(word.substr(0, std::size_t(perLine)));
                    word.erase(0, std::size_t(perLine));
                }
                if (word.empty()) continue;
                if (line.empty()) line = word;
                else if (int(line.size() + 1 + word.size()) <= perLine) line += " " + word;
                else { out.push_back(line); line = word; }
            }
            if (!line.empty()) out.push_back(line);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

// Lines cut into pages of at most `perPage` lines; a page never starts or ends with an empty line; there is always at least one page.
inline std::vector<std::vector<std::string>> Paginate(const std::vector<std::string>& lines, int perPage) {
    perPage = std::max(1, perPage);
    std::vector<std::vector<std::string>> pages;
    std::size_t i = 0;
    while (i < lines.size()) {
        while (i < lines.size() && lines[i].empty()) ++i;
        if (i >= lines.size()) break;
        std::vector<std::string> page;
        while (i < lines.size() && int(page.size()) < perPage) page.push_back(lines[i++]);
        while (!page.empty() && page.back().empty()) page.pop_back();
        pages.push_back(std::move(page));
    }
    if (pages.empty()) pages.emplace_back();
    return pages;
}

// ---- geometry (screen pixels of the editor's overlay) ----
struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool Contains(double px, double py) const { return w > 0 && px >= x && px <= x + w && py >= y && py <= y + h; }
};
inline constexpr double kLetterPx = 9.0;  // the editor's text advance (f7layers::kLetterPx; routeedit.cpp asserts they match)
inline constexpr double kLineH = 20.0, kHeader = 54.0, kFooter = 62.0, kSide = 28.0;
struct Geometry {
    Rect panel, ok, prev, next;
    double textX = 0, textY = 0, textW = 0, titleX = 0, titleY = 0;
    int perLine = 40, perPage = 6;
};
inline Geometry Layout(double screenW, double screenH, int linesOnPage) {
    Geometry g;
    const double pw = std::clamp(screenW - 40.0, 260.0, 660.0);
    g.perLine = std::max(8, int((pw - 2 * kSide) / kLetterPx));
    g.perPage = std::max(4, int((std::max(240.0, screenH) * 0.8 - kHeader - kFooter) / kLineH));
    const int lines = std::clamp(linesOnPage <= 0 ? g.perPage : linesOnPage, 1, g.perPage);
    const double ph = kHeader + lines * kLineH + kFooter;
    g.panel = { std::max(0.0, (screenW - pw) / 2), std::max(0.0, (screenH - ph) / 2), pw, ph };
    g.titleX = g.panel.x + kSide; g.titleY = g.panel.y + 18;
    g.textX = g.panel.x + kSide; g.textY = g.panel.y + kHeader; g.textW = pw - 2 * kSide;
    const double by = g.panel.y + ph - kFooter + 16;
    g.ok = { g.panel.x + pw - kSide - 110, by, 110, 30 };
    g.prev = { g.panel.x + kSide, by, 84, 30 };
    g.next = { g.panel.x + kSide + 92, by, 84, 30 };
    return g;
}

// ---- keys and state ----
// One frame's FRESH presses (edges), read by the editor from the keyboard / pad it already owns.
struct Input {
    bool e = false, enter = false, space = false, escape = false;  // close
    bool padAccept = false, padBack = false;                       // A / B close
    bool okClicked = false;                                        // a fresh left click inside the OK button
    bool prevPage = false, nextPage = false;                       // arrows / PageUp / PageDown / D-pad / the page buttons
};
struct Result {
    bool swallowed = false;  // the editor must not act on anything this frame
    bool closed = false;     // true on exactly one call per opening
    bool paged = false;
};
inline bool Closes(const Input& in) { return in.e || in.enter || in.space || in.escape || in.padAccept || in.padBack || in.okClicked; }

class Panel {
public:
    bool IsOpen() const { return open_; }
    int Page() const { return page_; }
    int PageCount() const { return int(pages_.size()); }
    const std::string& Title() const { return title_; }
    const std::vector<std::string>& Lines() const { static const std::vector<std::string> none; return open_ && page_ < int(pages_.size()) ? pages_[std::size_t(page_)] : none; }
    int Opened() const { return opened_; }
    int Closed() const { return closed_; }

    // Opens the panel on page 1 (a second Show while open replaces it: the Readme button pressed twice is still one open panel).
    void Show(const std::string& title, const std::string& body, int perLine, int perPage) {
        title_ = title; body_ = body;
        pages_ = Paginate(Wrap(body, perLine), perPage);
        page_ = 0;
        if (!open_) ++opened_;
        open_ = true;
    }
    // Wrap again for a new screen size (the page kept as far as it still exists).
    void Rewrap(int perLine, int perPage) {
        if (!open_) return;
        pages_ = Paginate(Wrap(body_, perLine), perPage);
        page_ = std::clamp(page_, 0, int(pages_.size()) - 1);
    }
    Result Update(const Input& in) {
        Result r;
        if (!open_) return r;
        r.swallowed = true;
        if (Closes(in)) { open_ = false; ++closed_; r.closed = true; pages_.clear(); return r; }
        const int last = int(pages_.size()) - 1;
        if (in.nextPage && page_ < last) { ++page_; r.paged = true; }
        else if (in.prevPage && page_ > 0) { --page_; r.paged = true; }
        return r;
    }
    // World change, editor closed, photo mode left: gone at once, nothing stays blocked (the editor derives its input block from IsOpen()).
    bool Reset() { const bool was = open_; open_ = false; pages_.clear(); page_ = 0; return was; }

private:
    bool open_ = false;
    int page_ = 0, opened_ = 0, closed_ = 0;
    std::string title_, body_;
    std::vector<std::vector<std::string>> pages_;
};

// The first-time trigger: counts editor frames (editor on, window focused); true exactly once, on frame kFirstDelayFrames of an opening.
// A frame with the editor off starts the count again. (Whether the message is due at all is the FVO_TrafficManagerSeen flag, kept in routeedit.cpp.)
struct FirstTimer {
    int frames = 0;
    bool Tick(bool editorOn, bool focused) {
        if (!editorOn) { frames = 0; return false; }
        if (!focused) return false;
        return ++frames == kFirstDelayFrames;
    }
};

}  // namespace editormessage
