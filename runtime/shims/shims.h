// Shim registry: reimplementations of console-only imports.
// Add a DLL by writing a shims/<dll>.cpp with a SHIM_TABLE and listing it in CMakeLists.txt.
#pragma once
#include <cstdint>
#include <string>

namespace rt::shims {

struct Entry {
    const char* dll;   // lower-case, with .dll
    const char* name;  // export name, or "#ordinal"
    void* fn;
};

// Registers a static table at startup.
struct Table {
    Table(const Entry* entries, size_t count);
};

void* find(const std::string& dll_lower, const std::string& name);

// Whether the shims own a whole module: the registry serves names from it and it is not a host system DLL (kernel32,
// kernelbase, the CRTs, api-ms-* sets, vulkan-1) of which only a few functions are replaced. A title's
// LoadLibrary/GetModuleHandle of an owned module gets a pseudo-handle that GetProcAddress resolves through find()
// (library_loader.cpp). `dll_lower` is a lower-case file name with ".dll".
bool owns_dll(const std::string& dll_lower);

// What a call to an import that has no binding does. Chosen by the environment variable LEONIDA_UNIMPLEMENTED
// (read once, on the first call):
//   log   (default)  print the name once and return 0, in rax and xmm0
//   fail             as log, but return E_NOTIMPL for imports classified as HRESULT-returning (classify_import)
//   stop             on the first call print the import, the caller's return address as module+RVA and rcx, rdx, r8,
//                    r9 as received; then break if a debugger is attached, else end the process with exit code 3
// Out parameters are never written in any mode.
enum class UnimplementedPolicy { Log, Fail, Stop };

// Whether an import is known to return an HRESULT. Judged by DLL and name only; see registry.cpp for the list and
// where each entry comes from. Unknown imports are Other.
enum class ReturnClass { HResult, Other };

inline constexpr uint32_t kENotImpl = 0x80004001u;  // E_NOTIMPL

// "log", "fail" or "stop", any case. Empty or null is log without complaint; anything else is log with *valid = false.
UnimplementedPolicy parse_unimplemented_policy(const char* text, bool* valid = nullptr);
UnimplementedPolicy unimplemented_policy();
void set_unimplemented_policy(UnimplementedPolicy policy);  // overrides the environment (tests)
ReturnClass classify_import(const std::string& dll, const std::string& name);

// Resolves an address to a mapped module for the `stop` message. A Loader registers itself; `fn` returns true and
// fills the module's file name and the address's RVA when the address is inside one of its modules.
using ModuleLookup = bool (*)(void* ctx, uint64_t address, std::string& module, uint32_t& rva);
void add_module_lookup(void* ctx, ModuleLookup fn);
// "a.dll+0x1234" for an address inside a mapped module, "host:ntdll.dll+0x1234" inside a host module, else the bare
// address in hex. Used by the startup trace (startup_trace.h).
std::string module_offset(uint64_t address);
void remove_module_lookup(void* ctx);

// The base of the mapped module holding `address`, through the same lookups. False if no live Loader maps it.
bool mapped_image_base(uint64_t address, uint64_t& base);

// Run-time LoadLibrary / GetModuleHandle / GetProcAddress / FreeLibrary for bundled package DLLs managed by a live
// Loader. Registered in Loader::Loader and removed in Loader::~Loader.
struct BundledModuleOps {
    void* (*find_loaded)(void* ctx, const std::string& dll_lower);
    void* (*load_bundled)(void* ctx, const std::string& dll_lower, uint32_t flags, bool& handled);
    void* (*get_proc)(void* ctx, void* module, const std::string& sym, bool& owns_handle);
    bool (*owns_handle)(void* ctx, void* module);
    // The same two lookups by the module's native (wide) basename, for names the ANSI code page cannot spell
    // (library_loader's W entries use them when a name has a character outside ASCII). Either may be null.
    void* (*find_loaded_w)(void* ctx, const std::wstring& basename);
    void* (*load_bundled_w)(void* ctx, const std::wstring& basename, uint32_t flags, bool& handled);
    // LEONIDA_COMPAT_DIR at run time. compat_module: `handled` when the folder holds `basename` (`name` is what the
    // caller gave: a name with a directory counts only when it is that file), which then owns the module ahead of
    // any shim; the result is its handle (loaded first when `load`, with the LoadLibraryEx `flags`, else null until
    // something loaded it), or null with the Windows error set when it cannot be loaded. owns_compat_handle: whether
    // `module` is a DLL from that folder, whose exports are its own. Either may be null.
    void* (*compat_module)(void* ctx, const std::wstring& basename, const std::wstring& name, bool load,
                           uint32_t flags, bool& handled);
    bool (*owns_compat_handle)(void* ctx, void* module);
};
void add_bundled_loader(void* ctx, const BundledModuleOps* ops);
void remove_bundled_loader(void* ctx);

void* find_loaded_bundled_module(const std::string& dll_lower);
void* load_bundled_module(const std::string& dll_lower, uint32_t flags, bool& handled);
void* find_loaded_bundled_module_w(const std::wstring& basename);
void* load_bundled_module_w(const std::wstring& basename, uint32_t flags, bool& handled);
void* get_bundled_proc_address(void* module, const std::string& sym, bool& owns_handle);
bool owns_bundled_module_handle(void* module);
void* compat_module(const std::wstring& basename, const std::wstring& name, bool load, uint32_t flags,
                    bool& handled);
bool owns_compat_module_handle(void* module);

// Title-exported __XMemAllocationHooks__ (GDK XMEM_ALLOCATION_HOOKS): the console runtime routes internal
// allocations (including D3D12 descriptor heaps) through these callbacks when the mapped image exports the struct.
// Registered by Loader after relocations and removed in Loader::~Loader.
using XMemAllocHookFn = void*(__stdcall*)(size_t size, uint64_t attributes);
using XMemFreeHookFn = void(__stdcall*)(void* ptr, uint64_t attributes);
struct XMemAllocationHooks {
    XMemAllocHookFn alloc;
    XMemFreeHookFn free;
};
void add_xmem_allocation_hooks(const void* owner, const XMemAllocationHooks* hooks);
void remove_xmem_allocation_hooks(const void* owner);
const XMemAllocationHooks* xmem_allocation_hooks();

// Test seam: called at the top of every unimplemented-import call with the address of rt_unimplemented's return
// address slot (so a test can check stack alignment) and from inside the call (so it can walk the stack).
using TestHook = void (*)(const void* return_address_slot);
void set_unimplemented_test_hook(TestHook hook);

// Makes a small thunk for an import with no binding; calling it follows the policy above. It passes the caller's
// return address and the first four argument registers through unchanged.
void* make_unimplemented_thunk(const std::string& dll, const std::string& name);

// Prints every unimplemented import that has been called.
void report_called_unimplemented();

}  // namespace rt::shims

#define SHIM_TABLE(id, ...)                                                       \
    static const ::rt::shims::Entry id##_entries[] = {__VA_ARGS__};               \
    static ::rt::shims::Table id##_table(id##_entries, sizeof(id##_entries) / sizeof(id##_entries[0]))
