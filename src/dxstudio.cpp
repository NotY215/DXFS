// src/dxstudio.cpp — Win32 GUI editor for DXFS, with lazy block loading.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>

#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "dx/format.hpp"
#include "dx/bigint.hpp"
#include "dx/blockstore.hpp"

static const wchar_t* APP_CLASS = L"DxStudioWnd";
static const wchar_t* APP_TITLE = L"DX Studio";

constexpr int    DPR            = 80;
constexpr size_t MAX_BUFFER     = 1000000000ULL;
constexpr UINT_PTR TIMER_CARET  = 1;
constexpr UINT_PTR TIMER_GEN    = 2;
constexpr UINT     GEN_SLICE_MS = 25;
constexpr size_t   GEN_ITER_PER_CHK = 64;

enum {
    IDM_NEW = 2001, IDM_OPEN, IDM_SAVE, IDM_SAVEAS, IDM_EXIT,
    IDM_UNDO, IDM_REDO, IDM_COPY, IDM_PASTE,
    IDM_FIND, IDM_FINDNEXT, IDM_JUMP,
    IDM_GEN_PI, IDM_GEN_E, IDM_GEN_LCG, IDM_GEN_PATTERN,
    IDM_INFO, IDM_ABOUT,
    IDM_MODE_AUTO, IDM_MODE_PACKED, IDM_MODE_RLE,
    IDM_MODE_PATTERN, IDM_MODE_CONSTANT, IDM_MODE_BLOCKED
};

// ======================= streaming pi / e generators =======================
struct SeriesGen {
    virtual ~SeriesGen() = default;
    virtual size_t step(size_t iters) = 0;
    virtual std::string digits(size_t n) const = 0;
    virtual size_t target() const = 0;
    virtual bool   done()   const = 0;
    virtual const char* name() const = 0;
};

struct PiGen : SeriesGen {
    size_t target_, guard_ = 24, g_;
    dx::Big a_, sumA_, b_, sumB_;
    uint64_t nA_ = 1, nB_ = 1;
    bool doneA_ = false, doneB_ = false;

    explicit PiGen(size_t target) : target_(target) {
        if (target_ < 1) target_ = 1;
        g_ = target_ + guard_ - 1;
        a_ = dx::pow10(g_); a_.divSmall(5);
        b_ = dx::pow10(g_); b_.divSmall(239);
        sumA_ = a_; sumB_ = b_;
    }
    size_t step(size_t iters) override {
        for (size_t i = 0; i < iters; ++i) {
            if (!doneA_) {
                a_.divSmall(25);
                if (a_.isZero()) doneA_ = true;
                else {
                    dx::Big t = a_; t.divSmall(2 * nA_ + 1);
                    if (t.isZero()) doneA_ = true;
                    else { if (nA_ & 1) sumA_.sub(t); else sumA_.add(t); ++nA_; }
                }
            }
            if (!doneB_) {
                b_.divSmall(239u * 239u);
                if (b_.isZero()) doneB_ = true;
                else {
                    dx::Big t = b_; t.divSmall(2 * nB_ + 1);
                    if (t.isZero()) doneB_ = true;
                    else { if (nB_ & 1) sumB_.sub(t); else sumB_.add(t); ++nB_; }
                }
            }
            if (doneA_ && doneB_) return target_;
        }
        double d = (double)nA_ * std::log10(25.0);
        size_t stable = (size_t)d;
        if (stable > target_) stable = target_;
        return stable;
    }
    std::string digits(size_t n) const override {
        dx::Big a = sumA_; a.mulSmall(16);
        dx::Big b = sumB_; b.mulSmall(4);
        a.sub(b);
        dx::Big r = a;
        for (size_t i = 0; i < guard_; ++i) r.divSmall(10);
        std::string s = r.toDecimal(target_);
        if (s.size() > n) s.resize(n);
        return s;
    }
    size_t target() const override { return target_; }
    bool   done()   const override { return doneA_ && doneB_; }
    const char* name() const override { return "pi"; }
};

struct EGen : SeriesGen {
    size_t target_, guard_ = 24, g_;
    dx::Big sum_, term_;
    uint64_t n_ = 1;

    explicit EGen(size_t target) : target_(target) {
        if (target_ < 1) target_ = 1;
        g_ = target_ + guard_ - 1;
        sum_ = dx::pow10(g_);
        term_ = sum_;
    }
    size_t step(size_t iters) override {
        for (size_t i = 0; i < iters; ++i) {
            term_.divSmall(n_);
            if (term_.isZero()) return target_;
            sum_.add(term_);
            ++n_;
        }
        double d = (double)n_ * std::log10((double)n_ / 2.718281828459045);
        size_t stable = (size_t)d;
        if (stable > target_) stable = target_;
        return stable;
    }
    std::string digits(size_t n) const override {
        dx::Big r = sum_;
        for (size_t i = 0; i < guard_; ++i) r.divSmall(10);
        std::string s = r.toDecimal(target_);
        if (s.size() > n) s.resize(n);
        return s;
    }
    size_t target() const override { return target_; }
    bool   done()   const override { return term_.isZero(); }
    const char* name() const override { return "e"; }
};

// ================================ app state ================================
struct App {
    HWND   hwnd  = nullptr;
    HFONT  hFont = nullptr;
    int charW = 8, charH = 16, lineH = 18;

    std::string path;
    dx::File    file;
    std::string digits;                // empty in lazy mode
    std::string mode = "auto";

    // ==== LAZY ====
    dx::BlockReader blockReader;
    bool lazyMode = false;

    size_t cursor  = 0;
    size_t topLine = 0;
    bool   insert  = true;
    bool   dirty   = false;
    bool   cursorVisible = true;

    bool   panning      = false;
    int    panAnchorY   = 0;
    size_t panAnchorTop = 0;

    enum class LastAction { None, Typing, Other };
    std::vector<std::string> undoStack, redoStack;
    std::vector<size_t>      undoCursor, redoCursor;
    LastAction lastAction     = LastAction::None;
    DWORD      lastActionTime = 0;

    int lastConstantId = -1;

    enum class GenState { Idle, Running } genState = GenState::Idle;
    std::unique_ptr<SeriesGen> gen;
    size_t genTarget = 0;
    size_t genStable = 0;

