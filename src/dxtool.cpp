#include "dx/format.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static std::string readText(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

static std::vector<uint8_t> readBin(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

static void writeBin(const std::string& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static void writeText(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p);
    f.write(s.data(), std::streamsize(s.size()));
}

static void usage() {
    std::puts(
        "dxtool — DX digit container utility\n"
        "\n"
        "  dxtool pack   <in.txt> <out.dx> [--mode auto|packed|rle|pattern|pi|e]\n"
        "  dxtool unpack <in.dx>  <out.txt>\n"
        "  dxtool info   <in.dx>\n"
        "  dxtool gen    <pi|e|lcg|pattern> <count> <out.dx> [seed|pattern]\n"
        "  dxtool check  <in.dx>\n");
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    std::string cmd = argv[1];

    try {
        if (cmd == "pack") {
            if (argc < 4) { usage(); return 1; }
            std::string mode = "auto";
            for (int i = 4; i < argc; ++i)
                if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];

            std::string digits = readText(argv[2]);
            // tolerate a trailing newline
            while (!digits.empty() && (digits.back() == '\n' || digits.back() == '\r'))
                digits.pop_back();

            dx::File f = dx::encode(digits, mode);
            auto bytes = dx::serialize(f);
            writeBin(argv[3], bytes);

            std::printf("codec=%s  digits=%llu  payload=%zu B  file=%zu B  %.4f bits/digit\n",
                        dx::kindName(f.kind),
                        (unsigned long long)f.count,
                        f.payload.size(), bytes.size(),
                        bytes.size() * 8.0 / double(f.count ? f.count : 1));
        }
        else if (cmd == "unpack") {
            auto bytes = readBin(argv[2]);
            std::string err;
            auto f = dx::deserialize(bytes, &err);
            if (!f) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
            writeText(argv[3], dx::materialize(*f));
            std::printf("wrote %llu digits to %s\n",
                        (unsigned long long)f->count, argv[3]);
        }
        else if (cmd == "info" || cmd == "check") {
            auto bytes = readBin(argv[2]);
            std::string err;
            auto f = dx::deserialize(bytes, &err);
            if (!f) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
            std::printf("file      : %s\n", argv[2]);
            std::printf("codec     : %s\n", dx::kindName(f->kind));
            std::printf("digits    : %llu\n", (unsigned long long)f->count);
            std::printf("payload   : %zu bytes\n", f->payload.size());
            std::printf("file size : %zu bytes\n", bytes.size());
            std::printf("density   : %.4f bits/digit\n",
                        bytes.size() * 8.0 / double(f->count ? f->count : 1));
            std::printf("vs ASCII  : %.1fx smaller\n",
                        double(f->count) / double(bytes.size() ? bytes.size() : 1));
            if (cmd == "check") std::puts("checksum  : OK");
        }
        else if (cmd == "gen") {
            if (argc < 5) { usage(); return 1; }
            std::string what = argv[2];
            uint64_t n = std::strtoull(argv[3], nullptr, 10);
            dx::File f;
            f.count = n;

            if (what == "pi" || what == "e") {
                f.kind = dx::Kind::Constant;
                f.payload = { uint8_t(what == "pi" ? 0 : 1) };
            } else if (what == "lcg") {
                uint64_t seed = (argc > 5) ? std::strtoull(argv[5], nullptr, 10)
                                           : 88172645463325252ULL;
                f.kind = dx::Kind::Lcg;
                for (int i = 0; i < 8; ++i) f.payload.push_back(uint8_t(seed >> (8 * i)));
            } else if (what == "pattern") {
                std::string pat = (argc > 5) ? argv[5] : "0123456789";
                if (!dx::isDigits(pat) || pat.empty())
                    throw std::runtime_error("pattern must be a non-empty digit string");
                f.kind = dx::Kind::Pattern;
                // build a sample so encode() picks the period
                std::string sample;
                sample.reserve(size_t(n));
                for (uint64_t i = 0; i < n; ++i) sample.push_back(pat[i % pat.size()]);
                f = dx::encode(sample, "pattern");
            } else {
                throw std::runtime_error("unknown generator: " + what);
            }

            auto bytes = dx::serialize(f);
            writeBin(argv[4], bytes);
            std::printf("generated %llu digits -> %zu bytes (%s)\n",
                        (unsigned long long)n, bytes.size(), dx::kindName(f.kind));
        }
        else { usage(); return 1; }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}