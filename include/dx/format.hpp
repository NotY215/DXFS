#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dx {

    inline constexpr uint8_t VERSION = 1;
    inline constexpr size_t  HDR_SIZE = 32;

    enum class Kind : uint8_t {
        Packed   = 0,
        Rle      = 1,
        Constant = 2,
        Lcg      = 3,
        Pattern  = 4,
        Blocked  = 5,   // <-- new
    };

    // Exported so blockstore.cpp can reuse the base-10^19 packing.
    std::vector<uint8_t> encPackedExported(const std::string& d);
    std::string          decPackedExported(const std::vector<uint8_t>& p, uint64_t count);

    const char* kindName(Kind k);

    struct File {
        Kind                 kind  = Kind::Packed;
        uint64_t             count = 0;
        std::vector<uint8_t> payload;
    };

    bool isDigits(const std::string& s);

    // Container I/O
    std::vector<uint8_t> serialize(const File& f);
    std::optional<File>  deserialize(const std::vector<uint8_t>& bytes,
                                     std::string* err = nullptr);

    // Digits <-> source
    std::string materialize(const File& f);
    File        encode(const std::string& digits, const std::string& mode = "auto");

    // Raw generators (used by `:gen` and constant codecs)
    std::string constantDigits(uint8_t id, uint64_t count);
    std::string lcgDigits(uint64_t seed, uint64_t count);

} // namespace dx