    enum class Bar { None, Find, Jump, GenCount, GenPattern } bar = Bar::None;
    std::string barPrompt, barText;
    int    pendingGen = 0;
    size_t pendingGenCount = 0;
    std::string lastFind;
    std::string message = "Ready";
    int clientW = 0, clientH = 0;
};

static App g;

// --------------------------------- helpers --------------------------------
static std::wstring toW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string toU8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}
static int digitX(int c) { return c + c / 5; }

// ==== LAZY ====
static uint64_t totalDigits() {
    return g.lazyMode ? g.blockReader.count() : (uint64_t)g.digits.size();
}

// ==== LAZY ====  materialize the whole buffer so edits can proceed
static void ensureEditable() {
    if (!g.lazyMode) return;
    g.digits = g.blockReader.readRange(0, g.blockReader.count());
    g.lazyMode = false;
    g.blockReader.clearCache();
}

static void createFont() {
    if (g.hFont) DeleteObject(g.hFont);
    LOGFONTW lf = {};
    lf.lfHeight = -18;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lstrcpynW(lf.lfFaceName, L"Consolas", LF_FACESIZE);
    g.hFont = CreateFontIndirectW(&lf);
    if (!g.hFont) {
        lstrcpynW(lf.lfFaceName, L"Courier New", LF_FACESIZE);
        g.hFont = CreateFontIndirectW(&lf);
    }
}

static void measureFont() {
    if (!g.hwnd || !g.hFont) return;
    HDC hdc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(hdc, g.hFont);
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    g.charH = tm.tmHeight;
    g.lineH = tm.tmHeight + 3;
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, L"0", 1, &sz);
    g.charW = sz.cx;
    SelectObject(hdc, old);
    ReleaseDC(g.hwnd, hdc);
}

static int gridRowsVisible() {
    int statusH = g.lineH + 4;
    int barH    = (g.bar != App::Bar::None) ? g.lineH + 8 : 0;
    int gridTop = 6;
    int gridBottom = g.clientH - statusH - barH - 4;
    int rows = (gridBottom - gridTop) / g.lineH;
    return rows > 0 ? rows : 1;
}

static void updateScrollbar() {
    int rows = gridRowsVisible();
    uint64_t total = totalDigits();
    int totalLines = total == 0 ? 1 : int((total + DPR - 1) / DPR);

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin   = 0;
    si.nMax   = totalLines > 0 ? totalLines - 1 : 0;
    si.nPage  = (UINT)(rows > 0 ? rows : 1);
    si.nPos   = (int)g.topLine;
    SetScrollInfo(g.hwnd, SB_VERT, &si, TRUE);
}

static void ensureCursorVisible() {
    uint64_t total = totalDigits();
    if (total == 0) { g.cursor = 0; g.topLine = 0; updateScrollbar(); return; }
    if ((uint64_t)g.cursor > total) g.cursor = (size_t)total;
    int rows = gridRowsVisible();
    size_t curLine = g.cursor / DPR;
    if (curLine < g.topLine) g.topLine = curLine;
    if (curLine >= g.topLine + (size_t)rows)
        g.topLine = curLine - (size_t)rows + 1;
    updateScrollbar();
}

// -------------------------------- undo/redo -------------------------------
static void beginEdit(bool isTyping) {
    ensureEditable();
    DWORD now = GetTickCount();
    bool push = true;
    if (isTyping && g.lastAction == App::LastAction::Typing &&
        (now - g.lastActionTime) < 1500)
        push = false;
    if (push) {
        constexpr size_t CAP = 200;
        if (g.undoStack.size() >= CAP) {
            g.undoStack.erase(g.undoStack.begin());
            g.undoCursor.erase(g.undoCursor.begin());
        }
        g.undoStack.push_back(g.digits);
        g.undoCursor.push_back(g.cursor);
        g.redoStack.clear();
        g.redoCursor.clear();
    }
    g.lastAction     = isTyping ? App::LastAction::Typing : App::LastAction::Other;
    g.lastActionTime = now;
    g.lastConstantId = -1;
}

