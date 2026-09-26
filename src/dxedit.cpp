// dxedit — a small terminal editor for .dx files.
#include "dx/format.hpp"

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int DPR      = 50;   // digits per grid row
constexpr int LABEL_W  = 8;    // width of the offset label
constexpr int MIN_W    = 68;

enum {
    K_UP = -2, K_DOWN, K_LEFT, K_RIGHT,
    K_PGUP, K_PGDN, K_HOME, K_END, K_DEL, K_ESC = 27
};

struct Term {
    termios orig{};
    bool raw = false;

    Term() {
        if (tcgetattr(STDIN_FILENO, &orig) == 0) {
            termios r = orig;
            r.c_lflag &= ~(ECHO | ICANON | ISIG);
            r.c_iflag &= ~(IXON | ICRNL);
            r.c_cc[VMIN] = 1;
            r.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &r);
            raw = true;
        }
        std::fputs("\x1b[?1049h\x1b[?25l", stdout);
        std::fflush(stdout);
    }
    ~Term() {
        std::fputs("\x1b[?25h\x1b[?1049l", stdout);
        std::fflush(stdout);
        if (raw) tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    }
};

std::string pad(const std::string& s, int w) {
    if (int(s.size()) >= w) return s.substr(0, size_t(w));
    return s + std::string(size_t(w) - s.size(), ' ');
}

