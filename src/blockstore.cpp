#include "dx/blockstore.hpp"
#include "dx/format.hpp"

#include <algorithm>
#include <cstring>

namespace dx {

// File layout:
//   0   4   "DXF2"
//   4   1   version (2)
//   5   1   kind (5 = Blocked)
//   6   2   flags
//   8   8   count (total digits)
//  16   4   blockSize
//  20   4   blockCount
//  24   8   reserved
//  32   blockCount*8   absolute offset of each block payload
//  ...  block payloads (each is encPacked() of up to blockSize digits)

// ================================= writer =================================

BlockWriter::BlockWriter(uint32_t blockSize) : blockSize_(blockSize) {
    if (blockSize_ == 0) blockSize_ = BLOCK_SIZE_DEFAULT;
    staging_.reserve(blockSize_);
}

void BlockWriter::append(const std::string& digits) {
    size_t i = 0;
    while (i < digits.size()) {
        size_t space = blockSize_ - staging_.size();
        size_t take = (std::min)(space, digits.size() - i);
        staging_.append(digits, i, take);
        i += take;
        if (staging_.size() == blockSize_) flushStaging();
    }
}

void BlockWriter::flushStaging() {
    if (staging_.empty()) return;
    blocks_.push_back(encPackedExported(staging_));
    count_ += staging_.size();
    staging_.clear();
}

std::vector<uint8_t> BlockWriter::finish() {
    flushStaging();
    const uint32_t blockCount = (uint32_t)blocks_.size();
    const uint64_t indexBytes = (uint64_t)blockCount * 8;

    std::vector<uint8_t> out;
    out.reserve(32 + indexBytes + 4096);

    out.push_back('D'); out.push_back('X'); out.push_back('F'); out.push_back('2');
    out.push_back(2);
    out.push_back(5);            // Kind::Blocked
    out.push_back(0); out.push_back(0);

    auto put64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(v >> (8 * i))); };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i))); };

    put64(count_);
    put32(blockSize_);
    put32(blockCount);
    put64(0);

    uint64_t cur = 32 + indexBytes;
    for (const auto& b : blocks_) { put64(cur); cur += b.size(); }
    for (const auto& b : blocks_) out.insert(out.end(), b.begin(), b.end());
    return out;
}

// ================================= reader =================================

bool BlockReader::open(const std::vector<uint8_t>& bytes, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };

    if (bytes.size() < 32)                         return fail("file too short");
    if (std::memcmp(bytes.data(), "DXF2", 4) != 0) return fail("not a DXF2 file");
    if (bytes[4] != 2)                             return fail("unsupported DXF2 version");
    if (bytes[5] != 5)                             return fail("DXF2 kind is not Blocked");

    auto get64 = [&](size_t off) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= (uint64_t)bytes[off + i] << (8 * i);
        return v;
    };
    auto get32 = [&](size_t off) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (uint32_t)bytes[off + i] << (8 * i);
        return v;
    };

    uint64_t count      = get64(8);
    uint32_t blockSize  = get32(16);
    uint32_t blockCount = get32(20);

    if (blockSize == 0)  return fail("zero block size");
    if (blockCount == 0) return fail("zero blocks");

    uint64_t indexBytes = (uint64_t)blockCount * 8;
    if (32 + indexBytes > bytes.size()) return fail("truncated block index");

    std::vector<uint64_t> offsets(blockCount);
    for (uint32_t i = 0; i < blockCount; ++i) offsets[i] = get64(32 + (size_t)i * 8);

    if (offsets[0] != 32 + indexBytes) return fail("block 0 offset wrong");
    for (uint32_t i = 0; i < blockCount; ++i) {
        if (offsets[i] > bytes.size()) return fail("block offset past EOF");
        if (i > 0 && offsets[i] < offsets[i - 1]) return fail("block offsets not monotonic");
    }

    bytes_      = bytes;
    count_      = count;
    blockSize_  = blockSize;
    blockCount_ = blockCount;
    offsets_    = std::move(offsets);
    cache_.clear();
    tick_ = 0;
    open_ = true;
    return true;
}

void BlockReader::setCacheCap(size_t blocks) {
    cacheCap_ = blocks ? blocks : 1;
    while (cache_.size() > cacheCap_) {
        auto lru = std::min_element(cache_.begin(), cache_.end(),
            [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        cache_.erase(lru);
    }
}

void BlockReader::clearCache() { cache_.clear(); tick_ = 0; }

const std::string& BlockReader::getBlock(uint32_t idx) {
    auto it = cache_.find(idx);
    if (it != cache_.end()) {
        it->second.lastUse = ++tick_;
        return it->second.digits;
    }

    size_t start = (size_t)offsets_[idx];
    size_t end   = (idx + 1 < blockCount_) ? (size_t)offsets_[idx + 1] : bytes_.size();
    std::vector<uint8_t> payload(bytes_.begin() + (ptrdiff_t)start,
                                 bytes_.begin() + (ptrdiff_t)end);

    uint64_t blockStartDigit = (uint64_t)idx * blockSize_;
    uint64_t blockDigits     = (std::min)((uint64_t)blockSize_, count_ - blockStartDigit);
    std::string digits = decPackedExported(payload, blockDigits);

    if (cache_.size() >= cacheCap_) {
        auto lru = std::min_element(cache_.begin(), cache_.end(),
            [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        cache_.erase(lru);
    }
    auto [ins, _] = cache_.emplace(idx, CacheEntry{ std::move(digits), ++tick_ });
    return ins->second.digits;
}

std::string BlockReader::readRange(uint64_t start, uint64_t len) {
    if (!open_) return {};
    if (start >= count_) return {};
    if (start + len > count_) len = count_ - start;
    if (len == 0) return {};

    std::string out;
    out.reserve((size_t)len);

    uint64_t pos = start;
    uint64_t end = start + len;
    while (pos < end) {
        uint32_t idx = (uint32_t)(pos / blockSize_);
        uint64_t blockStart = (uint64_t)idx * blockSize_;
        uint64_t inBlock    = pos - blockStart;
        const std::string& blk = getBlock(idx);
        if (inBlock >= blk.size()) break;
        uint64_t canTake = (std::min)((uint64_t)blk.size() - inBlock, end - pos);
        out.append(blk, (size_t)inBlock, (size_t)canTake);
        pos += canTake;
    }
    return out;
}

} // namespace dx