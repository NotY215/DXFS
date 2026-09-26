#include "dx/format.hpp"
#include "dx/bigint.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dx {

// ------------------------------------------------------------------ helpers
static uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static void putVarint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) { out.push_back(uint8_t(v) | 0x80); v >>= 7; }
    out.push_back(uint8_t(v));
}

static uint64_t getVarint(const std::vector<uint8_t>& in, size_t& pos) {
    uint64_t v = 0; int shift = 0;
    while (pos < in.size() && shift < 64) {
        uint8_t b = in[pos++];
        v |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

    const char* kindName(Kind k) {
    switch (k) {
        case Kind::Packed:   return "packed";
        case Kind::Rle:      return "rle";
        case Kind::Constant: return "constant";
        case Kind::Lcg:      return "lcg";
        case Kind::Pattern:  return "pattern";
        case Kind::Blocked:  return "blocked";   // <-- new
    }
    return "?";
}
    // forward declarations for the file-local packers
    static std::vector<uint8_t> encPacked(const std::string& d);
    static std::string          decPacked(const std::vector<uint8_t>& p, uint64_t count);
    // thin public wrappers around the file-local static helpers
    std::vector<uint8_t> encPackedExported(const std::string& d) { return encPacked(d); }
    std::string          decPackedExported(const std::vector<uint8_t>& p, uint64_t count) {
    return decPacked(p, count);
}

bool isDigits(const std::string& s) {
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// ------------------------------------------------------------------- packed
// 19 decimal digits fit in a uint64 (10^19 < 2^64). Blocks are packed
// left-to-right; a trailing partial block is still padded to 8 bytes.
static std::vector<uint8_t> encPacked(const std::string& d) {
    std::vector<uint8_t> out;
    out.reserve((d.size() / 19 + 1) * 8);
    for (size_t i = 0; i < d.size(); i += 19) {
        size_t take = std::min<size_t>(19, d.size() - i);
        uint64_t v = 0;
        for (size_t j = 0; j < take; ++j) v = v * 10 + uint64_t(d[i + j] - '0');
        for (int b = 0; b < 8; ++b) out.push_back(uint8_t((v >> (8 * b)) & 0xFF));
    }
    return out;
}

static std::string decPacked(const std::vector<uint8_t>& p, uint64_t count) {
    std::string out;
    out.reserve(size_t(count));
    size_t pos = 0;
    uint64_t rem = count;
    while (rem > 0) {
        if (pos + 8 > p.size()) break;
        uint64_t v = 0;
        for (int b = 0; b < 8; ++b) v |= uint64_t(p[pos + b]) << (8 * b);
        pos += 8;
        size_t take = size_t(std::min<uint64_t>(19, rem));
        char blk[19];
        for (int i = 18; i >= 0; --i) { blk[i] = char('0' + int(v % 10)); v /= 10; }
        out.append(blk + (19 - take), take);
        rem -= take;
    }
    return out;
}

// ---------------------------------------------------------------------- rle
static std::vector<uint8_t> encRle(const std::string& d) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < d.size()) {
        char c = d[i];
        size_t j = i;
        while (j < d.size() && d[j] == c) ++j;
        out.push_back(uint8_t(c - '0'));
        putVarint(out, j - i);
        i = j;
    }
    return out;
}

static std::string decRle(const std::vector<uint8_t>& p, uint64_t count) {
    std::string out;
    out.reserve(size_t(count));
    size_t pos = 0;
    while (out.size() < count && pos < p.size()) {
        uint8_t dig = p[pos++];
        uint64_t run = getVarint(p, pos);
        if (dig > 9) break;
        for (uint64_t i = 0; i < run && out.size() < count; ++i)
            out.push_back(char('0' + dig));
    }
    return out;
}

// ------------------------------------------------------------------ pattern
// Smallest p such that d[i] == d[i+p]; via the KMP prefix function.
static uint64_t smallestPeriod(const std::string& d) {
    size_t n = d.size();
    if (n == 0) return 0;
    std::vector<size_t> pi(n, 0);
    for (size_t i = 1; i < n; ++i) {
        size_t j = pi[i - 1];
        while (j > 0 && d[i] != d[j]) j = pi[j - 1];
        if (d[i] == d[j]) ++j;
        pi[i] = j;
    }
    return n - pi[n - 1];
}

static std::vector<uint8_t> encPattern(const std::string& d) {
    std::vector<uint8_t> out;
    uint64_t p = smallestPeriod(d);
    putVarint(out, p);
    for (uint64_t i = 0; i < p && i < d.size(); ++i) out.push_back(uint8_t(d[i]));
    return out;
}

static std::string decPattern(const std::vector<uint8_t>& p, uint64_t count) {
    size_t pos = 0;
    uint64_t period = getVarint(p, pos);
    std::string pat;
    for (uint64_t i = 0; i < period && pos < p.size(); ++i)
        pat.push_back(char(p[pos++]));
    if (pat.empty()) return std::string(size_t(count), '0');
    std::string out;
    out.reserve(size_t(count));
    for (uint64_t i = 0; i < count; ++i) out.push_back(pat[i % pat.size()]);
    return out;
}

// ---------------------------------------------------------------------- lcg
static uint64_t xorshift64star(uint64_t& s) {
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return s * 2685821657736338717ULL;
}

std::string lcgDigits(uint64_t seed, uint64_t count) {
    if (seed == 0) seed = 88172645463325252ULL;
    std::string out;
    out.reserve(size_t(count));
    for (uint64_t i = 0; i < count; ++i)
        out.push_back(char('0' + int(xorshift64star(seed) % 10)));
    return out;
}

// ---------------------------------------------------------------- constants
std::string constantDigits(uint8_t id, uint64_t count) {
    if (count == 0) return "";
    if (id == 0) return piScaled(size_t(count) - 1).toDecimal(size_t(count));
    if (id == 1) return eScaled(size_t(count) - 1).toDecimal(size_t(count));
    return std::string(size_t(count), '0');
}

// ------------------------------------------------------------- container I/O
std::vector<uint8_t> serialize(const File& f) {
    std::vector<uint8_t> out;
    out.reserve(HDR_SIZE + f.payload.size());
    out.push_back('D'); out.push_back('X'); out.push_back('F'); out.push_back('1');
    out.push_back(VERSION);
    out.push_back(uint8_t(f.kind));
    out.push_back(0); out.push_back(0);                     // flags
    auto put64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    put64(f.count);
    put64(f.payload.size());
    put64(fnv1a(f.payload.data(), f.payload.size()));
    out.insert(out.end(), f.payload.begin(), f.payload.end());
    return out;
}

std::optional<File> deserialize(const std::vector<uint8_t>& b, std::string* err) {
    auto fail = [&](const char* m) -> std::optional<File> {
        if (err) *err = m;
        return std::nullopt;
    };
    if (b.size() < HDR_SIZE)                 return fail("file too short");
    if (std::memcmp(b.data(), "DXF1", 4))    return fail("bad magic");
    if (b[4] != VERSION)                     return fail("unsupported version");

    auto get64 = [&](size_t off) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(b[off + i]) << (8 * i);
        return v;
    };

    File f;
    f.kind  = Kind(b[5]);
    f.count = get64(8);
    uint64_t plen = get64(16);
    uint64_t csum = get64(24);

    if (b.size() < HDR_SIZE + plen)          return fail("truncated payload");
    f.payload.assign(b.begin() + HDR_SIZE, b.begin() + HDR_SIZE + plen);
    if (fnv1a(f.payload.data(), f.payload.size()) != csum)
        return fail("checksum mismatch");
    return f;
}

    std::string materialize(const File& f) {
        switch (f.kind) {
            case Kind::Packed:   return decPacked(f.payload, f.count);
            case Kind::Rle:      return decRle(f.payload, f.count);
            case Kind::Pattern:  return decPattern(f.payload, f.count);
            case Kind::Constant:
                return constantDigits(f.payload.empty() ? 0 : f.payload[0], f.count);
            case Kind::Lcg: {
                uint64_t s = 0;
                for (size_t i = 0; i < 8 && i < f.payload.size(); ++i)
                    s |= uint64_t(f.payload[i]) << (8 * i);
                return lcgDigits(s, f.count);
            }
            case Kind::Blocked:
                // Blocked files must be opened with dx::BlockReader, not materialized
                // in one shot. Return empty to signal "use the reader".
                return {};
        }
        return {};
    }