int readKey() {
    unsigned char c;
    if (::read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c != 0x1B) return c;

    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, 40) <= 0) return 0x1B;

    unsigned char seq[8];
    int n = int(::read(STDIN_FILENO, seq, sizeof(seq)));
    if (n <= 0) return 0x1B;

    if (seq[0] == '[' && n >= 2) {
        switch (seq[1]) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            case 'F': return K_END;
            case '5': return K_PGUP;
            case '6': return K_PGDN;
            case '3': return K_DEL;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------

struct Editor {
    std::string path;
    dx::File    file;
    std::string digits;
    std::string encMode = "auto";

    uint64_t cursor   = 0;
    uint64_t topLine  = 0;
    bool     insert   = false;
    bool     dirty    = false;
    bool     running  = true;
    bool     help     = false;

    std::string message = "dxedit — : for commands, ? for help";
    std::string cmd;
    bool        cmdMode = false;

    int W = 80, H = 24;

    // -------------------------------------------------------------- helpers
    void refreshSize() {
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            W = ws.ws_col; H = ws.ws_row;
        }
        if (W < MIN_W) W = MIN_W;
        if (H < 8)     H = 8;
    }

    int gridRows() const { return H - 3; }

    void clamp() {
        if (digits.empty()) { cursor = 0; topLine = 0; return; }
        if (cursor >= digits.size()) cursor = digits.size() - 1;
        uint64_t curLine = cursor / DPR;
        if (curLine < topLine) topLine = curLine;
        if (curLine >= topLine + uint64_t(gridRows()))
            topLine = curLine - uint64_t(gridRows()) + 1;
    }

    void centerCursor() {
        uint64_t curLine = digits.empty() ? 0 : cursor / DPR;
        uint64_t half = uint64_t(gridRows()) / 2;
        topLine = curLine > half ? curLine - half : 0;
        clamp();
    }

    // --------------------------------------------------------------- render
    void draw() {
        refreshSize();
        std::string o;
        o.reserve(size_t(W) * size_t(H) * 3 + 512);

        auto gotoRow = [&](int r) {
            o += "\x1b[" + std::to_string(r) + ";1H";
        };

        // title
        gotoRow(1);
        {
            std::string t = " dxedit  ";
            t += path.empty() ? std::string("[no name]") : path;
            if (dirty) t += " *";
            std::string right = std::string(" ") + dx::kindName(file.kind) + " ";
            if (int(t.size() + right.size()) < W)
                t += std::string(size_t(W) - t.size() - right.size(), ' ');
            t += right;
            o += "\x1b[7m" + pad(t, W) + "\x1b[0m\x1b[K";
        }

        if (help) {
            static const char* lines[] = {
                "  dxedit — help",
                "",
                "  movement     arrows, PgUp/PgDn, Home/End",
                "  0-9          type a digit at the cursor",
                "  i            toggle insert / overwrite",
                "  Backspace    delete digit before cursor",
                "  Del          delete digit at cursor",
                "",
                "  :w [file]    save (auto-picks the smallest codec)",
                "  :wq          save and quit          :q  quit   :q!  force quit",
                "  :mode M      force codec: auto|packed|rle|pattern",
                "  :gen pi N    generate N digits of pi (stored as a constant)",
                "  :gen e  N    generate N digits of e",
                "  :gen lcg N S generate N pseudo-random digits from seed S",
                "  :gen pattern N STR   repeat digit string STR",
                "  :info        show container statistics",
                "  :goto N      jump to digit offset N",
                "",
                "  ?            close this help",
            };
            int row = 2;
            for (const char* l : lines) {
                if (row >= H - 1) break;
                gotoRow(row++);
                o += pad(l, W) + "\x1b[K";
            }
            for (; row < H - 1; ++row) { gotoRow(row); o += std::string(size_t(W), ' ') + "\x1b[K"; }
        } else {
            uint64_t totalLines = (digits.size() + DPR - 1) / DPR;
            for (int r = 0; r < gridRows(); ++r) {
                gotoRow(2 + r);
                uint64_t line = topLine + uint64_t(r);
                std::string ln;
                if (line < totalLines) {
                    char lbl[24];
                    std::snprintf(lbl, sizeof lbl, "%08llu ",
                                  (unsigned long long)(line * DPR));
                    ln = lbl;
                    uint64_t start = line * DPR;
                    for (int i = 0; i < DPR; ++i) {
                        if (i && i % 5 == 0) ln += ' ';
                        uint64_t idx = start + uint64_t(i);
                        ln += (idx < digits.size()) ? digits[size_t(idx)] : ' ';
                    }
                }
                o += pad(ln, W) + "\x1b[K";
            }
        }

        // status
        gotoRow(H - 1);
        {
            char buf[256];
            double dens = digits.empty() ? 0.0
                         : double(digits.size()) * 0.0;  // filled below if saved
            (void)dens;
            std::snprintf(buf, sizeof buf,
                          " %llu/%llu  %s  %s  codec=%s  digits=%llu ",
                          (unsigned long long)(digits.empty() ? 0 : cursor),
                          (unsigned long long)digits.size(),
                          insert ? "INS" : "OVR",
                          dirty ? "modified" : "saved",
                          dx::kindName(file.kind),
                          (unsigned long long)file.count);
            o += "\x1b[7m" + pad(buf, W) + "\x1b[0m\x1b[K";
        }

        // command line
        gotoRow(H);
        {
            std::string line = cmdMode ? (":" + cmd + "_") : message;
            o += pad(line, W) + "\x1b[K";
        }

        // place the cursor
        int cr = 2, cc = 1;
        if (!help && !cmdMode && !digits.empty()) {
            uint64_t curLine = cursor / DPR;
            int row = 2 + int(curLine - topLine);
            uint64_t col = cursor % DPR;
            int x = LABEL_W + 2 + int(col) + int(col / 5);
            cr = std::max(2, std::min(row, H - 2));
            cc = std::max(1, std::min(x, W));
            o += "\x1b[?25h";
        } else if (cmdMode) {
            cr = H;
            cc = int(cmd.size()) + 2;
            o += "\x1b[?25h";
        } else {
            o += "\x1b[?25l";
        }
        o += "\x1b[" + std::to_string(cr) + ";" + std::to_string(cc) + "H";

        std::fwrite(o.data(), 1, o.size(), stdout);
        std::fflush(stdout);
    }

    // ------------------------------------------------------------------ I/O
    void load(const std::string& p) {
        std::ifstream in(p, std::ios::binary);
        if (!in) {
            path = p; digits.clear(); file = {};
            message = "new file: " + p;
            return;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
        std::string err;
        auto f = dx::deserialize(bytes, &err);
        if (!f) { message = "load error: " + err; return; }

        file = *f;
        if (file.kind == dx::Kind::Constant) {
            message = "materializing constant, please wait...";
            draw();
        }
        digits = dx::materialize(file);
        path = p;
        cursor = 0; topLine = 0; dirty = false;
        message = "loaded " + std::to_string(digits.size()) + " digits (" +
                  dx::kindName(file.kind) + ")";
    }

    void save(const std::string& p) {
        if (p.empty()) { message = "no file name"; return; }
        try {
            file = dx::encode(digits, encMode);
            auto bytes = dx::serialize(file);
            std::ofstream out(p, std::ios::binary);
            if (!out) { message = "cannot write " + p; return; }
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      std::streamsize(bytes.size()));
            path = p; dirty = false;
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "saved %zu digits as %s -> %zu bytes (%.4f bits/digit)",
                          digits.size(), dx::kindName(file.kind), bytes.size(),
                          bytes.size() * 8.0 / double(digits.empty() ? 1 : digits.size()));
            message = buf;
        } catch (const std::exception& e) {
            message = std::string("error: ") + e.what();
        }
    }

    void regenerate(const std::string& kind, uint64_t n, const std::string& arg) {
        if (n == 0) { message = "count must be > 0"; return; }
        message = "generating " + std::to_string(n) + " digits (" + kind + ")...";
        draw();

        dx::File f;
        f.count = n;
        if (kind == "pi" || kind == "e") {
            f.kind = dx::Kind::Constant;
            f.payload = { uint8_t(kind == "pi" ? 0 : 1) };
        } else if (kind == "lcg") {
            uint64_t seed = arg.empty() ? 88172645463325252ULL
                                        : std::strtoull(arg.c_str(), nullptr, 10);
            f.kind = dx::Kind::Lcg;
            for (int i = 0; i < 8; ++i) f.payload.push_back(uint8_t(seed >> (8 * i)));
        } else if (kind == "pattern") {
            std::string pat = arg.empty() ? std::string("0123456789") : arg;
            if (!dx::isDigits(pat)) { message = "pattern must be digits"; return; }
            std::string sample;
            sample.reserve(size_t(n));
            for (uint64_t i = 0; i < n; ++i) sample.push_back(pat[i % pat.size()]);
            f = dx::encode(sample, "pattern");
        } else {
            message = "unknown generator: " + kind;
            return;
        }

        file = f;
        digits = dx::materialize(file);
        cursor = 0; topLine = 0; dirty = true;
        message = "generated " + std::to_string(digits.size()) + " digits as " +
                  dx::kindName(file.kind);
    }

    // -------------------------------------------------------------- editing
    void typeDigit(char c) {
        if (insert) {
            if (digits.size() < 100000000ull) digits.insert(digits.begin() + long(cursor), c);
        } else {
            if (cursor < digits.size()) digits[size_t(cursor)] = c;
            else digits.push_back(c);
        }
        dirty = true;
        if (cursor + 1 < digits.size() + (insert ? 1 : 0)) cursor++;
        if (cursor >= digits.size() && !digits.empty()) cursor = digits.size() - 1;
        clamp();
    }

    void backspace() {
        if (digits.empty()) return;
        if (cursor > 0 && cursor <= digits.size()) {
            digits.erase(digits.begin() + long(cursor - 1));
            cursor--;
            dirty = true;
        } else if (cursor == 0 && !digits.empty()) {
            digits.erase(digits.begin());
            dirty = true;
        }
        clamp();
    }

    void deleteAt() {
        if (cursor < digits.size()) {
            digits.erase(digits.begin() + long(cursor));
            dirty = true;
        }
        clamp();
    }

    // ------------------------------------------------------------ commands
    void execCommand(const std::string& c) {
        std::istringstream is(c);
        std::string op;
        is >> op;

        if (op.empty()) return;

        if (op == "q") {
            if (dirty) message = "unsaved changes — use :q! to discard";
            else running = false;
        } else if (op == "q!") {
            running = false;
        } else if (op == "w") {
            std::string p; is >> p;
            save(p.empty() ? path : p);
        } else if (op == "wq") {
            std::string p; is >> p;
            save(p.empty() ? path : p);
            if (!dirty) running = false;
        } else if (op == "mode") {
            std::string m; is >> m;
            if (m == "auto" || m == "packed" || m == "rle" || m == "pattern") {
                encMode = m;
                message = "codec mode: " + m;
            } else {
                message = "mode must be auto|packed|rle|pattern";
            }
        } else if (op == "gen") {
            std::string kind; uint64_t n = 0;
            is >> kind >> n;
            std::string arg; is >> arg;
            regenerate(kind, n, arg);
        } else if (op == "goto") {
            uint64_t n = 0; is >> n;
            if (!digits.empty()) cursor = std::min<uint64_t>(n, digits.size() - 1);
            centerCursor();
        } else if (op == "info") {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "codec=%s  digits=%zu  stored=%llu  mode=%s  %s",
                          dx::kindName(file.kind), digits.size(),
                          (unsigned long long)file.count, encMode.c_str(),
                          dirty ? "(modified)" : "(saved)");
            message = buf;
        } else {
            message = "unknown command: " + op;
        }
    }

    // ---------------------------------------------------------------- keys
    void handleKey(int k) {
        if (cmdMode) {
            if (k == K_ESC) { cmdMode = false; cmd.clear(); }
            else if (k == '\r' || k == '\n') {
                cmdMode = false;
                std::string c = cmd;
                cmd.clear();
                execCommand(c);
            } else if (k == 127 || k == 8) {
                if (!cmd.empty()) cmd.pop_back();
                if (cmd.empty()) cmdMode = false;
            } else if (k >= 32 && k < 127) {
                cmd += char(k);
            }
            return;
        }

        if (help) {
            if (k == '?' || k == K_ESC || k == 'q') help = false;
            return;
        }

        uint64_t perPage = uint64_t(gridRows()) * DPR;

        if (k >= '0' && k <= '9') { typeDigit(char(k)); return; }

        switch (k) {
            case ':':  cmdMode = true; cmd.clear(); break;
            case '?':  help = true; break;
            case 'i':  insert = !insert; break;
            case 19:   save(path); break;               // Ctrl-S
            case 17:                                        // Ctrl-Q
                if (!dirty) running = false; else message = "unsaved changes — :q!";
                break;
            case K_LEFT:  if (cursor > 0) cursor--; clamp(); break;
            case K_RIGHT: if (cursor + 1 < digits.size()) cursor++; clamp(); break;
            case K_UP:    if (cursor >= DPR) cursor -= DPR; clamp(); break;
            case K_DOWN:  if (cursor + DPR < digits.size()) cursor += DPR; clamp(); break;
            case K_PGUP:  cursor = cursor > perPage ? cursor - perPage : 0; clamp(); break;
            case K_PGDN:  cursor = std::min<uint64_t>(cursor + perPage,
                                    digits.empty() ? 0 : digits.size() - 1); clamp(); break;
            case K_HOME:  cursor = 0; clamp(); break;
            case K_END:   if (!digits.empty()) cursor = digits.size() - 1; clamp(); break;
            case 127: case 8: backspace(); break;
            case K_DEL:   deleteAt(); break;
            default: break;
        }
    }

    void run() {
        while (running) {
            clamp();
            draw();
            int k = readKey();
            if (k == -1) continue;
            handleKey(k);
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    Term term;
    Editor ed;
    if (argc > 1) ed.load(argv[1]);
    ed.run();
    return 0;
}