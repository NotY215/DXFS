#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace dx {

// Minimal unsigned big integer, base 10^9 limbs (little-endian).
class Big {
public:
    static constexpr uint32_t BASE = 1000000000u;
    std::vector<uint32_t> L;

    Big() = default;
    explicit Big(uint64_t v) { while (v) { L.push_back(uint32_t(v % BASE)); v /= BASE; } }

    bool isZero() const { return L.empty(); }
    void trim() { while (!L.empty() && L.back() == 0) L.pop_back(); }

    static int cmp(const Big& a, const Big& b) {
        if (a.L.size() != b.L.size()) return a.L.size() < b.L.size() ? -1 : 1;
        for (size_t i = a.L.size(); i-- > 0;)
            if (a.L[i] != b.L[i]) return a.L[i] < b.L[i] ? -1 : 1;
        return 0;
    }

    Big& add(const Big& o) {
        size_t n = std::max(L.size(), o.L.size());
        L.resize(n, 0);
        uint64_t carry = 0;
        for (size_t i = 0; i < n; ++i) {
            uint64_t s = carry + L[i] + (i < o.L.size() ? o.L[i] : 0u);
            L[i] = uint32_t(s % BASE);
            carry = s / BASE;
        }
        if (carry) L.push_back(uint32_t(carry));
        return *this;
    }

    // requires *this >= o
    Big& sub(const Big& o) {
        int64_t borrow = 0;
        for (size_t i = 0; i < L.size(); ++i) {
            int64_t s = int64_t(L[i]) - borrow - (i < o.L.size() ? int64_t(o.L[i]) : 0);
            if (s < 0) { s += BASE; borrow = 1; } else borrow = 0;
            L[i] = uint32_t(s);
        }
        trim();
        return *this;
    }

    Big& mulSmall(uint64_t m) {
        if (m == 0 || isZero()) { L.clear(); return *this; }
        uint64_t carry = 0;
        for (size_t i = 0; i < L.size(); ++i) {
            uint64_t p = uint64_t(L[i]) * m + carry;
            L[i] = uint32_t(p % BASE);
            carry = p / BASE;
        }
        while (carry) { L.push_back(uint32_t(carry % BASE)); carry /= BASE; }
        return *this;
    }

    uint32_t divSmall(uint64_t m) {
        uint64_t rem = 0;
        for (size_t i = L.size(); i-- > 0;) {
            uint64_t cur = rem * BASE + L[i];
            L[i] = uint32_t(cur / m);
            rem = cur % m;
        }
        trim();
        return uint32_t(rem);
    }

    std::string toDecimal(size_t width = 0) const {
        if (L.empty()) return std::string(width, '0');
        std::string s = std::to_string(L.back());
        for (size_t i = L.size() - 1; i-- > 0;) {
            std::string part = std::to_string(L[i]);
            s += std::string(9 - part.size(), '0');
            s += part;
        }
        if (s.size() < width) s = std::string(width - s.size(), '0') + s;
        return s;
    }
};

inline Big pow10(size_t n) {
    Big r(1);
    while (n >= 9) { r.mulSmall(1000000000u); n -= 9; }
    if (n) { uint64_t m = 1; while (n--) m *= 10; r.mulSmall(m); }
    return r;
}

// floor(arctan(1/x) * 10^digits)  -- fixed point, truncated series
inline Big arctanInv(uint64_t x, size_t digits) {
    Big term = pow10(digits);
    term.divSmall(x);
    Big sum = term;
    const uint64_t x2 = x * x;
    uint64_t n = 1;
    while (!term.isZero()) {
        term.divSmall(x2);
        if (term.isZero()) break;
        Big t = term;
        t.divSmall(2 * n + 1);
        if (t.isZero()) break;
        if (n & 1) sum.sub(t); else sum.add(t);
        ++n;
    }
    return sum;
}

// Machin: pi = 16*atan(1/5) - 4*atan(1/239)
inline Big piScaled(size_t k) {
    const size_t guard = 24;
    size_t g = k + guard;
    Big a = arctanInv(5, g);   a.mulSmall(16);
    Big b = arctanInv(239, g); b.mulSmall(4);
    a.sub(b);
    for (size_t i = 0; i < guard; ++i) a.divSmall(10);
    return a;
}

// e = sum 1/k!
inline Big eScaled(size_t k) {
    const size_t guard = 24;
    size_t g = k + guard;
    Big scale = pow10(g);
    Big sum = scale, term = scale;
    uint64_t n = 1;
    while (true) {
        term.divSmall(n);
        if (term.isZero()) break;
        sum.add(term);
        ++n;
    }
    for (size_t i = 0; i < guard; ++i) sum.divSmall(10);
    return sum;
}

} // namespace dx