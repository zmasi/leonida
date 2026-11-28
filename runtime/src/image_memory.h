#pragma once

// Memory for textures, handed out as ranges of larger blocks instead of one driver allocation each. Making and freeing
// an allocation is a kernel call that costs tens to hundreds of microseconds, where a range costs a map lookup; a title
// that streams textures makes and frees them all the time. Vulkan also guarantees only 4096 live allocations
// (maxMemoryAllocationCount).
//
// No Vulkan or Windows dependency: the caller allocates and frees the blocks (ImageMemoryPool's two callbacks), so the
// policy is checked on the CPU (test/image_memory_test.cpp). All the images of a block are optimal-tiled, so
// bufferImageGranularity, which separates linear from optimal resources, does not apply.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace leonida {

// The free space of one block: the smallest free piece that holds the request is used (best fit), and a released range
// is merged with the free pieces beside it.
class BlockRanges {
public:
    explicit BlockRanges(std::uint64_t size) : size_(size) { insert(0, size); }

    // False when no free piece holds `size` bytes at `alignment`. `reused`: the range starts in bytes some earlier range
    // of this block held, so it may hold that range's data.
    bool allocate(std::uint64_t size, std::uint64_t alignment, std::uint64_t& offset, bool& reused) {
        if (!size || !alignment || size > size_) return false;
        for (auto it = by_size_.lower_bound({size, 0}); it != by_size_.end(); ++it) {
            const auto [piece_size, piece] = *it;
            const std::uint64_t rem = piece % alignment;
            const std::uint64_t lead = rem ? alignment - rem : 0;
            if (lead > piece_size || size > piece_size - lead) continue;
            const std::uint64_t start = piece + lead;
            erase(piece, piece_size);
            insert(piece, lead);
            insert(start + size, piece_size - lead - size);
            offset = start;
            reused = start < touched_;
            touched_ = std::max(touched_, start + size);
            in_use_ += size;
            return true;
        }
        return false;
    }

    // Gives back a range allocate() returned.
    void release(std::uint64_t offset, std::uint64_t size) {
        if (!size) return;
        in_use_ -= size;
        auto it = by_offset_.lower_bound(offset);
        if (it != by_offset_.end() && offset + size == it->first) {
            const std::uint64_t next_size = it->second;
            by_size_.erase({next_size, it->first});
            it = by_offset_.erase(it);
            size += next_size;
        }
        if (it != by_offset_.begin()) {
            const auto previous = std::prev(it);
            const auto [start, previous_size] = *previous;
            if (start + previous_size == offset) {
                by_size_.erase({previous_size, start});
                by_offset_.erase(previous);
                offset = start;
                size += previous_size;
            }
        }
        insert(offset, size);
    }

    bool empty() const { return in_use_ == 0; }
    std::uint64_t size() const { return size_; }
    std::uint64_t in_use() const { return in_use_; }
    // The largest free piece, so a full block is passed over without a search.
    std::uint64_t largest() const { return by_size_.empty() ? 0 : by_size_.rbegin()->first; }

private:
    void insert(std::uint64_t offset, std::uint64_t size) {
        if (!size) return;
        by_offset_[offset] = size;
        by_size_.insert({size, offset});
    }
    void erase(std::uint64_t offset, std::uint64_t size) {
        by_offset_.erase(offset);
        by_size_.erase({size, offset});
    }

    std::uint64_t size_;
    std::uint64_t in_use_ = 0;
    std::uint64_t touched_ = 0;  // bytes below this were handed out before
    std::map<std::uint64_t, std::uint64_t> by_offset_;            // offset -> size
    std::set<std::pair<std::uint64_t, std::uint64_t>> by_size_;  // (size, offset)
};

// Blocks of device memory, by memory type, split into ranges. Thread-safe. Every Range must be gone before the pool is.
template<typename Memory>
class ImageMemoryPool {
public:
    // A null Memory{}: the driver has no room in the type.
    using AllocateFn = std::function<Memory(std::uint32_t type, std::uint64_t bytes)>;
    using FreeFn = std::function<void(Memory)>;

    // One image's range. Destroying it gives the bytes back to the block, so it is destroyed when no submission that
    // used the image is still running (DeferredDeletionQueue).
    class Range {
    public:
        Range(const Range&) = delete;
        Range& operator=(const Range&) = delete;
        ~Range() { pool_.release(*this); }

        Memory memory() const { return memory_; }
        std::uint64_t offset() const { return offset_; }
        // The bytes may hold what an earlier range left; the memory of a new block was never given to one.
        bool reused() const { return reused_; }

