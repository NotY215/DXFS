#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace dx {

inline constexpr uint32_t BLOCK_SIZE_DEFAULT = 988;  // 52 * 19 digits
inline constexpr const char* BLOCK_MAGIC = "DXF2";

// Writer: feed digits in arbitrary chunks; get back a serialized DXF2 file.
class BlockWriter {
public:
    explicit BlockWriter(uint32_t blockSize = BLOCK_SIZE_DEFAULT);
    void append(const std::string& digits);
    std::vector<uint8_t> finish();

private:
    uint32_t blockSize_;
    std::string staging_;
    std::vector<std::vector<uint8_t>> blocks_;
    uint64_t count_ = 0;
    void flushStaging();
};

// Reader: random access with LRU block cache.
// Only the blocks overlapping a readRange() call are decompressed.
class BlockReader {
public:
    BlockReader() = default;

    bool open(const std::vector<uint8_t>& bytes, std::string* err = nullptr);
    bool isOpen() const { return open_; }

    uint64_t count()      const { return count_; }
    uint32_t blockSize()  const { return blockSize_; }
    uint32_t blockCount() const { return blockCount_; }

    // Read up to `len` digits starting at `start`. Only touching the
    // blocks that overlap [start, start+len).
    std::string readRange(uint64_t start, uint64_t len);

    void   setCacheCap(size_t blocks);
    void   clearCache();
    size_t cachedBlockCount() const { return cache_.size(); }

private:
    bool open_ = false;
    std::vector<uint8_t> bytes_;
    uint64_t count_ = 0;
    uint32_t blockSize_ = 0;
    uint32_t blockCount_ = 0;
    std::vector<uint64_t> offsets_;   // absolute file offset of each block

    struct CacheEntry { std::string digits; uint64_t lastUse = 0; };
    std::unordered_map<uint32_t, CacheEntry> cache_;
    uint64_t tick_ = 0;
    size_t cacheCap_ = 32;

    const std::string& getBlock(uint32_t idx);
};

} // namespace dx