// leonida::TitleVaAliases (src/title_va_alias.h): where an address inside a title-placed buffer (Xbox device slot
// 133) is translated to, and that everything else is left alone. No GPU.
#include "../src/title_va_alias.h"

#include <cstdint>
#include <cstdio>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

int main() {
    leonida::TitleVaAliases a;
    constexpr std::uint64_t title = 0x20000000000ull;  // a synthetic user-mode VA far above 4 GiB
    constexpr std::uint64_t size = 0x10000;
    constexpr std::uint64_t arena = 0x2340000;
    // Empty: identity everywhere.
    EXPECT(a.size() == 0 && a.shader_va(title) == title && a.shader_va(0) == 0);
    EXPECT(!a.add(title, 0, arena));                  // empty range
    EXPECT(!a.add(UINT64_MAX - 4, 16, arena));        // wraps
    EXPECT(a.add(title, size, arena) && a.size() == 1);
    // Inside: the same offset in the copy, first and last byte included.
    EXPECT(a.shader_va(title) == arena);
    EXPECT(a.shader_va(title + 0x123) == arena + 0x123);
    EXPECT(a.shader_va(title + size - 1) == arena + size - 1);
    // Negative controls: one byte before, the end itself, and an arena VA are untouched.
    EXPECT(a.shader_va(title - 1) == title - 1);
    EXPECT(a.shader_va(title + size) == title + size);
    EXPECT(a.shader_va(arena) == arena);
    // The free span around an address: between the ranges, so a window made for it never covers one.
    std::uint64_t lo = 1, hi = 1;
    a.gap(title + size + 5, lo, hi);
    EXPECT(lo == title + size && hi == UINT64_MAX);
    a.gap(title - 5, lo, hi);
    EXPECT(lo == 0 && hi == title);
    // Without a device's resolver, resolve only translates what was added, and counts it.
    EXPECT(a.resolve(title + 4, 4, leonida::TitleVaAliases::root_view) == arena + 4);
    EXPECT(a.resolve(title - 4, 4, leonida::TitleVaAliases::root_view) == title - 4);
    EXPECT(a.translated(leonida::TitleVaAliases::root_view) == 1);
    a.note(title + 8, leonida::TitleVaAliases::vertex_view);
    a.note(title - 8, leonida::TitleVaAliases::vertex_view);
    EXPECT(a.untranslated(leonida::TitleVaAliases::vertex_view) == 1);
    // The title's memory: a commit inside a reservation is one range; outside every range is not the title's.
    std::uint64_t mlo = 0, mhi = 0;
    a.add_title_memory(0x40000000, 0x100000);
    a.add_title_memory(0x40010000, 0x1000);
    EXPECT(a.title_memory(0x40080000, mlo, mhi) && mlo == 0x40000000 && mhi == 0x40100000);
    EXPECT(!a.title_memory(0x40100000, mlo, mhi) && !a.title_memory(0x3FFFFFFF, mlo, mhi));
    a.remove_title_memory(0x40000000);
    EXPECT(!a.title_memory(0x40080000, mlo, mhi));
    // A wrapping target (an import below heap_base) is kept as is: modular arithmetic.
    constexpr std::uint64_t below = 0xFFFFFFFFFFF00000ull;
    EXPECT(a.add(title + 0x100000, size, below) && a.shader_va(title + 0x100010) == below + 0x10);
    std::uint64_t source{}, available{};
    EXPECT(a.source_of_vsharp((below + 0x10) & ((1ull << 48) - 1), source, available));
    EXPECT(source == title + 0x100010 && available == size - 0x10);
    EXPECT(!a.source_of_vsharp((below + size) & ((1ull << 48) - 1), source, available));
    // A new buffer over part of a live range replaces it; the old range's removal does not touch the new one.
    EXPECT(a.add(title + 0x8000, size, 0x5000000));
    EXPECT(a.shader_va(title) == title);  // the replaced range is gone entirely
    EXPECT(a.shader_va(title + 0x8000) == 0x5000000);
    a.remove(title, arena);  // stale: nothing starts there any more
    EXPECT(a.shader_va(title + 0x8000) == 0x5000000);
    a.remove(title + 0x8000, 0x1234);  // another target: kept
    EXPECT(a.shader_va(title + 0x8000) == 0x5000000);
    a.remove(title + 0x8000, 0x5000000);
    EXPECT(a.shader_va(title + 0x8000) == title + 0x8000);
    a.remove(title + 0x100000, below);
    EXPECT(a.size() == 0 && a.shader_va(title + 0x100010) == title + 0x100010);
    // range_of: what a BVH build checks its bytes against (the whole range must lie in one copy), and mappings, the
    // table a TLAS build translates its instances' BLAS addresses with.
    std::uint64_t to = 0, room = 0;
    std::vector<leonida::TitleVaAliases::Mapping> table;
    a.mappings(table);
    EXPECT(table.empty() && !a.range_of(title, to, room));
    EXPECT(a.add(title + 0x40000, size, 0x7000000) && a.add(title, size, arena));
    EXPECT(a.range_of(title + 0x100, to, room) && to == arena + 0x100 && room == size - 0x100);
    EXPECT(a.range_of(title + size - 1, to, room) && to == arena + size - 1 && room == 1);
    EXPECT(!a.range_of(title + size, to, room) && !a.range_of(title - 1, to, room));
    EXPECT(a.resolve(title + 0x40010, 64, leonida::TitleVaAliases::bvh_address) == 0x7000010);
    EXPECT(a.translated(leonida::TitleVaAliases::bvh_address) == 1);
    a.mappings(table);
    EXPECT(table.size() == 2 && table[0].title_va == title && table[0].end == title + size &&
           table[0].shader_va == arena && table[1].title_va == title + 0x40000 && table[1].shader_va == 0x7000000);
    a.remove(title, arena);
    a.remove(title + 0x40000, 0x7000000);
    EXPECT(a.size() == 0);
    // Every listener is told each change as it happens (the console heaps' slot records): an add, the whole old
    // interval when part of it is replaced, a removal, a copy forgotten and title memory added. None is dropped, however
    // many come (the 4,096-entry ring this replaced fell back to a full sweep), and a removed listener hears nothing.
    struct Heard {
        leonida::TitleVaAliases::Change change;
        std::uint64_t title_va, bytes, shader_va;
    };
    std::vector<Heard> first, second;
    const auto hear = [](void* to, leonida::TitleVaAliases::Change change, std::uint64_t va, std::uint64_t bytes,
                         std::uint64_t shader_va) {
        static_cast<std::vector<Heard>*>(to)->push_back({change, va, bytes, shader_va});
    };
    using Change = leonida::TitleVaAliases::Change;
    a.add_listener(&first, hear);
    a.add_listener(&second, hear);
    EXPECT(a.add(title, size, arena));
    EXPECT(a.add(title + 16, 32, arena + 4096));
    for (const auto* heard : {&first, &second}) {
        EXPECT(heard->size() == 3);
        EXPECT((*heard)[0].change == Change::added && (*heard)[0].title_va == title && (*heard)[0].bytes == size);
        EXPECT((*heard)[1].change == Change::removed && (*heard)[1].title_va == title && (*heard)[1].bytes == size &&
               (*heard)[1].shader_va == arena);
        EXPECT((*heard)[2].change == Change::added && (*heard)[2].title_va == title + 16 &&
               (*heard)[2].bytes == 32 && (*heard)[2].shader_va == arena + 4096);
    }
    a.remove_listener(&second);
    a.remove(title + 16, arena + 4096);
    a.forget_copy(title + 0x1000, 0x100, 0x5000000);
    a.add_title_memory(0x30000000000ull, 0x2000);
    EXPECT(second.size() == 3 && first.size() == 6);
    EXPECT(first[3].change == Change::removed && first[3].title_va == title + 16 && first[3].shader_va == arena + 4096);
    EXPECT(first[4].change == Change::removed && first[4].title_va == title + 0x1000 && first[4].bytes == 0x100 &&
           first[4].shader_va == 0x5000000);
    EXPECT(first[5].change == Change::memory_added && first[5].title_va == 0x30000000000ull && first[5].bytes == 0x2000);
    a.remove_title_memory(0x30000000000ull);
    for (unsigned i = 0; i < 5000; ++i) a.forget_copy(title, 4, arena);
    EXPECT(first.size() == 5006);
    a.remove_listener(&first);
    // shader_side: whether an address is one the layer wrote for the title (inside some range's copy), so a scan that
    // sees an already rewritten V# again leaves it alone rather than taking it for an unmapped address.
    EXPECT(!a.shader_side(arena));
    EXPECT(a.add(title, size, arena) && a.add_twin(title + 0x200000, title + 0x400000, 0x1000, 0x9000000));
    EXPECT(a.shader_side(arena) && a.shader_side(arena + size - 1) && !a.shader_side(arena + size));
    EXPECT(a.shader_side(0x9000fff) && !a.shader_side(0x9001000) && !a.shader_side(title));
    a.remove(title + 0x200000, 0x9000000);
    EXPECT(a.shader_side(0x9000000));  // the twin still names it
    a.remove(title + 0x400000, 0x9000000);
    EXPECT(!a.shader_side(0x9000000));
    // A range replaced by an overlapping one leaves the index with the new copy only.
    EXPECT(a.add(title + 0x100, 0x100, 0xA000000));
    EXPECT(!a.shader_side(arena) && a.shader_side(0xA0000ff) && !a.shader_side(0xA000100));
    a.remove(title + 0x100, 0xA000000);
    EXPECT(a.size() == 0 && !a.shader_side(0xA000000));

    constexpr std::uint64_t twin = title + 0x10000000;
    EXPECT(a.add(twin + size, size, arena));
    first.clear();
    a.add_listener(&first, hear);
    const auto pool_generation = a.generation();
    EXPECT(!a.add_twin(title, twin, size * 2, 0x5000000));
    EXPECT(a.generation() == pool_generation && !a.shader_side(0x5000000));
    EXPECT(a.size() == 1 && a.shader_va(title) == title && a.shader_va(twin) == twin);
    EXPECT(a.shader_va(twin + size) == arena);
    EXPECT(a.add_twin(title, twin, size, 0x5000000));
    EXPECT(a.generation() != pool_generation && a.shader_side(0x5000000));
    EXPECT(first.size() == 2);
    EXPECT(first[0].change == Change::added && first[0].title_va == title && first[0].bytes == size);
    EXPECT(first[1].change == Change::added && first[1].title_va == twin && first[1].bytes == size);
    a.remove_listener(&first);
    EXPECT(a.size() == 3 && a.shader_va(title) == 0x5000000 && a.shader_va(twin) == 0x5000000);
    EXPECT(a.shader_va(twin + size) == arena);

    a.remove(title, 0x5000000);
    EXPECT(a.add(title + size / 2, size, 0x7000000));
    EXPECT(!a.reuse_pool_mapping(title, twin, size, size, to));
    EXPECT(a.shader_va(title) == title);
    EXPECT(a.reuse_pool_mapping(title, twin, size / 2, size, to) && to == 0x5000000);
    EXPECT(!a.reuse_pool_mapping(title, twin, size / 2 + 1, size, to));
    EXPECT(a.range_of(title, to, room) && room == size / 2);
    EXPECT(a.shader_va(title + size / 2) == 0x7000000);
    EXPECT(a.reuse_pool_mapping(title + size / 2, twin + size / 2, size, size, to) && to == 0x7000000);
    EXPECT(!a.add_twin(title + 4 * size, title + 5 * size, size * 2, arena));
    EXPECT(a.add(title + 4 * size, size, 0x8000000));
    EXPECT(a.add(twin + 5 * size - 16, 16, 0x9000000));
    EXPECT(a.reuse_pool_mapping(twin + 4 * size + 16, title + 4 * size + 16, size - 32, size, to) && to == 0x8000010);
    a.remove(title + 4 * size, 0x8000000);
    EXPECT(a.shader_side(0x8000010));
    a.forget_copy(twin + 4 * size, size, 0x8000000);
    EXPECT(!a.shader_side(0x8000010));
    EXPECT(a.shader_va(twin + 4 * size + 16) == twin + 4 * size + 16);
    EXPECT(a.shader_va(twin + 5 * size - 16) == 0x9000000);

    std::printf("title_va_alias: translation inside a title-placed range, identity outside it and after release\n");
    return 0;
}