    private:
        friend class ImageMemoryPool;
        Range(ImageMemoryPool& pool, void* block, Memory memory, std::uint64_t offset, std::uint64_t size, bool reused)
            : pool_(pool), block_(block), memory_(memory), offset_(offset), size_(size), reused_(reused) {}

        ImageMemoryPool& pool_;
        void* block_;
        Memory memory_;
        std::uint64_t offset_;
        std::uint64_t size_;
        bool reused_;
    };

    struct Stats {
        std::uint64_t blocks = 0;       // live driver allocations
        std::uint64_t block_bytes = 0;  // their size
        std::uint64_t ranges = 0;       // live ranges in them
    };

    ImageMemoryPool(AllocateFn allocate, FreeFn free) : allocate_(std::move(allocate)), free_(std::move(free)) {}
    ImageMemoryPool(const ImageMemoryPool&) = delete;
    ImageMemoryPool& operator=(const ImageMemoryPool&) = delete;
    ~ImageMemoryPool() {
        for (auto& [type, pool] : types_)
            for (const auto& block : pool.blocks) free_(block->memory);
    }

    // A range of a block of `type`, in a new block of `block_bytes` when no block has room. Null when the driver has no
    // room for that block; the caller then makes its own allocation. Driver callbacks (`allocate_` / `free_`) always
    // run outside `mutex_` so slow kernel allocations never block concurrent releases or allocations on other blocks.
    std::unique_ptr<Range> allocate(std::uint32_t type, std::uint64_t size, std::uint64_t alignment,
                                    std::uint64_t block_bytes) {
        if (!size || !alignment) return nullptr;
        std::uint64_t offset = 0;
        bool reused = false;
        std::unique_lock lock(mutex_);
        {
            Type& pool = types_[type];
            for (const auto& block : pool.blocks) {
                if (block->ranges.largest() < size || !block->ranges.allocate(size, alignment, offset, reused)) continue;
                ++ranges_;
                return std::unique_ptr<Range>(new Range(*this, block.get(), block->memory, offset, size, reused));
            }
            // A failed driver allocation is slow, and a full heap stays full until something is given back: do not ask
            // for another block at every texture until a range is released, or that many textures later.
            if (size > block_bytes || (pool.skip && pool.skip--)) return nullptr;
        }
        lock.unlock();
        const Memory memory = allocate_(type, block_bytes);
        if (!memory) {
            lock.lock();
            types_[type].skip = kRetryAfter;
            return nullptr;
        }
        std::unique_ptr<Block> block;
        try {
            block = std::make_unique<Block>(type, memory, block_bytes);
        } catch (...) {
            free_(memory);
            throw;
        }
        if (!block->ranges.allocate(size, alignment, offset, reused)) {
            free_(memory);
            return nullptr;
        }
        lock.lock();
        Type& pool = types_[type];
        ++ranges_;
        Block* raw = block.get();
        pool.blocks.push_back(std::move(block));
        return std::unique_ptr<Range>(new Range(*this, raw, memory, offset, size, reused));
    }

    Stats stats() const {
        std::lock_guard lock(mutex_);
        Stats result;
        result.ranges = ranges_;
        for (const auto& [type, pool] : types_)
            for (const auto& block : pool.blocks) {
                ++result.blocks;
                result.block_bytes += block->ranges.size();
            }
        return result;
    }

private:
    static constexpr std::uint32_t kRetryAfter = 64;

    struct Block {
        Block(std::uint32_t t, Memory m, std::uint64_t bytes) : type(t), memory(m), ranges(bytes) {}
        std::uint32_t type;
        Memory memory;
        BlockRanges ranges;
    };
    struct Type {
        std::vector<std::unique_ptr<Block>> blocks;
        std::uint32_t skip = 0;
    };

    void release(const Range& range) {
        Memory dead{};
        bool free_block = false;
        {
            std::lock_guard lock(mutex_);
            Block& block = *static_cast<Block*>(range.block_);
            Type& pool = types_[block.type];
            block.ranges.release(range.offset_, range.size_);
            pool.skip = 0;
            --ranges_;
            // One empty block is kept, so a title that frees and makes the same textures does not ask the driver each
            // time.
            if (block.ranges.empty() && std::count_if(pool.blocks.begin(), pool.blocks.end(),
                                                      [](const auto& b) { return b->ranges.empty(); }) > 1) {
                dead = block.memory;
                free_block = true;
                std::erase_if(pool.blocks, [&](const auto& b) { return b.get() == &block; });
            }
        }
        if (free_block) free_(dead);
    }

    mutable std::mutex mutex_;
    AllocateFn allocate_;
    FreeFn free_;
    std::map<std::uint32_t, Type> types_;
    std::uint64_t ranges_ = 0;
};

}  // namespace leonida