static void doUndo() {
    if (g.lazyMode) { g.message = "Nothing to undo (view mode)"; return; }
    if (g.undoStack.empty()) { g.message = "Nothing to undo"; return; }
    g.redoStack.push_back(g.digits);
    g.redoCursor.push_back(g.cursor);
    g.digits = g.undoStack.back(); g.undoStack.pop_back();
    g.cursor = g.undoCursor.back(); g.undoCursor.pop_back();
    g.dirty = true;
    g.lastAction = App::LastAction::None;
    g.lastConstantId = -1;
    ensureCursorVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void doRedo() {
    if (g.lazyMode) { g.message = "Nothing to redo (view mode)"; return; }
    if (g.redoStack.empty()) { g.message = "Nothing to redo"; return; }
    g.undoStack.push_back(g.digits);
    g.undoCursor.push_back(g.cursor);
    g.digits = g.redoStack.back(); g.redoStack.pop_back();
    g.cursor = g.redoCursor.back(); g.redoCursor.pop_back();
    g.dirty = true;
    g.lastAction = App::LastAction::None;
    g.lastConstantId = -1;
    ensureCursorVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ---------------------------------- paint ---------------------------------
static void paint(HDC hdc, int w, int h) {
    RECT full = { 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 30));
    FillRect(hdc, &full, bg);
    DeleteObject(bg);

    SelectObject(hdc, g.hFont);
    SetBkMode(hdc, TRANSPARENT);

    int statusH = g.lineH + 4;
    int barH    = (g.bar != App::Bar::None) ? g.lineH + 8 : 0;
    int gridTop = 6;
    int gridBottom = h - statusH - barH - 4;
    int rows = (gridBottom - gridTop) / g.lineH;
    if (rows < 1) rows = 1;

    const int gutterPx = 8;
    const int LABEL_CH = 9;
    const int labelPx  = LABEL_CH * g.charW;
    const int digitsPx = gutterPx + labelPx;

    // ==== LAZY ====  build a small window of digits covering only what's on screen
    uint64_t total = totalDigits();
    size_t visStart = g.topLine * DPR;
    size_t visLen = 0;
    std::string lazyBuf;
    const char* buf = nullptr;

    if (g.lazyMode) {
        if (visStart < total) {
            visLen = (size_t)(std::min<uint64_t>((uint64_t)rows * DPR,
                                                 total - visStart));
            lazyBuf = g.blockReader.readRange(visStart, visLen);
            buf = lazyBuf.data();
            if (lazyBuf.size() < visLen) visLen = lazyBuf.size();
        }
    } else {
        if (visStart < g.digits.size()) {
            visLen = (std::min)((size_t)rows * DPR, g.digits.size() - visStart);
            buf = g.digits.data() + visStart;
        }
    }

    HRGN clip = CreateRectRgn(0, 0, w, gridBottom);
    SelectClipRgn(hdc, clip);

    for (int r = 0; r < rows; ++r) {
        size_t line = g.topLine + (size_t)r;
        size_t start = line * DPR;
        int y = gridTop + r * g.lineH;

        // is any digit of this row visible?
        uint64_t firstIdx = (uint64_t)start;
        uint64_t lastIdx  = (uint64_t)start + DPR - 1;
        bool hasData = (total == 0 && line == 0) ||
                       (firstIdx <= total && visLen > 0 &&
                        lastIdx >= visStart && firstIdx < visStart + visLen);
        if (!hasData && r > 0) break;

        wchar_t lbl[16];
        swprintf(lbl, 16, L"%08llu", (unsigned long long)start);
        SetTextColor(hdc, RGB(110, 120, 140));
        TextOutW(hdc, gutterPx, y, lbl, 8);

        SetTextColor(hdc, RGB(220, 220, 220));
        for (int c = 0; c < DPR; ++c) {
            uint64_t idx = (uint64_t)start + (uint64_t)c;
            if (idx >= total) break;
            if (buf == nullptr || idx < visStart || idx >= visStart + visLen) continue;
            size_t local = (size_t)(idx - visStart);
            int x = digitsPx + digitX(c) * g.charW;
            wchar_t ch[2] = { (wchar_t)(unsigned char)buf[local], 0 };
            TextOutW(hdc, x, y, ch, 1);
        }
    }

    if (g.cursorVisible) {
        size_t curLine = g.cursor / DPR;
        if (curLine >= g.topLine && curLine < g.topLine + (size_t)rows) {
            int r = (int)(curLine - g.topLine);
            int c = (int)(g.cursor % DPR);
            int x = digitsPx + digitX(c) * g.charW;
            int y = gridTop + r * g.lineH;

            if (g.insert || (uint64_t)g.cursor >= total) {
                RECT cur = { x, y, x + 2, y + g.charH };
                HBRUSH cb = CreateSolidBrush(RGB(230, 230, 230));
                FillRect(hdc, &cur, cb);
                DeleteObject(cb);
            } else {
                RECT cur = { x, y, x + g.charW, y + g.charH };
                HBRUSH cb = CreateSolidBrush(RGB(0, 110, 200));
                FillRect(hdc, &cur, cb);
                DeleteObject(cb);
                if (buf != nullptr && g.cursor >= visStart &&
                    g.cursor < visStart + visLen) {
                    wchar_t ch[2] = { (wchar_t)(unsigned char)buf[g.cursor - visStart], 0 };
                    SetTextColor(hdc, RGB(255, 255, 255));
                    TextOutW(hdc, x, y, ch, 1);
                }
            }
        }
    }

    SelectClipRgn(hdc, nullptr);
    DeleteObject(clip);

    if (g.bar != App::Bar::None) {
        int barY = h - statusH - barH;
        RECT br = { 0, barY, w, barY + barH };
        HBRUSH bb = CreateSolidBrush(RGB(40, 44, 52));
        FillRect(hdc, &br, bb);
        DeleteObject(bb);

        SetTextColor(hdc, RGB(200, 200, 200));
        std::wstring prompt = toW(g.barPrompt);
        TextOutW(hdc, 8, barY + 4, prompt.c_str(), (int)prompt.size());

        std::wstring txt = toW(g.barText);
        int tx = 8 + (int)prompt.size() * g.charW + 4;
        TextOutW(hdc, tx, barY + 4, txt.c_str(), (int)txt.size());

        RECT cr = { tx + (int)txt.size() * g.charW, barY + 3,
                    tx + (int)txt.size() * g.charW + 1, barY + 3 + g.charH };
        HBRUSH cbr = CreateSolidBrush(RGB(230, 230, 230));
        FillRect(hdc, &cr, cbr);
        DeleteObject(cbr);
    }

    {
        int sy = h - statusH;
        RECT sr = { 0, sy, w, h };
        HBRUSH sb = CreateSolidBrush(RGB(45, 48, 56));
        FillRect(hdc, &sr, sb);
        DeleteObject(sb);

        size_t curDisp = total == 0 ? 1 :
                         (g.cursor < total ? g.cursor + 1 : (size_t)total + 1);
        std::wstring s = L" ";
        s += std::to_wstring(curDisp);
        s += L"/";
        s += std::to_wstring((unsigned long long)total);
        s += L"   ";
        s += g.insert ? L"INS" : L"OVR";
        s += L"   codec=";
        if (g.lazyMode) s += L"blocked(lazy)";
        else            s += toW(dx::kindName(g.file.kind));
        s += L"   mode="; s += toW(g.mode);
        if (g.lazyMode) {
            s += L"   cache=";
            s += std::to_wstring(g.blockReader.cachedBlockCount());
            s += L"/";
            s += std::to_wstring(g.blockReader.blockCount());
            s += L" blocks";
        } else {
            s += L"   payload="; s += std::to_wstring(g.file.payload.size());
            s += L" B";
            if (!g.digits.empty() && g.file.payload.size() > 0) {
                double bits = 8.0 * (double)g.file.payload.size() /
                              (double)g.digits.size();
                wchar_t b[40];
                swprintf(b, 40, L"   %.4f bits/digit", bits);
                s += b;
            }
        }
        s += L"   |   ";
        if (g.genState == App::GenState::Running) {
            s += L"[";
            s += toW(g.gen ? g.gen->name() : "gen");
            s += L"] computing ";
            s += std::to_wstring(g.genStable);
            s += L" / ";
            s += std::to_wstring(g.genTarget);
            s += L" digits...  (Esc to cancel)";
        } else {
            s += toW(g.message);
        }

        RECT tr = sr; tr.left += 8; tr.top += 3;
        SetTextColor(hdc, RGB(200, 205, 215));
        DrawTextW(hdc, s.c_str(), -1, &tr,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
    }
}

// --------------------------------- file I/O -------------------------------
static void refreshTitle() {
    std::wstring t = APP_TITLE;
    if (!g.path.empty()) t += L" - " + toW(g.path);
    if (g.dirty) t += L" *";
    SetWindowTextW(g.hwnd, t.c_str());
}

static void resetEditHistory() {
    g.undoStack.clear(); g.redoStack.clear();
    g.undoCursor.clear(); g.redoCursor.clear();
    g.lastAction = App::LastAction::None;
}

static void doNew() {
    g.path.clear();
    g.digits.clear();
    g.file = {};
    g.lazyMode = false;
    g.blockReader.clearCache();
    g.cursor = 0; g.topLine = 0; g.dirty = false;
    g.lastConstantId = -1;
    resetEditHistory();
    g.message = "New file";
    refreshTitle();
    ensureCursorVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static bool doSaveTo(const std::string& p) {
    try {
        std::vector<uint8_t> bytes;

        if (g.mode == "auto" && g.lastConstantId >= 0) {
            // fast path for freshly-generated pi / e
            dx::File f;
            f.kind = dx::Kind::Constant;
            f.count = g.digits.size();
            f.payload.push_back((uint8_t)g.lastConstantId);
            g.file = f;
            bytes = dx::serialize(f);
        } else if (g.mode == "blocked") {
            // force the block-indexed format
            dx::BlockWriter bw;
            bw.append(g.digits);
            bytes = bw.finish();
            g.file = {};
            g.file.kind = dx::Kind::Blocked;
            g.file.count = g.digits.size();
        } else if (g.mode == "auto" && g.digits.size() >= 1'000'000) {
            // auto-pick block format for huge buffers
            dx::BlockWriter bw;
            bw.append(g.digits);
            bytes = bw.finish();
            g.file = {};
            g.file.kind = dx::Kind::Blocked;
            g.file.count = g.digits.size();
        } else {
            g.file = dx::encode(g.digits, g.mode);
            bytes = dx::serialize(g.file);
        }

        FILE* f = fopen(p.c_str(), "wb");
        if (!f) { g.message = "Cannot write " + p; return false; }
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
        g.path = p;
        g.dirty = false;

        char buf[256];
        std::snprintf(buf, sizeof buf,
            "Saved %zu digits as %s -> %zu bytes (%.4f bits/digit)",
            g.digits.size(), dx::kindName(g.file.kind), bytes.size(),
            bytes.size() * 8.0 /
                (double)(g.digits.empty() ? 1 : g.digits.size()));
        g.message = buf;
        refreshTitle();
        return true;
    } catch (const std::exception& e) {
        g.message = std::string("Save error: ") + e.what();
        return false;
    }
}

static bool doSaveAs() {
    wchar_t fn[MAX_PATH] = L"";
    if (!g.path.empty()) lstrcpynW(fn, toW(g.path).c_str(), MAX_PATH);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g.hwnd;
    ofn.lpstrFilter = L"DX Files (*.dx)\0*.dx\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = fn;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"dx";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;
    return doSaveTo(toU8(fn));
}

static void doOpen() {
    wchar_t fn[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g.hwnd;
    ofn.lpstrFilter =
        L"DX Files (*.dx)\0*.dx\0Digit Text (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = fn;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    std::string p = toU8(fn);
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { g.message = "Cannot open " + p; return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> bytes;
    if (sz > 0) {
        bytes.resize((size_t)sz);
        size_t got = fread(bytes.data(), 1, (size_t)sz, f);
        bytes.resize(got);
    }
    fclose(f);

    // ==== LAZY ====  try block-indexed format first
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DXF2", 4) == 0) {
        std::string err;
        dx::BlockReader rd;
        if (rd.open(bytes, &err)) {
            g.blockReader = std::move(rd);
            g.digits.clear();
            g.lazyMode = true;
            g.file = {};
            g.file.kind  = dx::Kind::Blocked;
            g.file.count = g.blockReader.count();
            g.path = p;
            g.cursor = 0; g.topLine = 0; g.dirty = false;
            g.lastConstantId = -1;
            resetEditHistory();
            char buf[256];
            std::snprintf(buf, sizeof buf,
                "Opened lazy: %llu digits, %u blocks, cache=%zu",
                (unsigned long long)g.blockReader.count(),
                (unsigned)g.blockReader.blockCount(),
                g.blockReader.cachedBlockCount());
            g.message = buf;
            refreshTitle();
            ensureCursorVisible();
            InvalidateRect(g.hwnd, nullptr, FALSE);
            return;
        }
        // fall through if the block reader rejected the file
    }

    // classic single-payload format
    std::string err;
    auto parsed = dx::deserialize(bytes, &err);
    if (!parsed) {
        std::string txt(bytes.begin(), bytes.end());
        while (!txt.empty() && (txt.back() == '\n' || txt.back() == '\r'))
            txt.pop_back();
        if (!txt.empty() && dx::isDigits(txt)) {
            g.lazyMode = false;
            g.digits = txt;
            g.file   = dx::encode(txt, g.mode);
            g.path   = p;
            g.cursor = 0; g.topLine = 0; g.dirty = false;
            g.lastConstantId = -1;
            resetEditHistory();
            g.message = "Loaded text: " + std::to_string(txt.size()) + " digits";
            refreshTitle();
            ensureCursorVisible();
            InvalidateRect(g.hwnd, nullptr, FALSE);
            return;
        }
        g.message = "Load error: " + err;
        return;
    }

    g.lazyMode = false;
    g.file = *parsed;
    if (g.file.kind == dx::Kind::Constant) {
        g.message = "Materializing constant digits (this can take a moment)...";
        InvalidateRect(g.hwnd, nullptr, FALSE);
        UpdateWindow(g.hwnd);
    }
    g.digits = dx::materialize(g.file);
    g.path = p;
    g.cursor = 0; g.topLine = 0; g.dirty = false;

    if (g.file.kind == dx::Kind::Constant && !g.file.payload.empty())
        g.lastConstantId = g.file.payload[0];
    else
        g.lastConstantId = -1;

    resetEditHistory();
    g.message = "Loaded " + std::to_string(g.digits.size()) + " digits (" +
                dx::kindName(g.file.kind) + ")";
    refreshTitle();
    ensureCursorVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ----------------------------- generators ---------------------------------
static void startStreamingGen(int genId, size_t n) {
    if (n == 0)        { g.message = "Count must be > 0"; return; }
    if (n > MAX_BUFFER){ n = MAX_BUFFER; }

    std::unique_ptr<SeriesGen> gen;
    if (genId == IDM_GEN_PI) gen.reset(new PiGen(n));
    else if (genId == IDM_GEN_E) gen.reset(new EGen(n));
    else return;

    g.digits.clear();
    g.lazyMode = false;
    g.blockReader.clearCache();
    g.cursor = 0; g.topLine = 0; g.dirty = true;
    g.lastConstantId = -1;
    resetEditHistory();

    g.gen       = std::move(gen);
    g.genTarget = n;
    g.genStable = 0;
    g.genState  = App::GenState::Running;
    g.pendingGen = genId;

    refreshTitle();
    updateScrollbar();
    SetTimer(g.hwnd, TIMER_GEN, 1, nullptr);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void doInstantGen(int genId, size_t n, const std::string& arg = "") {
    if (n == 0) { g.message = "Count must be > 0"; return; }
    if (n > MAX_BUFFER) n = MAX_BUFFER;

    char msg[80];
    std::snprintf(msg, sizeof msg, "Generating %zu digits...", n);
    g.message = msg;
    InvalidateRect(g.hwnd, nullptr, FALSE);
    UpdateWindow(g.hwnd);

    dx::File f;
    f.count = n;
    try {
        switch (genId) {
            case IDM_GEN_LCG: {
                uint64_t seed = arg.empty()
                    ? 88172645463325252ULL
                    : std::strtoull(arg.c_str(), nullptr, 10);
                f.kind = dx::Kind::Lcg;
                for (int i = 0; i < 8; ++i)
                    f.payload.push_back((unsigned char)(seed >> (8 * i)));
                break;
            }
            case IDM_GEN_PATTERN: {
                std::string pat = arg.empty() ? std::string("3141592653") : arg;
                if (!dx::isDigits(pat) || pat.empty()) {
                    g.message = "Pattern must be a non-empty digit string";
                    return;
                }
                std::string sample;
                sample.reserve(n);
                for (size_t i = 0; i < n; ++i)
                    sample.push_back(pat[i % pat.size()]);
                f = dx::encode(sample, "pattern");
                break;
            }
            default: return;
        }
    } catch (const std::exception& e) {
        g.message = std::string("Generate error: ") + e.what();
        return;
    }

    g.file   = f;
    g.digits = dx::materialize(f);
    g.lazyMode = false;
    g.blockReader.clearCache();
    g.cursor = 0; g.topLine = 0; g.dirty = true;
    g.lastConstantId = -1;
    resetEditHistory();
    g.message = "Generated " + std::to_string(g.digits.size()) + " digits (" +
                dx::kindName(f.kind) + ")";
    refreshTitle();
    ensureCursorVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ------------------------------ clipboard ---------------------------------
static void doCopyAll() {
    uint64_t total = totalDigits();
    if (total == 0) { g.message = "Nothing to copy"; return; }
    std::string all = g.lazyMode
        ? g.blockReader.readRange(0, total)
        : g.digits;
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, all.size() + 1);
    if (h) {
        char* p = (char*)GlobalLock(h);
        memcpy(p, all.data(), all.size());
        p[all.size()] = '\0';
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
    g.message = "Copied " + std::to_string(all.size()) + " digits";
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void doPaste() {
    if (!IsClipboardFormatAvailable(CF_TEXT)) {
        g.message = "Clipboard empty"; return;
    }
    if (!OpenClipboard(g.hwnd)) return;
    std::string txt;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        const char* p = (const char*)GlobalLock(h);
        if (p) {
            for (const char* q = p; *q; ++q)
                if (*q >= '0' && *q <= '9') txt += *q;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (txt.empty()) { g.message = "No digits in clipboard"; return; }
    if ((uint64_t)g.digits.size() + txt.size() > MAX_BUFFER) {
        g.message = "Buffer size limit exceeded"; return;
    }
    ensureEditable();
    beginEdit(false);
    g.digits.insert(g.cursor, txt);
    g.cursor += txt.size();
    g.dirty = true;
    g.cursorVisible = true;
    g.message = "Pasted " + std::to_string(txt.size()) + " digits";
    ensureCursorVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ------------------------------- input bar --------------------------------
static void showBar(App::Bar b, const std::string& prompt,
                    const std::string& initial = "") {
    g.bar = b; g.barPrompt = prompt; g.barText = initial;
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
static void hideBar() {
    g.bar = App::Bar::None;
    g.barPrompt.clear();
    g.barText.clear();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void findNextFrom(size_t from) {
    if (g.lastFind.empty()) { g.message = "Nothing to find"; return; }
    std::string hay = g.lazyMode ? g.blockReader.readRange(0, g.blockReader.count())
                                 : g.digits;
    size_t pos = hay.find(g.lastFind, from);
    if (pos == std::string::npos) pos = hay.find(g.lastFind, 0);
    if (pos != std::string::npos) {
        g.cursor = pos;
        ensureCursorVisible();
        g.message = "Found at offset " + std::to_string(pos);
    } else {
        g.message = "Not found: " + g.lastFind;
    }
}

static void commitBar() {
    App::Bar b = g.bar;
    std::string text = g.barText;
    hideBar();
    switch (b) {
        case App::Bar::Find:
            g.lastFind = text;
            findNextFrom(0);
            break;
        case App::Bar::Jump:
            try {
                size_t n = (size_t)std::stoull(text);
                uint64_t total = totalDigits();
                if (total == 0) n = 0;
                else if ((uint64_t)n > total) n = (size_t)total;
                g.cursor = n;
                ensureCursorVisible();
                g.message = "Jumped to offset " + std::to_string(n);
            } catch (...) { g.message = "Invalid offset"; }
            break;
        case App::Bar::GenCount:
            try {
                size_t n = (size_t)std::stoull(text);
                if (n == 0) { g.message = "Count must be > 0"; break; }
                if (g.pendingGen == IDM_GEN_PATTERN) {
                    g.pendingGenCount = n;
                    showBar(App::Bar::GenPattern, "pattern: ", "3141592653");
                } else if (g.pendingGen == IDM_GEN_LCG) {
                    g.pendingGenCount = n;
                    showBar(App::Bar::GenPattern, "seed: ", "42");
                } else if (g.pendingGen == IDM_GEN_PI ||
                           g.pendingGen == IDM_GEN_E) {
                    startStreamingGen(g.pendingGen, n);
                    g.pendingGen = 0;
                } else {
                    g.pendingGen = 0;
                }
            } catch (...) { g.message = "Invalid count"; }
            break;
        case App::Bar::GenPattern:
            if (g.pendingGen == IDM_GEN_PATTERN)
                doInstantGen(IDM_GEN_PATTERN, g.pendingGenCount, text);
            else if (g.pendingGen == IDM_GEN_LCG)
                doInstantGen(IDM_GEN_LCG, g.pendingGenCount, text);
            g.pendingGen = 0;
            g.pendingGenCount = 0;
            break;
        default: break;
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// --------------------------------- menu -----------------------------------
static HMENU buildMenu() {
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_NEW,    L"&New\tCtrl+N");
    AppendMenuW(file, MF_STRING, IDM_OPEN,   L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_SAVE,   L"&Save\tCtrl+S");
    AppendMenuW(file, MF_STRING, IDM_SAVEAS, L"Save &As...\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT,   L"E&xit");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_UNDO,  L"&Undo\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, IDM_REDO,  L"&Redo\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_COPY,  L"Copy &All\tCtrl+C");
    AppendMenuW(edit, MF_STRING, IDM_PASTE, L"&Paste\tCtrl+V");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_FIND,     L"&Find...\tCtrl+F");
    AppendMenuW(edit, MF_STRING, IDM_FINDNEXT, L"Find &Next\tF3");
    AppendMenuW(edit, MF_STRING, IDM_JUMP,     L"&Jump to offset...\tCtrl+G");

    HMENU gen = CreatePopupMenu();
    AppendMenuW(gen, MF_STRING, IDM_GEN_PI,      L"First N digits of &pi");
    AppendMenuW(gen, MF_STRING, IDM_GEN_E,       L"First N digits of &e");
    AppendMenuW(gen, MF_STRING, IDM_GEN_LCG,     L"&Random (LCG)");
    AppendMenuW(gen, MF_STRING, IDM_GEN_PATTERN, L"Repeating &pattern");

    HMENU codec = CreatePopupMenu();
    AppendMenuW(codec, MF_STRING, IDM_MODE_AUTO,     L"&Auto");
    AppendMenuW(codec, MF_STRING, IDM_MODE_PACKED,   L"&Packed");
    AppendMenuW(codec, MF_STRING, IDM_MODE_RLE,      L"&RLE");
    AppendMenuW(codec, MF_STRING, IDM_MODE_PATTERN,  L"&Pattern");
    AppendMenuW(codec, MF_STRING, IDM_MODE_CONSTANT, L"&Constant (pi, e)");
    AppendMenuW(codec, MF_STRING, IDM_MODE_BLOCKED,  L"&Blocked (lazy)");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_INFO,  L"&Info");
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About");

    HMENU root = CreateMenu();
    AppendMenuW(root, MF_POPUP, (UINT_PTR)file,  L"&File");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)edit,  L"&Edit");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)gen,   L"&Generate");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)codec, L"&Codec");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)help,  L"&Help");
    return root;
}

// -------------------------------- movement --------------------------------
static void moveCursor(int direction, long long amount) {
    long long pos   = (long long)g.cursor;
    long long total = (long long)totalDigits();
    pos += (direction < 0) ? -amount : amount;
    if (pos < 0) pos = 0;
    if (pos > total) pos = total;
    g.cursor = (size_t)pos;
}
static void moveCursorLineStart() {
    g.cursor = (g.cursor / DPR) * DPR;
    if ((uint64_t)g.cursor > totalDigits()) g.cursor = (size_t)totalDigits();
}
static void moveCursorLineEnd() {
    size_t lineEnd = (g.cursor / DPR + 1) * DPR;
    uint64_t total = totalDigits();
    g.cursor = (std::min)((uint64_t)lineEnd, total);
}
static void moveCursorFileStart() { g.cursor = 0; g.topLine = 0; }
static void moveCursorFileEnd() {
    uint64_t total = totalDigits();
    g.cursor = (size_t)total;
    int rows = gridRowsVisible();
    size_t totalLines = (size_t)((total + DPR - 1) / DPR);
    g.topLine = totalLines > (size_t)rows ? totalLines - (size_t)rows : 0;
}

// -------------------------------- WndProc ---------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            createFont();
            measureFont();
            SetTimer(hwnd, TIMER_CARET, 530, nullptr);
            updateScrollbar();
            return 0;

        case WM_SIZE:
            g.clientW = LOWORD(lp);
            g.clientH = HIWORD(lp);
            ensureCursorVisible();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = 720;
            mmi->ptMinTrackSize.y = 480;
            return 0;
        }

        case WM_TIMER:
            if (wp == TIMER_CARET) {
                g.cursorVisible = !g.cursorVisible;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == TIMER_GEN && g.genState == App::GenState::Running && g.gen) {
                DWORD start = GetTickCount();
                size_t stable = g.genStable;
                while (GetTickCount() - start < GEN_SLICE_MS) {
                    stable = g.gen->step(GEN_ITER_PER_CHK);
                    if (g.gen->done()) break;
                }
                if (g.gen->done()) stable = g.gen->target();
                if (stable > g.genTarget) stable = g.genTarget;
                g.genStable = stable;
                g.digits = g.gen->digits(stable);
                g.cursor = g.digits.size();
                g.cursorVisible = true;
                ensureCursorVisible();
                InvalidateRect(hwnd, nullptr, FALSE);

                if (g.gen->done()) {
                    KillTimer(hwnd, TIMER_GEN);
                    g.digits = g.gen->digits(g.gen->target());
                    g.cursor = g.digits.size();
                    g.dirty = true;
                    g.lastConstantId = (g.pendingGen == IDM_GEN_PI) ? 0 :
                                       (g.pendingGen == IDM_GEN_E)  ? 1 : -1;
                    g.pendingGen = 0;
                    g.genState = App::GenState::Idle;
                    g.gen.reset();
                    ensureCursorVisible();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            return 0;

        case WM_ERASEBKGND: return 1;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && g.panning) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w < 1) w = 1;
            if (h < 1) h = 1;

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
            paint(memDC, w, h);
            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int steps = delta / WHEEL_DELTA * 3;
            int rows = gridRowsVisible();
            uint64_t total = totalDigits();
            int totalLines = total == 0 ? 1 : int((total + DPR - 1) / DPR);
            int maxTop = (std::max)(0, totalLines - rows);
            int newTop = (int)g.topLine - steps;
            if (newTop < 0) newTop = 0;
            if (newTop > maxTop) newTop = maxTop;
            if ((size_t)newTop != g.topLine) {
                g.topLine = (size_t)newTop;
                updateScrollbar();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_VSCROLL: {
            int code = LOWORD(wp);
            int rows = gridRowsVisible();
            uint64_t total = totalDigits();
            int totalLines = total == 0 ? 1 : int((total + DPR - 1) / DPR);
            int maxTop = (std::max)(0, totalLines - rows);
            int newTop = (int)g.topLine;

            switch (code) {
                case SB_LINEUP:    newTop -= 1;     break;
                case SB_LINEDOWN:  newTop += 1;     break;
                case SB_PAGEUP:    newTop -= rows;  break;
                case SB_PAGEDOWN:  newTop += rows;  break;
                case SB_TOP:       newTop = 0;      break;
                case SB_BOTTOM:    newTop = maxTop; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO si = {};
                    si.cbSize = sizeof(si);
                    si.fMask = SIF_TRACKPOS;
                    GetScrollInfo(hwnd, SB_VERT, &si);
                    newTop = si.nTrackPos;
                    break;
                }
            }
            if (newTop < 0) newTop = 0;
            if (newTop > maxTop) newTop = maxTop;
            if ((size_t)newTop != g.topLine) {
                g.topLine = (size_t)newTop;
                updateScrollbar();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MBUTTONDOWN:
            g.panning      = true;
            g.panAnchorY   = GET_Y_LPARAM(lp);
            g.panAnchorTop = g.topLine;
            SetCapture(hwnd);
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return 0;

        case WM_MBUTTONUP:
            if (g.panning) {
                g.panning = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (g.panning) {
                int dy = GET_Y_LPARAM(lp) - g.panAnchorY;
                int lines = dy / g.lineH;
                int rows = gridRowsVisible();
                uint64_t total = totalDigits();
                int totalLines = total == 0 ? 1 : int((total + DPR - 1) / DPR);
                int maxTop = (std::max)(0, totalLines - rows);
                long long newTop = (long long)g.panAnchorTop - lines;
                if (newTop < 0) newTop = 0;
                if (newTop > maxTop) newTop = maxTop;
                if ((size_t)newTop != g.topLine) {
                    g.topLine = (size_t)newTop;
                    updateScrollbar();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (g.panning) {
                g.panning = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            if (g.genState == App::GenState::Running) return 0;
            int mx = GET_X_LPARAM(lp);
            int my = GET_Y_LPARAM(lp);
            int statusH = g.lineH + 4;
            int barH = (g.bar != App::Bar::None) ? g.lineH + 8 : 0;
            int gridTop = 6;
            int gridBottom = g.clientH - statusH - barH - 4;
            if (my < gridTop || my >= gridBottom) return 0;

            int r = (my - gridTop) / g.lineH;
            size_t line = g.topLine + (size_t)r;
            size_t start = line * DPR;
            int digitsPx = 8 + 9 * g.charW;
            int x = mx - digitsPx;
            uint64_t total = totalDigits();
            if (x < 0) {
                g.cursor = (std::min)((uint64_t)start, total);
                ensureCursorVisible();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            int c = 0;
            while (c < DPR - 1 && digitX(c + 1) * g.charW <= x) c++;
            uint64_t idx = (uint64_t)start + (uint64_t)c;
            if (idx > total) idx = total;
            g.cursor = (size_t)idx;
            g.cursorVisible = true;
            ensureCursorVisible();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            if (g.bar != App::Bar::None) {
                if (wp == VK_ESCAPE) { hideBar(); return 0; }
                if (wp == VK_RETURN) { commitBar(); return 0; }
                if (wp == VK_BACK) {
                    if (!g.barText.empty()) g.barText.pop_back();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                return 0;
            }

            if (g.genState == App::GenState::Running) {
                if (wp == VK_ESCAPE) {
                    KillTimer(hwnd, TIMER_GEN);
                    g.genState = App::GenState::Idle;
                    g.gen.reset();
                    g.genStable = 0;
                    g.genTarget = 0;
                    g.pendingGen = 0;
                    g.message = "Generation cancelled";
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            g.cursorVisible = true;
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;

            if (ctrl) {
                switch (wp) {
                    case 'Z': doUndo(); return 0;
                    case 'Y': doRedo(); return 0;
                    case 'C': doCopyAll(); return 0;
                    case 'V': doPaste(); return 0;
                    case 'N': PostMessageW(hwnd, WM_COMMAND, IDM_NEW, 0); return 0;
                    case 'O': PostMessageW(hwnd, WM_COMMAND, IDM_OPEN, 0); return 0;
                    case 'S':
                        PostMessageW(hwnd, WM_COMMAND,
                                     shift ? IDM_SAVEAS : IDM_SAVE, 0);
                        return 0;
                    case 'F': PostMessageW(hwnd, WM_COMMAND, IDM_FIND, 0); return 0;
                    case 'G': PostMessageW(hwnd, WM_COMMAND, IDM_JUMP, 0); return 0;
                    case VK_HOME: moveCursorFileStart(); ensureCursorVisible(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
                    case VK_END:  moveCursorFileEnd();   ensureCursorVisible(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
                }
            }

            if (wp == VK_F3) {
                findNextFrom(g.cursor + 1);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            switch (wp) {
                case VK_LEFT:  moveCursor(-1, 1); break;
                case VK_RIGHT: moveCursor(+1, 1); break;
                case VK_UP:    moveCursor(-1, DPR); break;
                case VK_DOWN:  moveCursor(+1, DPR); break;
                case VK_HOME:  moveCursorLineStart(); break;
                case VK_END:   moveCursorLineEnd(); break;
                case VK_PRIOR: moveCursor(-1, (long long)DPR * 20); break;
                case VK_NEXT:  moveCursor(+1, (long long)DPR * 20); break;

                case VK_BACK:
                    if (g.cursor > 0 && totalDigits() > 0) {
                        ensureEditable();
                        beginEdit(true);
                        g.digits.erase(g.digits.begin() + (ptrdiff_t)(g.cursor - 1));
                        g.cursor--;
                        g.dirty = true;
                    }
                    break;
                case VK_DELETE:
                    if ((uint64_t)g.cursor < totalDigits()) {
                        ensureEditable();
                        beginEdit(true);
                        g.digits.erase(g.digits.begin() + (ptrdiff_t)g.cursor);
                        g.dirty = true;
                    }
                    break;
                case VK_INSERT:
                    g.insert = !g.insert;
                    g.message = g.insert ? "Insert mode" : "Overwrite mode";
                    break;
                default: break;
            }
            ensureCursorVisible();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_CHAR: {
            if (g.bar != App::Bar::None) {
                if (wp >= 32 && wp < 127) {
                    g.barText += (char)wp;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (g.genState == App::GenState::Running) return 0;
            if (wp >= '0' && wp <= '9') {
                if (totalDigits() >= MAX_BUFFER) {
                    g.message = "Buffer size limit reached";
                    return 0;
                }
                ensureEditable();
                beginEdit(true);
                if (g.insert) {
                    g.digits.insert(g.digits.begin() + (ptrdiff_t)g.cursor, (char)wp);
                    g.cursor++;
                } else {
                    if (g.cursor < g.digits.size()) g.digits[g.cursor] = (char)wp;
                    else g.digits.push_back((char)wp);
                    g.cursor++;
                }
                if (g.cursor > g.digits.size()) g.cursor = g.digits.size();
                g.dirty = true;
                g.cursorVisible = true;
                ensureCursorVisible();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (g.genState == App::GenState::Running) {
                if (id == IDM_EXIT) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            switch (id) {
                case IDM_NEW:    doNew(); break;
                case IDM_OPEN:   doOpen(); break;
                case IDM_SAVE:
                    if (g.lazyMode) { g.message = "Save: materializing..."; UpdateWindow(hwnd);
                                      ensureEditable(); }
                    if (g.path.empty()) doSaveAs();
                    else doSaveTo(g.path);
                    break;
                case IDM_SAVEAS:
                    if (g.lazyMode) { ensureEditable(); }
                    doSaveAs();
                    break;
                case IDM_EXIT:   PostMessageW(hwnd, WM_CLOSE, 0, 0); break;

                case IDM_UNDO:  doUndo(); break;
                case IDM_REDO:  doRedo(); break;
                case IDM_COPY:  doCopyAll(); break;
                case IDM_PASTE: doPaste(); break;

                case IDM_FIND:     showBar(App::Bar::Find, "Find: ", g.lastFind); break;
                case IDM_FINDNEXT: findNextFrom(g.cursor + 1); break;
                case IDM_JUMP:     showBar(App::Bar::Jump, "Jump to offset: "); break;

                case IDM_GEN_PI:
                case IDM_GEN_E:
                case IDM_GEN_LCG:
                case IDM_GEN_PATTERN:
                    g.pendingGen = id;
                    g.pendingGenCount = 0;
                    showBar(App::Bar::GenCount, "Count: ", "100000");
                    break;

                case IDM_INFO: {
                    char buf[640];
                    std::snprintf(buf, sizeof buf,
                        "lazy=%s\ncodec=%s\n"
                        "total digits=%llu\npayload=%zu B\nmode=%s\n"
                        "cursor=%zu (0..%llu)\nconstant-id=%d\n"
                        "cache blocks=%zu / %u\nstate=%s",
                        g.lazyMode ? "yes" : "no",
                        dx::kindName(g.file.kind),
                        (unsigned long long)totalDigits(),
                        g.file.payload.size(), g.mode.c_str(),
                        g.cursor, (unsigned long long)totalDigits(),
                        g.lastConstantId,
                        g.lazyMode ? g.blockReader.cachedBlockCount() : (size_t)0,
                        g.lazyMode ? g.blockReader.blockCount() : 0u,
                        g.dirty ? "modified" : "saved");
                    g.message = "Info shown";
                    MessageBoxA(hwnd, buf, "DX Studio - Info",
                                MB_OK | MB_ICONINFORMATION);
                    break;
                }
                case IDM_ABOUT:
                    MessageBoxW(hwnd,
                        L"DX Studio — DXFS editor\n\n"
                        L"Blocked (lazy) format: opens with only a few KB of RAM,\n"
                        L"decompressing only the digits that are on screen.\n\n"
                        L"File > Codec > Blocked  forces saving as .dx blocked format.\n"
                        L"Editing a lazy file materializes it into RAM.\n\n"
                        L"Ctrl+Z / Ctrl+Y  undo / redo\n"
                        L"Ctrl+C / Ctrl+V  copy all / paste\n"
                        L"Ctrl+O / Ctrl+S  open / save\n"
                        L"Ctrl+F / F3      find / find next\n"
                        L"Ctrl+G           jump to offset\n"
                        L"Ctrl+Home / End  file start / end\n"
                        L"Ins              toggle insert / overwrite",
                        L"About DX Studio", MB_OK | MB_ICONINFORMATION);
                    break;

                case IDM_MODE_AUTO:     g.mode = "auto";     g.message = "mode=auto";     break;
                case IDM_MODE_PACKED:   g.mode = "packed";   g.message = "mode=packed";   break;
                case IDM_MODE_RLE:      g.mode = "rle";      g.message = "mode=rle";      break;
                case IDM_MODE_PATTERN:  g.mode = "pattern";  g.message = "mode=pattern";  break;
                case IDM_MODE_CONSTANT: g.mode = "constant"; g.message = "mode=constant"; break;
                case IDM_MODE_BLOCKED:  g.mode = "blocked";  g.message = "mode=blocked";  break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_CLOSE:
            if (g.genState == App::GenState::Running) {
                int r = MessageBoxW(hwnd,
                    L"A generation is still running. Cancel it and quit?",
                    L"DX Studio", MB_YESNO | MB_ICONWARNING);
                if (r != IDYES) return 0;
                KillTimer(hwnd, TIMER_GEN);
                g.genState = App::GenState::Idle;
                g.gen.reset();
            }
            if (g.dirty) {
                int r = MessageBoxW(hwnd, L"Unsaved changes. Quit anyway?",
                                    L"DX Studio", MB_YESNO | MB_ICONWARNING);
                if (r != IDYES) return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_CARET);
            KillTimer(hwnd, TIMER_GEN);
            if (g.hFont) DeleteObject(g.hFont);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_IBEAM);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = APP_CLASS;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HMENU menu = buildMenu();
    HWND hwnd = CreateWindowExW(
        0, APP_CLASS, APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
        nullptr, menu, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}