File encode(const std::string& digits, const std::string& mode) {
    if (!isDigits(digits))
        throw std::runtime_error("input contains non-digit characters");

    const size_t n = digits.size();

    auto makeConstant = [n](uint8_t id) -> File {
        File f;
        f.kind = Kind::Constant;
        f.count = n;
        f.payload.push_back(id);
        return f;
        };

    // ---- explicit codec modes ----
    if (mode == "packed") {
        File f; f.kind = Kind::Packed;  f.count = n; f.payload = encPacked(digits);  return f;
    }
    if (mode == "rle") {
        File f; f.kind = Kind::Rle;     f.count = n; f.payload = encRle(digits);     return f;
    }
    if (mode == "pattern") {
        File f; f.kind = Kind::Pattern; f.count = n; f.payload = encPattern(digits); return f;
    }
    if (mode == "pi" || mode == "e") {
        uint8_t id = (mode == "pi") ? uint8_t(0) : uint8_t(1);
        if (constantDigits(id, n) != digits)
            throw std::runtime_error("digits do not match " + mode);
        return makeConstant(id);
    }
    if (mode == "constant") {
        if (constantDigits(0, n) == digits) return makeConstant(0);
        if (constantDigits(1, n) == digits) return makeConstant(1);
        throw std::runtime_error("digits are not pi or e; use another codec");
    }

    // ---- auto: hardcoded 32-digit prefixes for pi and e ----
    static const char PI32[] = "31415926535897932384626433832795";
    static const char E32[] = "27182818284590452353602874713527";

    if (n >= 32) {
        for (int i = 0; i < 2; ++i) {
            const char* pfx = (i == 0) ? PI32 : E32;
            if (std::memcmp(digits.data(), pfx, 32) != 0) continue;
            if (constantDigits((uint8_t)i, n) == digits)
                return makeConstant((uint8_t)i);
            break;
        }
    }

    // ---- auto: pick smallest of the three general codecs ----
    File a{ Kind::Packed,  n, encPacked(digits) };
    File b{ Kind::Rle,     n, encRle(digits) };
    File c{ Kind::Pattern, n, encPattern(digits) };
    const File* best = &a;
    if (b.payload.size() < best->payload.size()) best = &b;
    if (c.payload.size() < best->payload.size()) best = &c;
    return *best;
}
} // namespace dx