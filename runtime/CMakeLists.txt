cmake_minimum_required(VERSION 3.24)
project(gta6rt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The sources are UTF-8. Without /utf-8, MSVC reads them in the system's ANSI code page, so on code page 936 (or
# any other non-1252 locale) it prints C4819 for every non-ASCII comment. Many targets set /utf-8 themselves; this
# covers every target, including those that don't. clang-cl accepts it too. Only C and C++: ml64 (abi_probe.asm) has
# no /utf-8 and warns A4018 for it.
if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "gta6rt must be built for x64 (run from an x64 Native Tools prompt or vcvars64.bat)")
endif()

# Portable root-signature model + packer to user SGPRs (no d3d12.h, no windows.h), and the host CPU preflight of
# --init/--run (src/host_cpu.h). Built on every OS, so their tests run in the Linux and macOS jobs as well as with the
# Windows runtime.
add_library(gta6rt_root_signature STATIC
  src/root_signature.cpp
)
target_include_directories(gta6rt_root_signature PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Portable contract test: model, packer, canonical codec and RTS0 hypothesis. Needs neither d3d12.lib nor the game.
add_executable(root_signature_test
  test/root_signature_test.cpp
)
target_link_libraries(root_signature_test PRIVATE gta6rt_root_signature)
# AVX, OSXSAVE and XCR0 YMM state from synthetic CPUID/XCR0 words, plus one call on the real host.
add_executable(host_cpu_test test/host_cpu_test.cpp src/host_cpu.cpp)
target_include_directories(host_cpu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MSVC)
  foreach(_portable gta6rt_root_signature root_signature_test host_cpu_test)
    target_compile_definitions(${_portable} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${_portable} PRIVATE /W4 /permissive- /utf-8)
  endforeach()
  target_link_options(root_signature_test PRIVATE /OPT:NOREF)
endif()

option(LEONIDA_BUILD_WINDOWS_RUNTIME "Build the Windows runtime, its shims and their tests" ${WIN32})
if(WIN32 AND LEONIDA_BUILD_WINDOWS_RUNTIME)
  # Custom CMAKE_CXX_FLAGS can replace MSVC's default /EHsc. Require C++ stack unwinding so error
  # paths in the runtime and its libraries still destroy automatic objects when an exception leaves a frame.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
  endif()

  # The d3d12_xs shim and its tests use version 1.2 root signatures (D3D12_ROOT_SIGNATURE_DESC2), which the Windows
  # SDK declares from 10.0.26100 on. Stop here with the fix instead of failing halfway through the build.
  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles([=[
    #include <d3d12.h>
    int main() { D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{}; d.Version = D3D_ROOT_SIGNATURE_VERSION_1_2;
                 return static_cast<int>(sizeof(d.Desc_1_2)); }
  ]=] GTA6RT_SDK_HAS_ROOT_SIGNATURE_1_2)
  if(NOT GTA6RT_SDK_HAS_ROOT_SIGNATURE_1_2)
    if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
      set(_gta6rt_sdk "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    elseif(DEFINED ENV{WindowsSDKVersion})
      string(REGEX REPLACE "[\\/]+$" "" _gta6rt_sdk "$ENV{WindowsSDKVersion}")
    else()
      set(_gta6rt_sdk "an older one")
    endif()
    message(FATAL_ERROR
      "gta6rt needs the Windows SDK 10.0.26100 or newer (version 1.2 root signatures); "
      "this build uses ${_gta6rt_sdk}. Install it with "
      "`winget install Microsoft.WindowsSDK.10.0.26100` or the Visual Studio Installer, then delete the build "
      "folder and configure again.")
  endif()

  # Root-signature flag fix, shared by the d3d12_xs shim and its tests.
  add_library(gta6rt_d3d12_xs_rootsig STATIC
    shims/d3d12_xs_rootsig.cpp
  )
  target_include_directories(gta6rt_d3d12_xs_rootsig PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(gta6rt_d3d12_xs_rootsig PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(gta6rt_d3d12_xs_rootsig PRIVATE /W4 /permissive- /utf-8)

  # XTaskQueue/XAsync from microsoft/libHttpClient (MIT), vendored in third_party/xtaskqueue; shims/xtaskqueue/pch.h
  # stands in for libHttpClient's pch.h so only the task queue builds. Used by the xgameruntime shim.
  set(XTASKQUEUE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/xtaskqueue)
  add_library(gta6rt_xtaskqueue STATIC
    ${XTASKQUEUE_DIR}/Source/Task/TaskQueue.cpp
    ${XTASKQUEUE_DIR}/Source/Task/AsyncLib.cpp
    ${XTASKQUEUE_DIR}/Source/Task/ThreadPool_win32.cpp
    ${XTASKQUEUE_DIR}/Source/Task/WaitTimer_win32.cpp
  )
  target_include_directories(gta6rt_xtaskqueue
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims/xtaskqueue ${XTASKQUEUE_DIR}/Source/Task ${XTASKQUEUE_DIR}/Source/Common
    PUBLIC ${XTASKQUEUE_DIR}/Include)
  target_compile_definitions(gta6rt_xtaskqueue PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(gta6rt_xtaskqueue PRIVATE /utf-8)  # third-party code: its own warning level
  target_link_libraries(gta6rt_xtaskqueue PUBLIC ole32)

  # Reserves one loader-assigned module TLS index for mapped images. Loaded from the executable's directory by
  # Loader::initialize_tls; its trivial DllMain only keeps the TLS anchor in the image. Built in the same directory as
  # the executables that map images, because the loader looks for it next to the running EXE.
  add_library(gta6rt_tls_host SHARED src/tls_host.cpp)
  target_compile_options(gta6rt_tls_host PRIVATE /W4 /permissive- /utf-8 /GS-)
  target_compile_definitions(gta6rt_tls_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  set_target_properties(gta6rt_tls_host PROPERTIES
    OUTPUT_NAME gta6rt_tls_host
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )

  # Rust XMA2 decoder (runtime/xma_decoder), built at configure time when cargo is on PATH,
  # or supplied prebuilt via -DLEONIDA_XMA2_DECODER_LIB=<path> (skipping cargo at configure time).
  # Without it (or if the cargo build fails) the runtime still builds and XMA voices output silence.
  option(LEONIDA_REQUIRE_XMA2_DECODER "Fail configure if the Rust XMA2 decoder is unavailable" OFF)
  set(LEONIDA_XMA2_DECODER_LIB "" CACHE FILEPATH
    "Prebuilt leonida_xma2_decoder.lib (skips running cargo at configure time)")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xma2_decoder.cmake")
  leonida_resolve_prebuilt_xma2_decoder("${LEONIDA_XMA2_DECODER_LIB}" "${CMAKE_BINARY_DIR}" _xma_lib_resolved)
  if(_xma_lib_resolved)
    message(STATUS "Using prebuilt Rust XMA2 decoder: ${_xma_lib_resolved}")
  else()
    find_program(LEONIDA_CARGO cargo)
    if(LEONIDA_CARGO)
      # Cargo's host (or configured default target) can differ from the runtime's MSVC ABI.
      set(_xma_target x86_64-pc-windows-msvc)
      set(_xma_target_dir ${CMAKE_BINARY_DIR}/xma_decoder)
      set(_xma_lib ${_xma_target_dir}/${_xma_target}/release/leonida_xma2_decoder.lib)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/xma_decoder/Cargo.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/xma_decoder/Cargo.lock
        ${CMAKE_CURRENT_SOURCE_DIR}/xma_decoder/src/lib.rs)
      execute_process(
        COMMAND ${LEONIDA_CARGO} build --release --locked --target ${_xma_target}
                --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/xma_decoder/Cargo.toml
                --target-dir ${_xma_target_dir}
        TIMEOUT 120
        RESULT_VARIABLE _xma_result)
      if(_xma_result STREQUAL "0" AND EXISTS "${_xma_lib}")
        set(_xma_lib_resolved "${_xma_lib}")
      else()
        message(WARNING "cargo build of runtime/xma_decoder failed: XMA voices will be silent")
      endif()
    else()
      message(STATUS "cargo not found: runtime/xma_decoder not built, XMA voices will be silent")
    endif()
  endif()
  if(_xma_lib_resolved)
    add_library(leonida_xma2_decoder STATIC IMPORTED)
    # Link Rust's Windows SDK dependencies. CMake's C++ target selects its own CRT
    # (/MD or /MDd); the C ABI keeps allocation and deallocation on the same side.
    # synchronization: Rust's std imports WaitOnAddress and WakeByAddress*, which MSVC's
    # default libraries cover but clang-cl with an xwin SDK does not (lld-link: undefined symbol).
    set_target_properties(leonida_xma2_decoder PROPERTIES
      IMPORTED_LOCATION "${_xma_lib_resolved}"
      INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/xma_decoder"
      INTERFACE_COMPILE_DEFINITIONS LEONIDA_HAVE_XMA2_DECODER=1
      INTERFACE_LINK_LIBRARIES "kernel32;ntdll;userenv;ws2_32;dbghelp;synchronization")
  endif()
  if(LEONIDA_REQUIRE_XMA2_DECODER AND NOT TARGET leonida_xma2_decoder)
    message(FATAL_ERROR "LEONIDA_REQUIRE_XMA2_DECODER is enabled, but the Rust XMA2 decoder is unavailable")
  endif()

  # Rust Opus decoder (runtime/opus_decoder), the same pattern as the XMA2 decoder: cargo at
  # configure time, a prebuilt -DLEONIDA_OPUS_DECODER_LIB, or the silent path in xapu_queue.cpp.
  option(LEONIDA_REQUIRE_OPUS_DECODER "Fail configure if the Rust Opus decoder is unavailable" OFF)
  set(LEONIDA_OPUS_DECODER_LIB "" CACHE FILEPATH
    "Prebuilt leonida_opus_decoder.lib (skips running cargo at configure time)")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/opus_decoder.cmake")
  leonida_resolve_prebuilt_opus_decoder("${LEONIDA_OPUS_DECODER_LIB}" "${CMAKE_BINARY_DIR}" _opus_lib_resolved)
  if(_opus_lib_resolved)
    message(STATUS "Using prebuilt Rust Opus decoder: ${_opus_lib_resolved}")
  else()
    if(NOT LEONIDA_CARGO)
      find_program(LEONIDA_CARGO cargo)
    endif()
    if(LEONIDA_CARGO)
      set(_opus_target x86_64-pc-windows-msvc)
      set(_opus_target_dir ${CMAKE_BINARY_DIR}/opus_decoder)
      set(_opus_lib ${_opus_target_dir}/${_opus_target}/release/leonida_opus_decoder.lib)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/opus_decoder/Cargo.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/opus_decoder/Cargo.lock
        ${CMAKE_CURRENT_SOURCE_DIR}/opus_decoder/src/lib.rs)
      execute_process(
        COMMAND ${LEONIDA_CARGO} build --release --locked --target ${_opus_target}
                --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/opus_decoder/Cargo.toml
                --target-dir ${_opus_target_dir}
        TIMEOUT 300
        RESULT_VARIABLE _opus_result)
      if(_opus_result STREQUAL "0" AND EXISTS "${_opus_lib}")
        set(_opus_lib_resolved "${_opus_lib}")
      else()
        message(WARNING "cargo build of runtime/opus_decoder failed: the XAPU Opus path will be silent")
      endif()
    else()
      message(STATUS "cargo not found: runtime/opus_decoder not built, the XAPU Opus path will be silent")
    endif()
  endif()
  if(_opus_lib_resolved)
    add_library(leonida_opus_decoder STATIC IMPORTED)
    set_target_properties(leonida_opus_decoder PROPERTIES
      IMPORTED_LOCATION "${_opus_lib_resolved}"
      INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/opus_decoder"
      INTERFACE_COMPILE_DEFINITIONS LEONIDA_HAVE_OPUS_DECODER=1
      INTERFACE_LINK_LIBRARIES "kernel32;ntdll;userenv;ws2_32;dbghelp;synchronization")
  endif()
  if(LEONIDA_REQUIRE_OPUS_DECODER AND NOT TARGET leonida_opus_decoder)
    message(FATAL_ERROR "LEONIDA_REQUIRE_OPUS_DECODER is enabled, but the Rust Opus decoder is unavailable")
  endif()

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(bcpack_tools "${CMAKE_CURRENT_SOURCE_DIR}/../tools")
  set(bcpack_tables "${CMAKE_CURRENT_BINARY_DIR}/bcpack_tables.inc")
  add_custom_command(OUTPUT "${bcpack_tables}"
    COMMAND "${Python3_EXECUTABLE}" -I "${bcpack_tools}/gen_bcpack_runtime_tables.py" "${bcpack_tables}"
    DEPENDS "${bcpack_tools}/gen_bcpack_runtime_tables.py" "${bcpack_tools}/bcpack.py"
            "${bcpack_tools}/bcpack_tables.py" "${bcpack_tools}/bcpack_tables_bptc.py"
    VERBATIM)
  add_executable(bcpack_history_train "../tools/bcpack_history_train.cpp" "${bcpack_tables}")
  target_include_directories(bcpack_history_train PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  add_library(gta6rt_bcpack STATIC src/bcpack.cpp "${bcpack_tables}")
  target_include_directories(gta6rt_bcpack PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

  add_executable(gta6rt
    src/main.cpp
    src/host_cpu.cpp
    src/pe_loader.cpp
    src/thread_stacks.cpp
    shims/registry.cpp
    shims/console_registry.cpp
    shims/file_system.cpp
    shims/winhttp_offline.cpp
    shims/ws2_offline.cpp
    shims/xmem.cpp
    shims/xg_xs.cpp
    shims/acphal.cpp
    shims/d3d12_xs.cpp
    shims/d3d12_xs_device.cpp
    shims/d3d12_xs_pso_stream.cpp
    shims/xgameruntime.cpp
    shims/xgameruntime_user.cpp
    shims/xgameruntime_gamesave.cpp
    shims/xgameruntime_store.cpp
    shims/xgameruntime_offline.cpp
    shims/xbox_bvh_builder.cpp
    shims/library_loader.cpp
    shims/process.cpp
    shims/lifecycle.cpp
    shims/com_shim.cpp
    shims/silent_audio.cpp
    shims/bcrypt_rng.cpp
    shims/xapu_queue.cpp
    shims/xapu_chad.cpp
    shims/dstorage.cpp
  )
  # These sources use __try to handle access violations. MSVC always catches them; clang-cl needs asynchronous
  # exceptions (as for tiny_dll below). A source file property also applies to dstorage_test.
  set_source_files_properties(shims/dstorage.cpp src/thread_stacks.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang>:/EHa>")
  add_dependencies(gta6rt gta6rt_tls_host bcpack_history_train)
  target_compile_definitions(gta6rt PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_compile_options(gta6rt PRIVATE /W4 /permissive- /utf-8)
  # Static shim tables register themselves at startup; keep the linker from dropping their objects.
  # GTA6RT_HOST_BASE keeps the loader's own image off the default x64 EXE base (0x140000000), where a mapped EXE
  # without relocations wants to go. Windows moves the EXE with ASLR anyway; Wine loads it at its preferred base.
  set(GTA6RT_HOST_BASE /BASE:0x10000000000)
  # BUILD-1457: a thread that asks for no stack size gets the reserve in this program's PE header, 1 MB by default:
  # a thread the game starts with dwStackSize 0, a thread-pool thread, a std::thread of the shims. The D3D12 shim
  # creates Vulkan pipelines on the calling thread, the driver compiles there, and AMD's Windows compiler (26.11.2)
  # overflows 1 MB on some of the game's shaders (docs/pipeline_check.md, "Stack reserve"). run_entry already gives
  # the entry thread 8 MB (shims/process_shim.h); this is the same number for the others. Keep it in step with
  # scripts/build_runtime.ps1.
  set(GTA6RT_STACK 8388608)
  target_link_options(gta6rt PRIVATE /OPT:NOREF ${GTA6RT_HOST_BASE} /STACK:${GTA6RT_STACK})
  target_link_libraries(gta6rt PRIVATE gta6rt_bcpack gta6rt_d3d12_xs_rootsig gta6rt_xtaskqueue psapi)

  # Root-signature forwarding test: needs a real PC d3d12.lib to serialize the synthetic descriptors.
  # find_library searches %LIB% (a Developer Prompt), not the Windows SDK the Visual Studio generator links
  # against, so also look in that SDK's um/x64 folder.
  set(_sdk_lib_hints)
  if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKLibVersion})
    list(APPEND _sdk_lib_hints "$ENV{WindowsSdkDir}Lib/$ENV{WindowsSDKLibVersion}um/x64")
  endif()
  get_filename_component(_kits_root
    "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
  if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND EXISTS "${_kits_root}")
    list(APPEND _sdk_lib_hints "${_kits_root}/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/um/x64")
  endif()
  find_library(D3D12_LIBRARY NAMES d3d12 HINTS ${_sdk_lib_hints})
  find_library(DXGI_LIBRARY NAMES dxgi HINTS ${_sdk_lib_hints})
  if(D3D12_LIBRARY)
    add_executable(root_sig_test
      test/root_sig_test.cpp
      shims/registry.cpp
      shims/d3d12_xs.cpp
      shims/d3d12_xs_device.cpp
      shims/d3d12_xs_pso_stream.cpp
    )
    target_link_libraries(root_sig_test PRIVATE gta6rt_d3d12_xs_rootsig ${D3D12_LIBRARY})
    target_compile_definitions(root_sig_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(root_sig_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(root_sig_test PRIVATE /OPT:NOREF)

    # The flag mask keeps every public root-signature flag: helpers over [0, 0xFFFF], the shims against the host
    # serializer and deserializer (old upstream PR 7140).
    add_executable(root_sig_public_flags_test
      test/root_sig_public_flags_test.cpp
      shims/registry.cpp
      shims/d3d12_xs.cpp
      shims/d3d12_xs_device.cpp
      shims/d3d12_xs_pso_stream.cpp
    )
    target_link_libraries(root_sig_public_flags_test PRIVATE gta6rt_d3d12_xs_rootsig ${D3D12_LIBRARY})
    target_compile_definitions(root_sig_public_flags_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(root_sig_public_flags_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(root_sig_public_flags_test PRIVATE /OPT:NOREF)

    # RTS0 parser oracle: synthetic descriptors only, serialized by the real PC
    # d3d12.dll and parsed back by rt::rootsig::parse_rts0. A layout mismatch
    # fails here with the blob bytes, never silently in a draw.
    add_executable(root_signature_oracle_test
      test/root_signature_oracle_test.cpp
    )
    target_link_libraries(root_signature_oracle_test PRIVATE gta6rt_root_signature ${D3D12_LIBRARY})
    target_compile_definitions(root_signature_oracle_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(root_signature_oracle_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(root_signature_oracle_test PRIVATE /OPT:NOREF)

    # Xbox-ABI device proxy test: WARP PC device, no game executable. Needs DXGI for the WARP adapter.
    if(DXGI_LIBRARY)
      add_executable(device_proxy_test
        test/device_proxy_test.cpp
        shims/registry.cpp
        shims/d3d12_xs.cpp
        shims/d3d12_xs_device.cpp
        shims/d3d12_xs_pso_stream.cpp
      )
      target_link_libraries(device_proxy_test PRIVATE gta6rt_d3d12_xs_rootsig ${D3D12_LIBRARY} ${DXGI_LIBRARY})
      target_compile_definitions(device_proxy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(device_proxy_test PRIVATE /W4 /permissive- /utf-8)
      target_link_options(device_proxy_test PRIVATE /OPT:NOREF)
    else()
      message(STATUS "dxgi.lib not found: device_proxy_test is not built")
    endif()
    # The queue and fence proxies over synthetic COM objects: no D3D12 runtime, no GPU. Replaces operator new and
    # patches its own import table to inject failures, so the proxy source is compiled into it alone.
    add_executable(xs_proxy_mock_test
      test/xs_proxy_mock_test.cpp
      shims/d3d12_xs_device.cpp
      shims/d3d12_xs_pso_stream.cpp
    )
    target_compile_definitions(xs_proxy_mock_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(xs_proxy_mock_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
  else()
    message(STATUS "d3d12.lib not found: root_sig_test and device_proxy_test are not built")
  endif()

  # xmem shim ABI test: resolves the registered xmem.dll exports itself, so xmem.cpp and registry.cpp are compiled
  # into this executable instead of reusing gta6rt's objects (which would duplicate their symbols).
  foreach(target IN ITEMS xmem_test xmem_wine_commit_test)
    add_executable(${target}
      test/${target}.cpp
      shims/xmem.cpp
      shims/registry.cpp
    )
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 $<$<CXX_COMPILER_ID:Clang>:/EHa>)
    target_link_options(${target} PRIVATE /OPT:NOREF)
    target_link_libraries(${target} PRIVATE psapi)
  endforeach()

  # The title's CLZERO loops rewritten to stubs on a CPU without CLZERO (src/clzero_patch.h): both loop shapes are
  # found, patched and run, so the test runs on any x86-64 CPU.
  add_executable(clzero_patch_test test/clzero_patch_test.cpp)
  target_include_directories(clzero_patch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(clzero_patch_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(clzero_patch_test PRIVATE /W4 /permissive- /utf-8)

  # xg_xs shim: the two exports and the texture computer's slots against the measured call shapes.
  add_executable(xg_xs_test
    test/xg_xs_test.cpp
    shims/xg_xs.cpp
    shims/registry.cpp
  )
  target_include_directories(xg_xs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(xg_xs_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(xg_xs_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(xg_xs_test PRIVATE /OPT:NOREF)

  # acphal shim ABI test: resolves the registered acphal.dll exports through the registry.
  add_executable(acphal_test
    test/acphal_test.cpp
    shims/acphal.cpp
    shims/registry.cpp
  )
  target_include_directories(acphal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(acphal_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(acphal_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(acphal_test PRIVATE /OPT:NOREF)

  if(TARGET leonida_xma2_decoder)
    target_link_libraries(gta6rt PRIVATE leonida_xma2_decoder)
    target_link_libraries(acphal_test PRIVATE leonida_xma2_decoder)
  endif()
  if(TARGET leonida_opus_decoder)
    target_link_libraries(gta6rt PRIVATE leonida_opus_decoder)
  endif()

  # The Zen 2 instructions a desktop CPU may lack (src/cpu_emulation.h): decoded, emulated on a CONTEXT, and run for
  # real with the #UD handler installed, so an Intel host takes the emulated path and an AMD one the native path.
  add_executable(cpu_emulation_test test/cpu_emulation_test.cpp)
  target_include_directories(cpu_emulation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(cpu_emulation_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(cpu_emulation_test PRIVATE /W4 /permissive- /utf-8)

  # Xbox DirectStorage (DStorageGetFactory under dstorage_xs.dll, xcrdapi.dll and dstorage.dll) over synthetic files:
  # GAME: paths through the file shims, queues, status arrays, fences and errors. No GPU, no game data.
  set(dstorage_vectors "${CMAKE_CURRENT_BINARY_DIR}/dstorage_vectors.inc")
  add_custom_command(OUTPUT "${dstorage_vectors}"
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test/dstorage_vectors.py" "${dstorage_vectors}"
    DEPENDS test/dstorage_vectors.py ../tests/test_bcpack.py "${bcpack_tools}/bcpack.py"
            "${bcpack_tools}/bcpack_tables.py" "${bcpack_tools}/bcpack_tables_bptc.py"
    VERBATIM)
  add_executable(dstorage_test
    "${dstorage_vectors}"
    test/dstorage_test.cpp
    shims/xmem.cpp
    shims/dstorage.cpp
    shims/file_system.cpp
    shims/registry.cpp
  )
  target_include_directories(dstorage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims "${CMAKE_CURRENT_BINARY_DIR}")
  target_compile_definitions(dstorage_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(dstorage_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(dstorage_test PRIVATE /OPT:NOREF)
  target_link_libraries(dstorage_test PRIVATE gta6rt_bcpack psapi)

  # xapu.dll and chad.dll, the APU and audio DSP command queues, resolved through the registry like xmem_test; Opus
  # frame counts from hand-made packets.
  add_executable(xapu_chad_test
    test/xapu_chad_test.cpp
    shims/xapu_queue.cpp
    shims/xapu_chad.cpp
    shims/registry.cpp
  )
  target_include_directories(xapu_chad_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(xapu_chad_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(xapu_chad_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(xapu_chad_test PRIVATE /OPT:NOREF)

  if(TARGET leonida_opus_decoder)
    # xapu_queue.cpp with the decoder compiled in, against the generated Opus tone.
    add_executable(xapu_opus_test
      test/xapu_opus_test.cpp
      shims/xapu_queue.cpp
      shims/registry.cpp
    )
    target_include_directories(xapu_opus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
    target_compile_definitions(xapu_opus_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(xapu_opus_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(xapu_opus_test PRIVATE /OPT:NOREF)
    target_link_libraries(xapu_opus_test PRIVATE leonida_opus_decoder)
  endif()

  # The boot stack's earlier xapu.dll and chad.dll shims (shims/xapu.cpp, shims/chad.cpp), which gta6rt no longer
  # links (it serves both DLLs from xapu_chad.cpp): their ABI tests stay as tests of those files.
  # xapu shim ABI test: resolves the registered xapu.dll exports through the registry.
  add_executable(xapu_test
    test/xapu_test.cpp
    shims/xapu.cpp
    shims/registry.cpp
  )
  target_include_directories(xapu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(xapu_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(xapu_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(xapu_test PRIVATE /OPT:NOREF)

  # chad shim ABI test: resolves the registered chad.dll exports through the registry.
  add_executable(chad_test
    test/chad_test.cpp
    shims/chad.cpp
    shims/registry.cpp
  )
  target_include_directories(chad_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(chad_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(chad_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(chad_test PRIVATE /OPT:NOREF)

  # The process shim's command line: GetCommandLineW must parse (CommandLineToArgvW) back to its __wargv.
  add_executable(process_cmdline_test
    test/process_cmdline_test.cpp
    shims/process.cpp
    shims/registry.cpp
  )
  target_include_directories(process_cmdline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(process_cmdline_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(process_cmdline_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(process_cmdline_test PRIVATE /OPT:NOREF)
  target_link_libraries(process_cmdline_test PRIVATE shell32)

  # Console lifecycle shim: RegisterAppStateChangeNotification, oversized window fitting, and subclass forwarding.
  add_executable(lifecycle_test
    test/lifecycle_test.cpp
    shims/lifecycle.cpp
    shims/registry.cpp
  )
  target_include_directories(lifecycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(lifecycle_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(lifecycle_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(lifecycle_test PRIVATE /OPT:NOREF)
  target_link_libraries(lifecycle_test PRIVATE user32)

  # Console paths (GAME:) and the write overlay: the file shims resolved through the registry, over temporary folders.
  add_executable(file_system_test
    test/file_system_test.cpp
    shims/file_system.cpp
    shims/registry.cpp
  )
  target_include_directories(file_system_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(file_system_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(file_system_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(file_system_test PRIVATE /OPT:NOREF)
  target_link_libraries(file_system_test PRIVATE advapi32)  # the ACL calls of the unlistable-marker-folder check

  # LEONIDA_SKIP_INT3 (shims/startup_trace.h): each case runs in a child process with its own environment.
  add_executable(int3_skip_test
    test/int3_skip_test.cpp
    shims/registry.cpp
  )
  target_include_directories(int3_skip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(int3_skip_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(int3_skip_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(int3_skip_test PRIVATE /OPT:NOREF)

  # Offline WinHTTP for mapped images (the package's XCurl.dll and libHttpClient.GDK.dll); no network is touched.
  add_executable(winhttp_offline_test
    test/winhttp_offline_test.cpp
    shims/winhttp_offline.cpp
    shims/registry.cpp
  )
  target_include_directories(winhttp_offline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(winhttp_offline_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(winhttp_offline_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(winhttp_offline_test PRIVATE /OPT:NOREF)

  # Offline Winsock2 for mapped images (ws2_32.dll restricted to loopback); no external network is touched.
  add_executable(ws2_offline_test
    test/ws2_offline_test.cpp
    shims/ws2_offline.cpp
    shims/registry.cpp
  )
  target_include_directories(ws2_offline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(ws2_offline_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(ws2_offline_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(ws2_offline_test PRIVATE /OPT:NOREF)
  target_link_libraries(ws2_offline_test PRIVATE ws2_32)

  # COM lifecycle and helper shims for api-ms-win-core-com-l1-1-*.dll.
  add_executable(com_shim_test
    test/com_shim_test.cpp
    shims/com_shim.cpp
    shims/silent_audio.cpp
    shims/registry.cpp
  )
  target_include_directories(com_shim_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(com_shim_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(com_shim_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(com_shim_test PRIVATE /OPT:NOREF)

  # The silent output endpoint (MMDeviceEnumerator through CoCreateInstance): stand-in hosts, a render stream on
  # its clock, and the CoCreateInstance hook with LEONIDA_AUDIO=silent. No host audio device is opened.
  add_executable(silent_audio_test
    test/silent_audio_test.cpp
    shims/silent_audio.cpp
    shims/com_shim.cpp
    shims/registry.cpp
  )
  target_include_directories(silent_audio_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(silent_audio_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(silent_audio_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(silent_audio_test PRIVATE /OPT:NOREF)

  # BCrypt RNG and algorithm provider shims.
  add_executable(bcrypt_rng_test
    test/bcrypt_rng_test.cpp
    shims/bcrypt_rng.cpp
    shims/registry.cpp
  )
  target_include_directories(bcrypt_rng_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(bcrypt_rng_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(bcrypt_rng_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(bcrypt_rng_test PRIVATE /OPT:NOREF)

  # LEONIDA_STACKS's sampler (src/thread_stacks.cpp): call chains of helper threads, no game data.
  add_executable(thread_stacks_test
    test/thread_stacks_test.cpp
    src/thread_stacks.cpp
    shims/registry.cpp
  )
  target_include_directories(thread_stacks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(thread_stacks_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_compile_options(thread_stacks_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(thread_stacks_test PRIVATE /OPT:NOREF)

  # kernel_writes (src/title_page_watch.h): a file read into reserved, first-touch memory retried after a commit.
  add_executable(kernel_writes_test test/kernel_writes_test.cpp)
  target_compile_definitions(kernel_writes_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_compile_options(kernel_writes_test PRIVATE /W4 /permissive- /utf-8)

  # guarded_copy (src/guarded_copy.h): a fault in a copy made under __try reaches its __except, under Wine too.
  add_executable(guarded_copy_test test/guarded_copy_test.cpp)
  target_compile_definitions(guarded_copy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(guarded_copy_test PRIVATE /W4 /permissive- /utf-8)

  # title_write_watch (src/title_page_watch.h): the journal that shares write-watch answers between trackers.
  add_executable(title_write_watch_test test/title_write_watch_test.cpp)
  target_compile_definitions(title_write_watch_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_compile_options(title_write_watch_test PRIVATE /W4 /permissive- /utf-8)

  # title_memory_map (src/title_page_watch.h): map changes by range, and the texture pass that probes only what they hit.
  add_executable(title_memory_map_test test/title_memory_map_test.cpp)
  target_compile_definitions(title_memory_map_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_compile_options(title_memory_map_test PRIVATE /W4 /permissive- /utf-8)

  # Console model in the registry (ConsoleMode) against LEONIDA_DEVICE_TYPE.
  add_executable(console_registry_test
    test/console_registry_test.cpp
    shims/console_registry.cpp
    shims/registry.cpp
  )
  target_include_directories(console_registry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(console_registry_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(console_registry_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(console_registry_test PRIVATE /OPT:NOREF)

  # Synthetic tests for the atomic API reference lifecycle.
  add_executable(api_ref_state_test test/api_ref_state_test.cpp)
  target_include_directories(api_ref_state_test PRIVATE
    ${XTASKQUEUE_DIR}/Source/Task)
  target_compile_definitions(api_ref_state_test PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(api_ref_state_test PRIVATE
    /W4 /permissive- /utf-8)
  # Verify global reference accounting during task queue teardown.
  add_executable(api_global_refs_test test/api_global_refs_test.cpp)
  target_compile_definitions(api_global_refs_test PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(api_global_refs_test PRIVATE
    /W4 /permissive- /utf-8)
  target_link_libraries(api_global_refs_test PRIVATE
    gta6rt_xtaskqueue)
  # Offline GDK runtime shim (XGameRuntime, XSystem, XTaskQueue, XAsync), driven through the shim registry the way
  # a title calls it at boot. Like xmem_test it compiles its own registry.cpp.
  add_executable(xgameruntime_test
    test/xgameruntime_test.cpp
    shims/xgameruntime.cpp
    shims/xgameruntime_user.cpp
    shims/xgameruntime_gamesave.cpp
    shims/xgameruntime_store.cpp
    shims/xgameruntime_offline.cpp
    shims/library_loader.cpp
    shims/registry.cpp
    shims/file_system.cpp
  )
  target_include_directories(xgameruntime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(xgameruntime_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(xgameruntime_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(xgameruntime_test PRIVATE /OPT:NOREF)
  target_link_libraries(xgameruntime_test PRIVATE gta6rt_xtaskqueue)

  # XUserImpl (offline local user) through QueryApiImpl, slot by slot as xgameruntime.lib's thunks call it.
  add_executable(xgameruntime_user_test
    test/xgameruntime_user_test.cpp
    shims/xgameruntime.cpp
    shims/xgameruntime_user.cpp
    shims/xgameruntime_gamesave.cpp
    shims/xgameruntime_store.cpp
    shims/xgameruntime_offline.cpp
    shims/library_loader.cpp
    shims/registry.cpp
    shims/file_system.cpp
  )
  target_include_directories(xgameruntime_user_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(xgameruntime_user_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(xgameruntime_user_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(xgameruntime_user_test PRIVATE /OPT:NOREF)
  target_link_libraries(xgameruntime_user_test PRIVATE gta6rt_xtaskqueue)

  # XGameSaveImpl (offline saves in LEONIDA_SAVE_DIR) through QueryApiImpl, slot by slot.
  add_executable(xgameruntime_gamesave_test
    test/xgameruntime_gamesave_test.cpp
    shims/xgameruntime.cpp
    shims/xgameruntime_user.cpp
    shims/xgameruntime_gamesave.cpp
    shims/xgameruntime_store.cpp
    shims/xgameruntime_offline.cpp
    shims/library_loader.cpp
    shims/registry.cpp
    shims/file_system.cpp
  )
  target_include_directories(xgameruntime_gamesave_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(xgameruntime_gamesave_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(xgameruntime_gamesave_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(xgameruntime_gamesave_test PRIVATE /OPT:NOREF)
  target_link_libraries(xgameruntime_gamesave_test PRIVATE gta6rt_xtaskqueue)

  # XStoreImpl (offline: the game is owned, no add-ons, no updates) through QueryApiImpl, slot by slot.
  add_executable(xgameruntime_store_test
    test/xgameruntime_store_test.cpp
    shims/xgameruntime.cpp
    shims/xgameruntime_user.cpp
    shims/xgameruntime_gamesave.cpp
    shims/xgameruntime_store.cpp
    shims/xgameruntime_offline.cpp
    shims/library_loader.cpp
    shims/registry.cpp
    shims/file_system.cpp
  )
  target_include_directories(xgameruntime_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(xgameruntime_store_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(xgameruntime_store_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(xgameruntime_store_test PRIVATE /OPT:NOREF)
  target_link_libraries(xgameruntime_store_test PRIVATE gta6rt_xtaskqueue)

  # XGameEventImpl, XGameUiImpl, XGameStreamingImpl (offline) through QueryApiImpl.
  add_executable(xgameruntime_offline_test
    test/xgameruntime_offline_test.cpp
    shims/xgameruntime.cpp
    shims/xgameruntime_user.cpp
    shims/xgameruntime_gamesave.cpp
    shims/xgameruntime_store.cpp
    shims/xgameruntime_offline.cpp
    shims/library_loader.cpp
    shims/registry.cpp
    shims/file_system.cpp
  )
  target_include_directories(xgameruntime_offline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
  target_compile_definitions(xgameruntime_offline_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(xgameruntime_offline_test PRIVATE /W4 /permissive- /utf-8)
  target_link_options(xgameruntime_offline_test PRIVATE /OPT:NOREF)
  target_link_libraries(xgameruntime_offline_test PRIVATE gta6rt_xtaskqueue)

  # GameInput shim on SDL3 (zlib licence): any controller SDL supports, DualSense trigger effects and lightbar.
  # Nothing is downloaded unless asked. The shim is built when SDL3 is available:
  #   - an installed SDL3 3.2+ that find_package finds (SDL3_DIR / CMAKE_PREFIX_PATH), or
  #   - -DLEONIDA_SDL3_FETCH=ON: download the pinned release below once at configure time (hash-checked) and build it
  #     static, or -DFETCHCONTENT_SOURCE_DIR_SDL3=<extracted SDL3-3.4.20> to build that copy without downloading.
  # Otherwise, or with -DLEONIDA_SDL3=OFF, gta6rt is built without the shim and the loader binds GameInputCreate to the
  # host's GameInput.dll or the unimplemented thunk as before. docs/gameinput.md has the details.
  option(LEONIDA_SDL3 "Build the GameInput shim when SDL3 is available" ON)
  option(LEONIDA_SDL3_FETCH "Download the pinned SDL3 release if no installed SDL3 is found" OFF)
  if(LEONIDA_SDL3)
    find_package(SDL3 3.2 CONFIG QUIET COMPONENTS SDL3-static NO_SYSTEM_ENVIRONMENT_PATH)
    if(NOT TARGET SDL3::SDL3-static AND (LEONIDA_SDL3_FETCH OR FETCHCONTENT_SOURCE_DIR_SDL3))
      include(FetchContent)
      FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
      # Only what gamepads need: the joystick, HIDAPI, haptic, sensor and power code, in a static library.
      set(SDL_SHARED OFF CACHE BOOL "" FORCE)
      set(SDL_STATIC ON CACHE BOOL "" FORCE)
      set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
      set(SDL_TESTS OFF CACHE BOOL "" FORCE)
      set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
      set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
      foreach(_sdl_off SDL_AUDIO SDL_VIDEO SDL_GPU SDL_RENDER SDL_CAMERA SDL_DIALOG SDL_TRAY)
        set(${_sdl_off} OFF CACHE BOOL "" FORCE)
      endforeach()
      FetchContent_MakeAvailable(SDL3)
    endif()
  endif()
  if(NOT (LEONIDA_SDL3 AND TARGET SDL3::SDL3-static))
    # Nothing served gameinput.dll in a build without SDL3, so the loader fell through to the host's gameinput.dll and
    # that one answers GameInputCreate with a null interface the title dereferences. Keep the object identity honest,
    # and with LEONIDA_INPUT_SCRIPT serve one pad driven by the script (shims/gameinput_script.h, shared with SDL3's).
    target_sources(gta6rt PRIVATE shims/gameinput_no_sdl.cpp)
    add_executable(gameinput_no_sdl_test
      test/gameinput_no_sdl_test.cpp
      shims/gameinput_no_sdl.cpp
      shims/registry.cpp
    )
    target_include_directories(gameinput_no_sdl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
    target_compile_definitions(gameinput_no_sdl_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(gameinput_no_sdl_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(gameinput_no_sdl_test PRIVATE /OPT:NOREF)
  endif()
  if(LEONIDA_SDL3 AND TARGET SDL3::SDL3-static)
    target_sources(gta6rt PRIVATE shims/gameinput.cpp)
    target_link_libraries(gta6rt PRIVATE SDL3::SDL3-static)

    # GameInput through the shim registry on SDL virtual joysticks: no controller, no game data.
    add_executable(gameinput_test test/gameinput_test.cpp shims/gameinput.cpp shims/registry.cpp)
    # gameinput_probe: the same shim on a real controller, run by hand (not a CTest).
    add_executable(gameinput_probe test/gameinput_probe.cpp shims/gameinput.cpp shims/registry.cpp)
    foreach(_gi gameinput_test gameinput_probe)
      target_include_directories(${_gi} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
      target_compile_definitions(${_gi} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(${_gi} PRIVATE /W4 /permissive- /utf-8)
      target_link_options(${_gi} PRIVATE /OPT:NOREF)
      target_link_libraries(${_gi} PRIVATE SDL3::SDL3-static)
    endforeach()
  else()
    if(LEONIDA_SDL3)
      message(STATUS "GameInput shim not built: no installed SDL3 found. Set SDL3_DIR, or pass -DLEONIDA_SDL3_FETCH=ON "
                     "to download the pinned SDL3 release (see docs/gameinput.md)")
    else()
      message(STATUS "GameInput shim not built (LEONIDA_SDL3=OFF)")
    endif()
  endif()

elseif(NOT WIN32)
  message(STATUS "gta6rt requires Windows; only Vulkan tools are available on Linux")
endif()

include(CTest)
# LEONIDA_TRANSLATOR_DEPS: what the rules that run test/shaders/build.py depend on besides their .asm files.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/translator_deps.cmake")
if(BUILD_TESTING)
  add_executable(gfx10_tiling_test test/gfx10_tiling_test.cpp)
  add_test(NAME gfx10_tiling COMMAND gfx10_tiling_test)

  add_executable(d3d12_xs_frame_events_test test/d3d12_xs_frame_events_test.cpp)
  add_test(NAME d3d12_xs_frame_events COMMAND d3d12_xs_frame_events_test)

  add_executable(retire_tickets_test test/retire_tickets_test.cpp)
  add_test(NAME retire_tickets COMMAND retire_tickets_test)
  # VulkanContext::buffer_at's VA index (runtime/src/buffer_va_index.h): pure stdlib, no Vulkan instance.
  add_executable(buffer_va_index_test test/buffer_va_index_test.cpp)
  add_test(NAME buffer_va_index COMMAND buffer_va_index_test)
  # Textures sharing blocks of device memory (runtime/src/image_memory.h): pure stdlib, counted fake allocations.
  add_executable(image_memory_test test/image_memory_test.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(image_memory_test PRIVATE Threads::Threads)
  add_test(NAME image_memory COMMAND image_memory_test)

  add_executable(xbox_present_readback_size_test test/xbox_present_readback_size_test.cpp)
  add_test(NAME xbox_present_readback_size COMMAND xbox_present_readback_size_test)

  # The title's own window, where PresentX shows its frames (shims/title_window.h): user32 only, no device.
  if(WIN32)
    add_executable(title_window_test test/title_window_test.cpp)
    target_include_directories(title_window_test PRIVATE shims)
    target_compile_definitions(title_window_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if(MSVC)
      target_compile_options(title_window_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    target_link_libraries(title_window_test PRIVATE user32)
    add_test(NAME title_window COMMAND title_window_test)

    # LEONIDA_TITLE_HUD's counters in the title bar of another thread's window (shims/title_hud.h): user32 and DXGI.
    add_executable(title_hud_test test/title_hud_test.cpp)
    target_include_directories(title_hud_test PRIVATE shims)
    target_compile_definitions(title_hud_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if(MSVC)
      target_compile_options(title_hud_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    target_link_libraries(title_hud_test PRIVATE Vulkan::Headers user32 psapi)
    add_test(NAME title_hud COMMAND title_hud_test)
  endif()

  # LEONIDA_DEV_OVERLAY's text and the phase bookkeeping behind it (src/dev_overlay.h): no window, GPU or game file.
  find_package(Threads REQUIRED)
  add_executable(dev_overlay_test test/dev_overlay_test.cpp)
  target_include_directories(dev_overlay_test PRIVATE src)
  target_link_libraries(dev_overlay_test PRIVATE Threads::Threads)
  if(WIN32)
    target_link_libraries(dev_overlay_test PRIVATE user32 gdi32 psapi)
  endif()
  add_test(NAME dev_overlay COMMAND dev_overlay_test)

  add_executable(pipeline_cache_file_test test/pipeline_cache_file_test.cpp)
  add_test(NAME pipeline_cache_file COMMAND pipeline_cache_file_test)
  # The pool that compiles a submission's pipelines (runtime/src/pipeline_workers.h): pure stdlib.
  add_executable(pipeline_workers_test test/pipeline_workers_test.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(pipeline_workers_test PRIVATE Threads::Threads)
  add_test(NAME pipeline_workers COMMAND pipeline_workers_test)
  set_tests_properties(pipeline_workers PROPERTIES TIMEOUT 120)

  # D3D-704's session-cache limit and header check: pure stdlib, no Vulkan headers.
  add_executable(pipeline_cache_store_test test/pipeline_cache_store_test.cpp)
  add_test(NAME pipeline_cache_store COMMAND pipeline_cache_store_test)

  add_executable(heavy_pipeline_cache_test test/heavy_pipeline_cache_test.cpp)
  add_test(NAME heavy_pipeline_cache COMMAND heavy_pipeline_cache_test)

  # RT-PIPELINE-PREWARM-ORDER phase 1: the file format and the recorder of src/pipeline_order.h (pure stdlib, no GPU).
  add_executable(pipeline_order_test test/pipeline_order_test.cpp)
  target_link_libraries(pipeline_order_test PRIVATE Threads::Threads)
  add_test(NAME pipeline_order COMMAND pipeline_order_test)
  set_tests_properties(pipeline_order PROPERTIES TIMEOUT 120)

  # The per-pipeline journal files compute pipelines are kept in: naming, header check, atomic replacement.
  add_executable(pipeline_cache_journal_test test/pipeline_cache_journal_test.cpp)
  add_test(NAME pipeline_cache_journal COMMAND pipeline_cache_journal_test)

  # The compile helper's request files and building markers (shims/d3d12_compile_helper.h): Win32 file calls only.
  if(WIN32)
    add_executable(compile_helper_test test/compile_helper_test.cpp)
    target_compile_definitions(compile_helper_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_test(NAME compile_helper COMMAND compile_helper_test)
  endif()

  # D24/D16 fragment depth quantisation and bias SPIR-V patcher (runtime/src/depth24_fragment.h): pure stdlib.
  add_executable(depth24_fragment_test test/depth24_fragment_test.cpp)
  add_test(NAME depth24_fragment COMMAND depth24_fragment_test)

  add_executable(xdxr_build_output_test test/xdxr_build_output_test.cpp)
  add_test(NAME xdxr_build_output COMMAND xdxr_build_output_test)

  # Which pipelines LEONIDA_ASYNC_PIPELINES=1 builds asynchronously by size (LEONIDA_ASYNC_MIN_BYTES and _GRAPHICS,
  # src/async_pipeline_size.h): pure stdlib.
  add_executable(async_pipeline_size_test test/async_pipeline_size_test.cpp)
  add_test(NAME async_pipeline_size COMMAND async_pipeline_size_test)

  # The gate of large_module_gate.h as the runtime uses it for large pipelines (LEONIDA_BIG_COMPILE_BUDGET_MB): pure
  # stdlib.
  add_executable(big_compile_gate_test test/big_compile_gate_test.cpp)
  target_link_libraries(big_compile_gate_test PRIVATE Threads::Threads)
  add_test(NAME big_compile_gate COMMAND big_compile_gate_test)
  set_tests_properties(big_compile_gate PROPERTIES TIMEOUT 120)
  # LEONIDA_COMPILE_GATE=auto (src/compile_gate_policy.h): the setting, the calibration verdict, its cache file and the
  # slow pipeline's SPIR-V: pure stdlib, no Vulkan device.
  add_executable(compile_gate_policy_test test/compile_gate_policy_test.cpp)
  add_test(NAME compile_gate_policy COMMAND compile_gate_policy_test)
  # vk_pipeline_check's memory budget for large modules: pure stdlib, no Vulkan device.
  add_executable(large_module_gate_test test/large_module_gate_test.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(large_module_gate_test PRIVATE Threads::Threads)
  add_test(NAME large_module_gate COMMAND large_module_gate_test)
  # vk_pipeline_check's isolate summary stage set: pure stdlib, no Vulkan device.
  add_executable(pipeline_summary_test test/pipeline_summary_test.cpp)
  add_test(NAME pipeline_summary COMMAND pipeline_summary_test)
  # XDXR build descriptions (docs/xdxr.md); compared with d3d12.h where the Windows SDK provides it.
  add_executable(xdxr_descs_test test/xdxr_descs_test.cpp)
  add_test(NAME xdxr_descs COMMAND xdxr_descs_test)
  # LEONIDA_VK_DEVICE name matching (runtime/src/vulkan_device_filter.h): pure stdlib, no Vulkan instance.
  add_executable(vulkan_device_filter_test test/vulkan_device_filter_test.cpp)
  target_include_directories(vulkan_device_filter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME vulkan_device_filter COMMAND vulkan_device_filter_test)
  # LEONIDA_HOST_IMAGE_COPY's choice (runtime/src/host_image_copy_policy.h): pure stdlib, no Vulkan instance.
  add_executable(host_image_copy_policy_test test/host_image_copy_policy_test.cpp)
  target_include_directories(host_image_copy_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME host_image_copy_policy COMMAND host_image_copy_policy_test)
  add_executable(title_texture_refresh_test test/title_texture_refresh_test.cpp)
  add_test(NAME title_texture_refresh COMMAND title_texture_refresh_test)
  add_executable(vk_presenter_capture_colour_test test/vk_presenter_capture_colour_test.cpp)
  add_test(NAME vk_presenter_capture_colour COMMAND vk_presenter_capture_colour_test)
  if(TARGET gta6rt)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
      add_executable(cpp_unwind_test test/cpp_unwind_test.cpp)
      add_test(NAME cpp_unwind COMMAND cpp_unwind_test)
    endif()

    # Compile-time ABI checks (D3D12/XMem structure sizes, alignments and member offsets against the Windows SDK).
    # The file existed but no target built it, so none of its static_asserts ever ran.
    add_executable(runtime_types_test test/test_runtime_types.cpp)
    target_include_directories(runtime_types_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/shims)
    add_test(NAME runtime_types COMMAND runtime_types_test)

    # Loader tests. They build their own PE images, so they need neither the game nor any other input.
    add_executable(loader_test
      test/loader_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
      shims/library_loader.cpp  # the run-time LEONIDA_COMPAT_DIR cases call its LoadLibrary / GetProcAddress
    )
    target_compile_definitions(loader_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(loader_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(loader_test PRIVATE /OPT:NOREF ${GTA6RT_HOST_BASE})
    add_dependencies(loader_test gta6rt_tls_host)
    add_test(NAME loader COMMAND loader_test)
    # It takes about 10 s under Wine and under a minute on a desktop with a virus scanner. Before #239 it waited on
    # a "Bad Image" box on the Windows runner until the job's own timeout cancelled the whole job (every Windows
    # run from 24 Nov 00:37 to 03:32 UTC); a hang like that now fails this test instead.
    set_tests_properties(loader PROPERTIES TIMEOUT 180)
    # PE loader tests use two locally built fixtures: a DLL with exports, imports and relocations, and an EXE that
    # imports it so Loader::load exercises recursive module loading. They contain no game data.
    set(TINY_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>")

    add_library(tiny_dll SHARED test/fixtures/tiny_dll.cpp)
    target_compile_definitions(tiny_dll PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_dll PRIVATE /W4 /permissive- /utf-8 /GS-)
    # tiny_fault_caught faults inside its own __try. MSVC always catches that; clang-cl only with asynchronous
    # exceptions, otherwise the access violation escapes and the unwind-registration check fails.
    target_compile_options(tiny_dll PRIVATE $<$<CXX_COMPILER_ID:Clang>:/EHa>)
    set_target_properties(tiny_dll PROPERTIES
      OUTPUT_NAME tiny_dll
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    add_executable(tiny_exe test/fixtures/tiny_exe.cpp)
    target_link_libraries(tiny_exe PRIVATE tiny_dll)
    target_compile_definitions(tiny_exe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_exe PRIVATE /W4 /permissive- /utf-8 /GS-)
    set_target_properties(tiny_exe PROPERTIES
      OUTPUT_NAME tiny_exe
      RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    # An EXE built with /GA reads its TLS at index 0 without _tls_index; loader_tls_test maps it and reads it from
    # threads that existed before load() and from ones that did not.
    add_executable(tiny_ga_tls test/fixtures/tiny_ga_tls.cpp)
    target_compile_definitions(tiny_ga_tls PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_ga_tls PRIVATE /W4 /permissive- /utf-8 /GS- /GA)
    # /GA can remove the _tls_index reference that pulls in the CRT TLS directory.
    target_link_options(tiny_ga_tls PRIVATE /INCLUDE:_tls_used)
    set_target_properties(tiny_ga_tls PROPERTIES
      OUTPUT_NAME tiny_ga_tls
      RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    add_executable(module_names_test test/module_names_test.cpp src/pe_loader.cpp shims/registry.cpp)
    target_include_directories(module_names_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(module_names_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(module_names_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(module_names_test PRIVATE /OPT:NOREF)
    add_dependencies(module_names_test gta6rt_tls_host tiny_dll)
    add_test(NAME module_names COMMAND module_names_test "$<TARGET_FILE:tiny_dll>")

    add_executable(tiny_module_names test/fixtures/tiny_module_names.cpp)
    target_compile_definitions(tiny_module_names PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_module_names PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_target_properties(tiny_module_names PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}")

    # Native Windows and --dry-run/--init/--run share original renamed fixtures, including an ACP-named import.
    if(NOT CMAKE_CROSSCOMPILING)
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_FOUND)
        add_test(NAME run_module_names COMMAND "${Python3_EXECUTABLE}" -I
          "${CMAKE_CURRENT_SOURCE_DIR}/test/check_module_names.py"
          "--loader=$<TARGET_FILE:gta6rt>" "--guest=$<TARGET_FILE:tiny_module_names>"
          "--importer=$<TARGET_FILE:tiny_exe>" "--dependency=$<TARGET_FILE:tiny_dll>"
          "--work-dir=${CMAKE_CURRENT_BINARY_DIR}"
          "--report=${CMAKE_CURRENT_BINARY_DIR}/module-names-results.json")
        set_tests_properties(run_module_names PROPERTIES TIMEOUT 90)
      endif()
    endif()

    # pe_loader.cpp needs the shim registry for shims::find/make_unimplemented_thunk. The xmem and d3d12_xs tables
    # are linked too so the optional LEONIDA_GAME_DIR run classifies the shipped modules' imports exactly like
    # gta6rt does (no d3d12.lib is needed: d3d12_xs resolves its host entry points at run time).
    add_executable(pe_loader_test
      test/pe_loader_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
      shims/xmem.cpp
      shims/d3d12_xs.cpp
      shims/d3d12_xs_device.cpp
      shims/d3d12_xs_pso_stream.cpp
    )
    target_include_directories(pe_loader_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/shims
    )
    target_compile_definitions(pe_loader_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(pe_loader_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(pe_loader_test PRIVATE /OPT:NOREF)
    target_link_libraries(pe_loader_test PRIVATE gta6rt_d3d12_xs_rootsig psapi)
    add_dependencies(pe_loader_test tiny_dll tiny_exe gta6rt_tls_host)

    # `gta6rt --run` rehearsal: a stand-in game EXE (CRT startup, statics, heap, files, threads) that imports
    # xgameruntime.dll from a link-time stub; under --run the shim registry answers those imports instead.
    add_library(xgameruntime_stub SHARED test/fixtures/xgameruntime_stub.cpp test/fixtures/xgameruntime_stub.def)
    target_include_directories(xgameruntime_stub PUBLIC ${XTASKQUEUE_DIR}/Include)
    target_compile_definitions(xgameruntime_stub PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(xgameruntime_stub PROPERTIES
      OUTPUT_NAME xgameruntime
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
    )
    add_executable(tiny_async test/fixtures/tiny_async.cpp)
    target_link_libraries(tiny_async PRIVATE xgameruntime_stub)
    set_target_properties(tiny_async PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
    )
    # Reaches xgameruntime.dll through LoadLibraryW/GetProcAddress, as xgameruntime.lib's thunks do (no import).
    add_executable(tiny_gdk_load test/fixtures/tiny_gdk_load.cpp)
    target_compile_definitions(tiny_gdk_load PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_gdk_load PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(tiny_gdk_load PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
    )
    # Run-time LoadLibrary of a bundled Xbox DLL (importing xmem.dll) through rt::Loader.
    add_library(xmem_stub SHARED test/fixtures/xmem_stub.cpp test/fixtures/xmem_stub.def)
    target_compile_definitions(xmem_stub PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(xmem_stub PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(xmem_stub PROPERTIES
      OUTPUT_NAME xmem
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/stubs/$<CONFIG>"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/stubs/$<CONFIG>"
    )
    add_library(tiny_bundled_dll SHARED test/fixtures/tiny_bundled_dll.cpp test/fixtures/tiny_bundled_dll.def)
    target_link_libraries(tiny_bundled_dll PRIVATE xmem_stub)
    target_compile_definitions(tiny_bundled_dll PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_bundled_dll PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(tiny_bundled_dll PROPERTIES
      OUTPUT_NAME tiny_bundled_dll
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/stubs/$<CONFIG>"
    )
    add_executable(tiny_bundled_load test/fixtures/tiny_bundled_load.cpp)
    target_compile_definitions(tiny_bundled_load PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_bundled_load PRIVATE /W4 /permissive- /utf-8)
    add_dependencies(tiny_bundled_load tiny_bundled_dll)
    set_target_properties(tiny_bundled_load PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
    )
    add_library(tiny_startup_cmdline_dll SHARED test/fixtures/tiny_startup_cmdline.cpp)
    target_compile_definitions(tiny_startup_cmdline_dll PRIVATE STARTUP_CMDLINE_DLL)
    add_executable(tiny_startup_cmdline test/fixtures/tiny_startup_cmdline.cpp)
    target_link_libraries(tiny_startup_cmdline PRIVATE tiny_startup_cmdline_dll)
    foreach(target tiny_startup_cmdline_dll tiny_startup_cmdline)
      target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
      set_target_properties(${target} PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
      )
    endforeach()
    # These linker-selected startup modes are specific to the MSVC CRT.
    if(MSVC)
      foreach(width a w)
        foreach(mode default expand noargs)
          set(target tiny_argv_${mode}_${width})
          add_executable(${target} test/fixtures/tiny_argv_modes.cpp)
          target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
          target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
          if(width STREQUAL "w")
            target_compile_definitions(${target} PRIVATE APP_WIDE)
          endif()
          if(mode STREQUAL "expand")
            if(width STREQUAL "w")
              target_link_options(${target} PRIVATE wsetargv.obj)
            else()
              target_link_options(${target} PRIVATE setargv.obj)
            endif()
          elseif(mode STREQUAL "noargs")
            target_link_options(${target} PRIVATE noarg.obj)
          endif()
          set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>")
        endforeach()
      endforeach()
    endif()
    add_executable(tiny_atexit test/fixtures/tiny_atexit.cpp)
    target_compile_definitions(tiny_atexit PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_atexit PRIVATE /W4 /permissive- /utf-8 /EHsc)  # a handler that throws
    set_target_properties(tiny_atexit PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
    )
    add_executable(tiny_game test/fixtures/tiny_game.cpp)
    target_link_libraries(tiny_game PRIVATE xgameruntime_stub)
    target_compile_definitions(tiny_game PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)  # plain fopen, as games use
    target_compile_options(tiny_game PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_target_properties(tiny_game PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
    )
    # The same game with no .reloc, at the default EXE base, as RT-22 asks about the real one: it can only run at
    # 0x140000000, which arm64 Wine on macOS uses for itself (src/base_reservation.h).
    add_executable(tiny_game_fixed test/fixtures/tiny_game.cpp)
    target_link_libraries(tiny_game_fixed PRIVATE xgameruntime_stub)
    target_compile_definitions(tiny_game_fixed PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(tiny_game_fixed PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(tiny_game_fixed PRIVATE /FIXED /DYNAMICBASE:NO /BASE:0x140000000)
    set_target_properties(tiny_game_fixed PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
    )
    # The same game built with /GA, as a title's executable is: its thread_local reads go to module TLS index 0
    # without _tls_index, and under gta6rt index 0 is gta6rt.exe's own (docs/boot_leads.md, BL-1).
    add_executable(tiny_game_ga test/fixtures/tiny_game.cpp)
    target_link_libraries(tiny_game_ga PRIVATE xgameruntime_stub)
    target_compile_definitions(tiny_game_ga PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(tiny_game_ga PRIVATE /W4 /permissive- /utf-8 /EHsc /GA)
    # /GA can remove the _tls_index reference that pulls in the CRT TLS directory.
    target_link_options(tiny_game_ga PRIVATE /INCLUDE:_tls_used)
    set_target_properties(tiny_game_ga PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
    )
    add_executable(tiny_game_exit_code_test test/tiny_game_exit_code_test.cpp)
    target_compile_definitions(tiny_game_exit_code_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_game_exit_code_test PRIVATE /W4 /permissive- /utf-8)
    add_dependencies(tiny_game_exit_code_test gta6rt tiny_game tiny_game_fixed)
    # abort() must reach the guest's SIGABRT handler before the run ends with the CRT's abort code (3).
    add_executable(tiny_game_abort_test test/tiny_game_abort_test.cpp)
    target_compile_definitions(tiny_game_abort_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_game_abort_test PRIVATE /W4 /permissive- /utf-8)
    add_dependencies(tiny_game_abort_test gta6rt tiny_game)
    add_test(NAME pe_loader COMMAND pe_loader_test "$<TARGET_FILE:tiny_dll>" "$<TARGET_FILE:tiny_exe>")

    # Synthetic, hand-built images through Loader::load: range checks, wrapping RVAs, import-table termination and
    # base relocations (HIGHLOW included). No fixture DLL is needed; the images are written to a temporary folder.
    add_executable(test_pe_loader_bounds
      test/test_pe_loader_bounds.cpp
      src/pe_loader.cpp
      shims/registry.cpp
      shims/xmem.cpp
      shims/d3d12_xs.cpp
      shims/d3d12_xs_device.cpp
      shims/d3d12_xs_pso_stream.cpp
    )
    target_include_directories(test_pe_loader_bounds PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/shims
    )
    target_compile_definitions(test_pe_loader_bounds PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(test_pe_loader_bounds PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(test_pe_loader_bounds PRIVATE /OPT:NOREF ${GTA6RT_HOST_BASE})
    target_link_libraries(test_pe_loader_bounds PRIVATE gta6rt_d3d12_xs_rootsig psapi)
    add_dependencies(test_pe_loader_bounds gta6rt_tls_host)
    add_test(NAME pe_loader_bounds COMMAND test_pe_loader_bounds)

    # RT-388: a deterministic mutation fuzzer over synthetic images through Loader::load (test/pe_loader_fuzz.cpp).
    # ctest runs a fixed seed and case count; docs/testing.md has the long runs. With LEONIDA_LOADER_FUZZ_ASAN the
    # fuzzer and the loader sources it compiles are built with AddressSanitizer (use RelWithDebInfo or Release:
    # /fsanitize=address does not combine with the /RTC1 of a Debug build).
    option(LEONIDA_LOADER_FUZZ_ASAN "Build pe_loader_fuzz with AddressSanitizer" OFF)
    add_executable(pe_loader_fuzz
      test/pe_loader_fuzz.cpp
      src/pe_loader.cpp
      shims/registry.cpp
      shims/xmem.cpp
      shims/d3d12_xs.cpp
      shims/d3d12_xs_device.cpp
      shims/d3d12_xs_pso_stream.cpp
    )
    target_include_directories(pe_loader_fuzz PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/shims
    )
    target_compile_definitions(pe_loader_fuzz PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(pe_loader_fuzz PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(pe_loader_fuzz PRIVATE /OPT:NOREF ${GTA6RT_HOST_BASE})
    target_link_libraries(pe_loader_fuzz PRIVATE gta6rt_d3d12_xs_rootsig psapi)
    add_dependencies(pe_loader_fuzz gta6rt_tls_host)
    if(LEONIDA_LOADER_FUZZ_ASAN)
      target_compile_options(pe_loader_fuzz PRIVATE /fsanitize=address /Zi)
      target_link_options(pe_loader_fuzz PRIVATE /INCREMENTAL:NO /DEBUG)
      # No STL container annotations: they need stl_asan.lib from the VS component, and the libraries linked here
      # are built without ASan, so the annotate_string/annotate_vector /FAILIFMISMATCH keys must stay as theirs.
      target_compile_definitions(pe_loader_fuzz PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
      # The ASan runtime: next to cl.exe for MSVC (the VS "C++ AddressSanitizer" component), under the resource
      # directory for clang-cl. Its objects name the runtime libraries; the folder goes on the library path, and the
      # DLL is copied beside the test so it runs without a Developer prompt or a PATH change.
      get_filename_component(_leonida_cc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
      file(GLOB _leonida_asan_dll
        "${_leonida_cc_dir}/clang_rt.asan_dynamic-x86_64.dll"
        "${_leonida_cc_dir}/../lib/clang/*/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
      if(_leonida_asan_dll)
        list(GET _leonida_asan_dll 0 _leonida_asan_dll)
        get_filename_component(_leonida_asan_dir "${_leonida_asan_dll}" DIRECTORY)
        target_link_directories(pe_loader_fuzz PRIVATE "${_leonida_asan_dir}")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
          # clang-cl 23's stack-use-after-return frames break MSVC C++ exception dispatch at /O2: a rethrow faults
          # in vcruntime's _C_specific_handler_noexcept (a five-line try/throw/catch does it). Without them it works.
          target_compile_options(pe_loader_fuzz PRIVATE -fsanitize-address-use-after-return=never)
          # clang-cl names these only when it runs the link itself; CMake calls the linker directly.
          target_link_options(pe_loader_fuzz PRIVATE clang_rt.asan_dynamic-x86_64.lib
                              /wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
        endif()
        add_custom_command(TARGET pe_loader_fuzz POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_leonida_asan_dll}" "$<TARGET_FILE_DIR:pe_loader_fuzz>")
      else()
        message(WARNING "LEONIDA_LOADER_FUZZ_ASAN: no clang_rt.asan_dynamic-x86_64.dll beside ${CMAKE_CXX_COMPILER}")
      endif()
    endif()
    add_test(NAME pe_loader_fuzz COMMAND pe_loader_fuzz --quiet)

    # Static TLS fixture: a DLL with __declspec(thread) and a TLS callback. It is ordinary test code; no game data
    # is involved. loader_tls_test needs gta6rt_tls_host.dll next to its own EXE, because that is where
    # Loader::initialize_tls looks for the reserved module TLS index.
    add_library(tiny_tls SHARED test/fixtures/tiny_tls.cpp)
    target_compile_definitions(tiny_tls PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_tls PRIVATE /W4 /permissive- /utf-8 /GS-)
    set_target_properties(tiny_tls PROPERTIES
      OUTPUT_NAME tiny_tls
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
    )

    add_library(tiny_tls_cb_only SHARED test/fixtures/tiny_tls_cb_only.cpp)
    target_compile_definitions(tiny_tls_cb_only PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_tls_cb_only PRIVATE /W4 /permissive- /utf-8 /GS-)
    target_link_options(tiny_tls_cb_only PRIVATE /NODEFAULTLIB /ENTRY:DllMainCRTStartup)
    target_link_libraries(tiny_tls_cb_only PRIVATE kernel32)
    set_target_properties(tiny_tls_cb_only PROPERTIES
      OUTPUT_NAME tiny_tls_cb_only
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
    )

    # pe_loader.cpp needs the shim registry for shims::find/make_unimplemented_thunk. The fixtures import only host
    # DLLs, so no shim tables are linked.
    add_executable(loader_tls_test
      test/loader_tls_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
    )
    target_include_directories(loader_tls_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(loader_tls_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(loader_tls_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    add_dependencies(loader_tls_test tiny_tls tiny_tls_cb_only tiny_ga_tls gta6rt_tls_host)
    add_test(NAME loader_tls
      COMMAND loader_tls_test "$<TARGET_FILE:tiny_tls>"
    )

    # Module TLS index 0 fixture: an EXE built /GA, so its thread_local reads go straight to
    # ThreadLocalStoragePointer[0] as a game executable's can. The loader must give a mapped executable index 0;
    # loader_tls_index0_test maps it next to tiny_tls.dll and run_tiny_tls_ga runs it under gta6rt. Ordinary test
    # code, no game data. The test checks the /GA code shape only where the compiler is MSVC, which implements /GA.
    add_executable(tiny_tls_ga test/fixtures/tiny_tls_ga.cpp)
    target_compile_options(tiny_tls_ga PRIVATE /W4 /permissive- /utf-8 /EHsc /GA)
    set_target_properties(tiny_tls_ga PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
    )
    add_executable(loader_tls_index0_test
      test/loader_tls_index0_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
    )
    target_include_directories(loader_tls_index0_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(loader_tls_index0_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
      $<$<CXX_COMPILER_ID:MSVC>:LEONIDA_GA_FIXTURE_IS_GA=1>)
    target_compile_options(loader_tls_index0_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    # Loaded by the threads scenario before the TLS helper: its DllMain(DLL_THREAD_ATTACH) runs test code first.
    add_library(tiny_thread_hook SHARED test/fixtures/tiny_thread_hook.cpp)
    target_compile_definitions(tiny_thread_hook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(tiny_thread_hook PRIVATE /W4 /permissive- /utf-8 /GS-)
    set_target_properties(tiny_thread_hook PROPERTIES
      OUTPUT_NAME tiny_thread_hook
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
    )
    add_dependencies(loader_tls_index0_test tiny_tls_ga tiny_tls tiny_thread_hook gta6rt_tls_host)
    # Imports tiny_tls.dll and tiny_tls_cb_only.dll, so gta6rt needs a helper clone for one of them
    # (check_tls_clone_cleanup.cmake).
    add_executable(tiny_tls_clones test/fixtures/tiny_tls_clones.cpp)
    target_link_libraries(tiny_tls_clones PRIVATE tiny_tls tiny_tls_cb_only)
    target_compile_options(tiny_tls_clones PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(tiny_tls_clones PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
    )
    # One process per scenario: the reservation is once per process. loader_tls_index0 (runs) reserves at start,
    # loader_tls_index0_threads reserves while threads are being created, loader_tls_index0_noreserve never does.
    foreach(scenario runs threads noreserve)
      if(scenario STREQUAL "runs")
        set(name loader_tls_index0)
      else()
        set(name loader_tls_index0_${scenario})
      endif()
      add_test(NAME ${name}
        COMMAND loader_tls_index0_test ${scenario} "$<TARGET_FILE:tiny_tls_ga>" "$<TARGET_FILE:tiny_tls>"
                "$<TARGET_FILE:tiny_thread_hook>"
      )
    endforeach()

    # DisableThreadLibraryCalls from a mapped DllMain (shims/library_loader.cpp). The fixture is built without the CRT, so
    # DllMain's only work is that one call. No game files.
    add_library(dll_thread_calls SHARED test/fixtures/dll_thread_calls.cpp)
    target_compile_options(dll_thread_calls PRIVATE /W4 /permissive- /utf-8 /GS-)
    target_link_options(dll_thread_calls PRIVATE /NODEFAULTLIB /ENTRY:dll_thread_calls_main)
    target_link_libraries(dll_thread_calls PRIVATE kernel32)
    set_target_properties(dll_thread_calls PROPERTIES
      OUTPUT_NAME dll_thread_calls
      PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
    )
    add_executable(dll_thread_calls_test
      test/dll_thread_calls_test.cpp
      src/pe_loader.cpp
      shims/library_loader.cpp
      shims/registry.cpp
      shims/xmem.cpp
    )
    target_include_directories(dll_thread_calls_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/shims
    )
    target_compile_definitions(dll_thread_calls_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dll_thread_calls_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(dll_thread_calls_test PRIVATE /OPT:NOREF)
    target_link_libraries(dll_thread_calls_test PRIVATE psapi)
    add_dependencies(dll_thread_calls_test dll_thread_calls)
    add_test(NAME dll_thread_calls
      COMMAND dll_thread_calls_test "$<TARGET_FILE:dll_thread_calls>"
    )

    # The host GameInput path (docs/gameinput.md): mapped code that imports GameInputCreate from GameInput.dll, with no
    # GameInput shim linked. Needs the SDK's gameinput.lib and GameInput.h to build the fixture, and a GameInput.dll on
    # the machine to run (the test skips with 77 only when Windows cannot load GameInput.dll). No game files.
    find_library(GAMEINPUT_LIBRARY NAMES gameinput HINTS ${_sdk_lib_hints})
    if(GAMEINPUT_LIBRARY)
      add_library(gameinput_host SHARED test/fixtures/gameinput_host.cpp)
      target_compile_options(gameinput_host PRIVATE /W4 /permissive- /utf-8 /GS-)
      target_link_options(gameinput_host PRIVATE /NODEFAULTLIB /ENTRY:gameinput_host_main)
      target_link_libraries(gameinput_host PRIVATE kernel32 ${GAMEINPUT_LIBRARY})
      set_target_properties(gameinput_host PROPERTIES
        OUTPUT_NAME gameinput_host
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${TINY_FIXTURE_DIR}"
      )
      add_executable(gameinput_host_test
        test/gameinput_host_test.cpp
        src/pe_loader.cpp
        shims/registry.cpp
      )
      target_include_directories(gameinput_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_compile_definitions(gameinput_host_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(gameinput_host_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
      add_dependencies(gameinput_host_test gameinput_host)
      add_test(NAME gameinput_host COMMAND gameinput_host_test "$<TARGET_FILE:gameinput_host>")
      set_tests_properties(gameinput_host PROPERTIES SKIP_RETURN_CODE 77)  # Windows cannot load GameInput.dll
    endif()

    # LEONIDA_UNIMPLEMENTED=log|fail|stop: builds its own image with two imports that bind to nothing, calls them in
    # process (log, fail) and in child processes (default/env parsing, stop). No game files, no GPU.
    add_executable(unimplemented_policy_test
      test/unimplemented_policy_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
    )
    # The register-preservation check needs a caller that sets rbx/rbp/rdi/rsi/r12-r15 and xmm6-15, which MSVC x64
    # cannot do inline: test/fixtures/abi_probe.asm (MASM). Without ml64 the rest of the test still runs.
    # Visual Studio locates cl.exe without adding its directory to PATH.
    # Keep looking for ml64 specifically so Ninja cannot select the 32-bit assembler.
    get_filename_component(_gta6rt_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(LEONIDA_ML64 NAMES ml64 HINTS "${_gta6rt_compiler_bin}")
    if(LEONIDA_ML64)
      set(CMAKE_ASM_MASM_COMPILER "${LEONIDA_ML64}")
      enable_language(ASM_MASM)
      target_sources(unimplemented_policy_test PRIVATE test/fixtures/abi_probe.asm)
      target_compile_definitions(unimplemented_policy_test PRIVATE LEONIDA_HAVE_ABI_PROBE=1)
    else()
      message(STATUS "ml64 not found: unimplemented_policy_test skips the register-preservation check")
    endif()
    target_include_directories(unimplemented_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(unimplemented_policy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(unimplemented_policy_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /EHsc>)
    target_link_options(unimplemented_policy_test PRIVATE /OPT:NOREF)
    add_dependencies(unimplemented_policy_test gta6rt_tls_host)
    add_test(NAME unimplemented_policy COMMAND unimplemented_policy_test)

    # gta6rt --trace FILE (shims/startup_trace.h): a synthetic image with an unimplemented import, an
    # OutputDebugStringA call and a read of address 0; checks the JSON lines. No game files, no GPU.
    add_executable(startup_trace_test
      test/startup_trace_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
    )
    target_include_directories(startup_trace_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(startup_trace_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(startup_trace_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(startup_trace_test PRIVATE /OPT:NOREF)
    add_dependencies(startup_trace_test gta6rt_tls_host)
    add_test(NAME startup_trace COMMAND startup_trace_test)
  endif()

  if(WIN32)
    add_executable(present_window_test test/present_window_test.cpp)
    target_link_libraries(present_window_test PRIVATE user32)
    add_test(NAME present_window COMMAND present_window_test)
    set_tests_properties(present_window PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)
  endif()

  if(TARGET device_proxy_test)
    add_test(NAME device_proxy COMMAND device_proxy_test)
    set_tests_properties(device_proxy PROPERTIES SKIP_RETURN_CODE 77)  # no WARP adapter (Wine)
    # With --trace-d3d12 the 24 D3D12XboxCreateDevice parameter words still print (docs/boot.md, question 1),
    # and the line with run-specific pointers does not.
    add_test(NAME device_proxy_trace COMMAND device_proxy_test --trace-d3d12)
    set_tests_properties(device_proxy_trace PROPERTIES SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "D3D12XboxCreateDevice params\\[24\\] =1234abcd"
        FAIL_REGULAR_EXPRESSION "FAIL;D3D12XboxCreateDevice adapter=")
  endif()
  if(TARGET xs_proxy_mock_test)
    add_test(NAME xs_proxy_mock COMMAND xs_proxy_mock_test)
    # The guard region's setup failing (one process each: the tables are built once per process).
    add_test(NAME xs_proxy_mock_no_reserve COMMAND xs_proxy_mock_test --fail-virtualalloc)
    add_test(NAME xs_proxy_mock_no_protect COMMAND xs_proxy_mock_test --fail-virtualprotect)
    add_test(NAME xs_proxy_mock_no_handler COMMAND xs_proxy_mock_test --fail-veh)
    # The routes are read once per process, so each override case is its own test.
    add_test(NAME xs_proxy_mock_env_slots COMMAND xs_proxy_mock_test --env-slots)
    add_test(NAME xs_proxy_mock_env_slots_invalid COMMAND xs_proxy_mock_test --env-slots-invalid)
    add_test(NAME xs_proxy_mock_env_slots_70 COMMAND xs_proxy_mock_test --env-slots-70)
    add_test(NAME xs_proxy_mock_env_slots_74 COMMAND xs_proxy_mock_test --env-slots-74)
    # LEONIDA_SLOT133_LAYOUT_WORD=1 (set by the test itself): the slot 133 stand-in reads its fourth word as a layout.
    add_test(NAME xs_proxy_mock_slot133_layout COMMAND xs_proxy_mock_test --slot133-layout)
  endif()
  # Reads d3d12.h and takes MSVC options, so only where the Windows runtime is built.
  if(TARGET gta6rt)
    add_executable(pso_stream_test
      test/pso_stream_test.cpp
      shims/d3d12_xs_pso_stream.cpp
    )
    target_include_directories(pso_stream_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims)
    target_compile_definitions(pso_stream_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(pso_stream_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(pso_stream_test PRIVATE /OPT:NOREF)
    add_test(NAME pso_stream COMMAND pso_stream_test)
  endif()

  if(TARGET root_sig_test)
    add_test(NAME root_sig COMMAND root_sig_test)
  endif()
  if(TARGET root_sig_public_flags_test)
    add_test(NAME root_sig_public_flags COMMAND root_sig_public_flags_test)
  endif()
  add_executable(command_list_test test/command_list_test.cpp)
  add_test(NAME command_list COMMAND command_list_test)
  # Which SPIR-V modules declare a storage image or buffer they may write (src/spirv_stores.h), on hand-built words.
  add_executable(spirv_stores_test test/spirv_stores_test.cpp)
  add_test(NAME spirv_stores COMMAND spirv_stores_test)
  add_executable(list_value_wait_budget_test test/list_value_wait_budget_test.cpp)
  add_test(NAME list_value_wait_budget COMMAND list_value_wait_budget_test)
  add_executable(shader_fixture_path_test test/shader_fixture_path_test.cpp)
  add_test(NAME shader_fixture_path COMMAND shader_fixture_path_test)
  add_executable(resource_tiling_test test/resource_tiling_test.cpp)
  add_test(NAME resource_tiling COMMAND resource_tiling_test)
  add_executable(resource_detile_test test/resource_detile_test.cpp)
  add_test(NAME resource_detile COMMAND resource_detile_test)
  add_executable(descriptor_table_test test/descriptor_table_test.cpp)
  add_test(NAME descriptor_table COMMAND descriptor_table_test)
  add_executable(shader_library_test test/shader_library_test.cpp)
  add_test(NAME shader_library COMMAND shader_library_test)
  add_executable(native_pso_blend_test test/native_pso_blend_test.cpp)
  add_test(NAME native_pso_blend COMMAND native_pso_blend_test)
  add_executable(native_pso_depth_test test/native_pso_depth_test.cpp)
  add_test(NAME native_pso_depth COMMAND native_pso_depth_test)
  if(WIN32)
    # The slot 57 factory retries only a pipeline that blends a B4G4R4A4 stand-in target (d3d12.h types).
    add_executable(native_pso_fallback_test test/native_pso_fallback_test.cpp)
    add_test(NAME native_pso_fallback COMMAND native_pso_fallback_test)
  endif()
  add_executable(spirv_f16_halves_test test/spirv_f16_halves_test.cpp)
  add_test(NAME spirv_f16_halves COMMAND spirv_f16_halves_test)
  add_executable(dual_source_fragment_test test/dual_source_fragment_test.cpp)
  add_test(NAME dual_source_fragment COMMAND dual_source_fragment_test)
  # Optional: skips unless LEONIDA_SHADERS names a translated work folder (<work>/shaders/spirv).
  add_executable(shader_library_corpus_test test/shader_library_corpus_test.cpp)
  add_test(NAME shader_library_corpus COMMAND shader_library_corpus_test)
  # Reads all 26,503 containers and modules: about 9 minutes from a hard disk.
  set_tests_properties(shader_library_corpus PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)
  if(TARGET gta6rt)
    # Optional: skips unless LEONIDA_GAME_DIR and LEONIDA_MOVIE are set.
    add_executable(bink_decode_test
      test/bink_decode_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
      shims/xmem.cpp
    )
    target_compile_definitions(bink_decode_test PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(bink_decode_test PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(bink_decode_test PRIVATE /OPT:NOREF)
    target_link_libraries(bink_decode_test PRIVATE psapi)
    add_dependencies(bink_decode_test gta6rt_tls_host)
    add_test(NAME bink_decode COMMAND bink_decode_test)
    set_tests_properties(bink_decode PROPERTIES SKIP_RETURN_CODE 77)

    # Optional: skips unless LEONIDA_GAME_DIR and LEONIDA_BINK_INDEX are set. Measures the HBINK/BINKSUMMARY
    # offsets and the decode return values over every stream of movies.rpf (docs/movies.md section 5).
    add_executable(bink_abi_test
      test/bink_abi_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
      shims/xmem.cpp
    )
    target_compile_definitions(bink_abi_test PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(bink_abi_test PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(bink_abi_test PRIVATE /OPT:NOREF)
    target_link_libraries(bink_abi_test PRIVATE psapi)
    add_dependencies(bink_abi_test gta6rt_tls_host)
    add_test(NAME bink_abi COMMAND bink_abi_test)
    set_tests_properties(bink_abi PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)

    # Optional local viewer: plays a .bk2 in a window through the package's Bink2Scarlett.dll (test/bink_play.cpp).
    # The CTest entry is the headless smoke run; it skips unless LEONIDA_GAME_DIR and LEONIDA_MOVIE are set.
    add_executable(bink_play
      test/bink_play.cpp
      src/pe_loader.cpp
      shims/registry.cpp
      shims/xmem.cpp
    )
    target_compile_definitions(bink_play PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(bink_play PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(bink_play PRIVATE /OPT:NOREF)
    target_link_libraries(bink_play PRIVATE psapi user32 gdi32)
    add_dependencies(bink_play gta6rt_tls_host)
    add_test(NAME bink_play_headless COMMAND bink_play --headless --frames 30)
    set_tests_properties(bink_play_headless PROPERTIES SKIP_RETURN_CODE 77)

    # Optional: skips unless LEONIDA_GAME_DIR is set. Drives the package's FSR DLL on the Xbox-ABI device proxy.
    if(D3D12_LIBRARY AND DXGI_LIBRARY)
      add_executable(fsr_dispatch_test
        test/fsr_dispatch_test.cpp
        src/pe_loader.cpp
        shims/registry.cpp
        shims/xmem.cpp
        shims/d3d12_xs.cpp
        shims/d3d12_xs_device.cpp
        shims/d3d12_xs_pso_stream.cpp
      )
      target_compile_definitions(fsr_dispatch_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
      target_compile_options(fsr_dispatch_test PRIVATE /W4 /permissive- /EHsc /utf-8)
      target_link_options(fsr_dispatch_test PRIVATE /OPT:NOREF)
      target_link_libraries(fsr_dispatch_test PRIVATE gta6rt_d3d12_xs_rootsig ${D3D12_LIBRARY} ${DXGI_LIBRARY} psapi)
      add_dependencies(fsr_dispatch_test gta6rt_tls_host)
      add_test(NAME fsr_dispatch COMMAND fsr_dispatch_test)
      set_tests_properties(fsr_dispatch PROPERTIES SKIP_RETURN_CODE 77)

      # Optional: skips unless LEONIDA_GAME_DIR is set. Constructs the package's XboxBvhBuilder on the device proxy.
      add_executable(bvh_builder_test
        test/bvh_builder_test.cpp
        src/pe_loader.cpp
        shims/registry.cpp
        shims/xmem.cpp
        shims/d3d12_xs.cpp
        shims/d3d12_xs_device.cpp
        shims/d3d12_xs_pso_stream.cpp
      )
      target_compile_definitions(bvh_builder_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
      target_compile_options(bvh_builder_test PRIVATE /W4 /permissive- /EHsc /utf-8)
      target_link_options(bvh_builder_test PRIVATE /OPT:NOREF)
      target_link_libraries(bvh_builder_test PRIVATE gta6rt_d3d12_xs_rootsig ${D3D12_LIBRARY} ${DXGI_LIBRARY} psapi)
      add_dependencies(bvh_builder_test gta6rt_tls_host)
      add_test(NAME bvh_builder COMMAND bvh_builder_test)
      set_tests_properties(bvh_builder PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # Optional: skips unless LEONIDA_GAME_DIR and LEONIDA_MOVIE_AUDIO are set.
    add_executable(bink_audio_test
      test/bink_audio_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
      shims/xmem.cpp
    )
    target_compile_definitions(bink_audio_test PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(bink_audio_test PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(bink_audio_test PRIVATE /OPT:NOREF)
    target_link_libraries(bink_audio_test PRIVATE psapi)
    add_dependencies(bink_audio_test gta6rt_tls_host)
    add_test(NAME bink_audio COMMAND bink_audio_test)
    set_tests_properties(bink_audio PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  if(WIN32 AND TARGET clzero_patch_test)
    add_test(NAME clzero_patch COMMAND clzero_patch_test)
  endif()
  if(WIN32 AND TARGET cpu_emulation_test)
    add_test(NAME cpu_emulation COMMAND cpu_emulation_test)
  endif()
  if(WIN32 AND TARGET xmem_test)
    add_test(NAME xmem COMMAND xmem_test)
    # The pool read test executes its read from a PE at the title's image base.
    target_link_options(xmem_test PRIVATE /BASE:0x140000000 /DYNAMICBASE:NO)
    add_test(NAME xmem_pool_zero_read COMMAND xmem_test --pool-zero-read)
    add_test(NAME xmem_wine_commit COMMAND xmem_wine_commit_test)
    # The same with write watching off: no reservation is write-watched for the shadows.
    add_test(NAME xmem_no_write_watch COMMAND xmem_test)
    set_tests_properties(xmem_no_write_watch PROPERTIES ENVIRONMENT "LEONIDA_WRITE_WATCH=0")
    # And with it on whatever the host's default (off under Wine).
    add_test(NAME xmem_write_watch COMMAND xmem_test)
    set_tests_properties(xmem_write_watch PROPERTIES ENVIRONMENT "LEONIDA_WRITE_WATCH=1")
  endif()
  if(WIN32 AND TARGET xg_xs_test)
    add_test(NAME xg_xs COMMAND xg_xs_test)
  endif()
  if(WIN32 AND TARGET acphal_test)
    add_test(NAME acphal COMMAND acphal_test)
    add_test(NAME acphal_noncached_only COMMAND acphal_test --noncached-only)
  endif()
  if(WIN32 AND TARGET xapu_chad_test)
    add_test(NAME xapu_chad COMMAND xapu_chad_test)
  endif()
  if(WIN32 AND TARGET xapu_opus_test)
    add_test(NAME xapu_opus COMMAND xapu_opus_test)
  endif()
  if(WIN32 AND TARGET xapu_test)
    add_test(NAME xapu COMMAND xapu_test)
  endif()
  if(WIN32 AND TARGET chad_test)
    add_test(NAME chad COMMAND chad_test)
  endif()
  if(WIN32 AND TARGET process_cmdline_test)
    add_test(NAME process_cmdline COMMAND process_cmdline_test)
  endif()
  if(WIN32 AND TARGET lifecycle_test)
    add_test(NAME lifecycle COMMAND lifecycle_test)
  endif()
  if(WIN32 AND TARGET file_system_test)
    add_test(NAME file_system COMMAND file_system_test)
  endif()
  if(WIN32 AND TARGET int3_skip_test)
    add_test(NAME int3_skip COMMAND int3_skip_test)
  endif()
  if(WIN32 AND TARGET winhttp_offline_test)
    add_test(NAME winhttp_offline COMMAND winhttp_offline_test)
  endif()
  if(WIN32 AND TARGET ws2_offline_test)
    add_test(NAME ws2_offline COMMAND ws2_offline_test)
  endif()
  if(WIN32 AND TARGET com_shim_test)
    add_test(NAME com_shim COMMAND com_shim_test)
  endif()
  if(WIN32 AND TARGET silent_audio_test)
    add_test(NAME silent_audio COMMAND silent_audio_test)
  endif()
  if(WIN32 AND TARGET bcrypt_rng_test)
    add_test(NAME bcrypt_rng COMMAND bcrypt_rng_test)
    add_test(NAME bcrypt_rng_seeded COMMAND bcrypt_rng_test --seeded)
    set_tests_properties(bcrypt_rng_seeded PROPERTIES ENVIRONMENT "LEONIDA_RNG_SEED=0x123456789abcdef0")
  endif()
  if(WIN32 AND TARGET kernel_writes_test)
    add_test(NAME kernel_writes COMMAND kernel_writes_test)
  endif()
  if(WIN32 AND TARGET guarded_copy_test)
    add_test(NAME guarded_copy COMMAND guarded_copy_test)
  endif()
  if(WIN32 AND TARGET title_memory_map_test)
    add_test(NAME title_memory_map COMMAND title_memory_map_test)
  endif()
  if(WIN32 AND TARGET title_write_watch_test)
    add_test(NAME title_write_watch COMMAND title_write_watch_test)
    add_test(NAME title_write_watch_on COMMAND title_write_watch_test)
    set_tests_properties(title_write_watch_on PROPERTIES ENVIRONMENT "LEONIDA_WRITE_WATCH=1")
  endif()
  if(WIN32 AND TARGET thread_stacks_test)
    add_test(NAME thread_stacks COMMAND thread_stacks_test)
    set_tests_properties(thread_stacks PROPERTIES TIMEOUT 60)
  endif()
  if(WIN32 AND TARGET console_registry_test)
    add_test(NAME console_registry COMMAND console_registry_test)
  endif()
  if(WIN32 AND TARGET api_ref_state_test)
    add_test(NAME api_ref_state COMMAND api_ref_state_test)
    set_tests_properties(api_ref_state PROPERTIES TIMEOUT 30)
  endif()
  if(WIN32 AND TARGET api_global_refs_test)
    add_test(NAME api_global_refs COMMAND api_global_refs_test)
    set_tests_properties(api_global_refs PROPERTIES TIMEOUT 30)
  endif()
  # Internal XTaskQueue context pinning and rundown regression tests.
  if(WIN32 AND TARGET gta6rt_xtaskqueue)
    foreach(_context_test context_pinning context_rundown context_reentrant context_concurrent_detach context_attach_detach_stress)
      add_executable(${_context_test}_test
        test/${_context_test}_test.cpp
      )
      target_include_directories(${_context_test}_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/shims/xtaskqueue
        ${XTASKQUEUE_DIR}/Source/Task
        ${XTASKQUEUE_DIR}/Source/Common
      )
      target_compile_definitions(${_context_test}_test PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX
      )
      target_compile_options(${_context_test}_test PRIVATE
        /W4 /permissive- /utf-8 /EHsc
      )
      target_link_libraries(${_context_test}_test PRIVATE
        gta6rt_xtaskqueue
      )
      add_test(NAME ${_context_test}
        COMMAND ${_context_test}_test
      )
      set_tests_properties(${_context_test} PROPERTIES TIMEOUT 15)
    endforeach()

    add_executable(wait_registration_test test/wait_registration_test.cpp)
    target_include_directories(wait_registration_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/shims/xtaskqueue ${XTASKQUEUE_DIR}/Source/Task ${XTASKQUEUE_DIR}/Source/Common)
    target_link_libraries(wait_registration_test PRIVATE gta6rt_xtaskqueue)
    foreach(_case unregister undispatched terminate parked self presignaled monitor close composite snapshot
                  cross concurrent held_snapshot admission cancel_unregister reentrant_admission
                  reentrant_vs_external_unregister multi_dispatcher_terminate multi_dispatcher_unregister_replace)
      add_test(NAME wait_registration_${_case} COMMAND wait_registration_test ${_case})
      set_tests_properties(wait_registration_${_case} PROPERTIES TIMEOUT 15)
    endforeach()
  endif()
  if(WIN32 AND TARGET dstorage_test)
    add_test(NAME dstorage COMMAND dstorage_test)
    set_tests_properties(dstorage PROPERTIES TIMEOUT 120)
  endif()
  if(WIN32 AND TARGET xgameruntime_test)
    add_test(NAME xgameruntime COMMAND xgameruntime_test)
    add_test(NAME xgameruntime_user COMMAND xgameruntime_user_test)
    add_test(NAME xgameruntime_gamesave COMMAND xgameruntime_gamesave_test)
    add_test(NAME xgameruntime_store COMMAND xgameruntime_store_test)
    add_test(NAME xgameruntime_offline COMMAND xgameruntime_offline_test)
    add_test(NAME xgameruntime_offline_hdr COMMAND xgameruntime_offline_test)
    set_tests_properties(xgameruntime_offline_hdr PROPERTIES ENVIRONMENT "LEONIDA_HDR=1")
    add_test(NAME xgameruntime_user_gamertag COMMAND xgameruntime_user_test --expect-gamertag Tester42)
    set_tests_properties(xgameruntime_user_gamertag PROPERTIES ENVIRONMENT "LEONIDA_GAMERTAG=Tester42")
    # Longer than getenv_s's old 64-byte buffer: still the "Player" fallback (and its message), not a silent one.
    add_test(NAME xgameruntime_user_gamertag_long COMMAND xgameruntime_user_test --expect-gamertag Player)
    set_tests_properties(xgameruntime_user_gamertag_long PROPERTIES ENVIRONMENT "LEONIDA_GAMERTAG=GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGG")
  endif()
  # The active-input tracker the GameInput shim feeds (src/active_input.h), on synthetic samples: no SDL, no
  # controller, no GPU, so it builds and runs everywhere, with or without SDL3.
  add_executable(active_input_test test/active_input_test.cpp)
  if(MSVC)
    target_compile_options(active_input_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_test(NAME active_input COMMAND active_input_test)
  # The same keyboard connection lifecycle as the shim, with a fake clock and virtual-device backend only.
  add_executable(keyboard_pad_retry_test test/keyboard_pad_retry_test.cpp)
  if(MSVC)
    target_compile_options(keyboard_pad_retry_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_test(NAME keyboard_pad_retry COMMAND keyboard_pad_retry_test)
  if(WIN32 AND TARGET gameinput_no_sdl_test)
    add_test(NAME gameinput_no_sdl COMMAND gameinput_no_sdl_test)
    # LEONIDA_INPUT_SCRIPT without SDL3: the scripted pad, its device callbacks and its readings on a fake clock.
    add_test(NAME gameinput_no_sdl_script COMMAND gameinput_no_sdl_test --input-script)
  endif()
  if(WIN32 AND TARGET gameinput_test)
    add_test(NAME gameinput COMMAND gameinput_test)
    add_test(NAME gameinput_script COMMAND gameinput_test --input-script)
  elseif(TARGET gta6rt)
    # Built without SDL3: listed as "Not Run (Disabled)" so it is visible that the shim was not tested.
    add_test(NAME gameinput COMMAND ${CMAKE_COMMAND} -E echo "built without SDL3")
    add_test(NAME gameinput_script COMMAND ${CMAKE_COMMAND} -E echo "built without SDL3")
    set_tests_properties(gameinput gameinput_script PROPERTIES DISABLED TRUE)
  endif()
  if(WIN32 AND TARGET gta6rt AND TARGET tiny_game)
    if(TARGET tiny_argv_default_a AND NOT CMAKE_CROSSCOMPILING)
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_FOUND)
        add_test(NAME run_argv_modes COMMAND "${Python3_EXECUTABLE}" -I
          "${CMAKE_CURRENT_SOURCE_DIR}/test/check_argv_modes.py"
          --loader "$<TARGET_FILE:gta6rt>" --fixtures "$<TARGET_FILE_DIR:tiny_argv_default_a>"
          --work-dir "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(run_argv_modes PROPERTIES TIMEOUT 90)
      endif()
    endif()
    add_test(NAME run_startup_cmdline COMMAND ${CMAKE_COMMAND}
      "-DLOADER=$<TARGET_FILE:gta6rt>" "-DGUEST=$<TARGET_FILE:tiny_startup_cmdline>"
      "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_startup_cmdline.cmake")
    add_test(NAME run_tiny_async COMMAND ${CMAKE_COMMAND}
      "-DLOADER=$<TARGET_FILE:gta6rt>" "-DGUEST=$<TARGET_FILE:tiny_async>"
      "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_loaded_async.cmake")
    add_test(NAME run_tiny_gdk_load COMMAND ${CMAKE_COMMAND}
      "-DLOADER=$<TARGET_FILE:gta6rt>" "-DGUEST=$<TARGET_FILE:tiny_gdk_load>"
      "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_loaded_gdk.cmake")
    add_test(NAME run_tiny_bundled_load COMMAND ${CMAKE_COMMAND}
      "-DLOADER=$<TARGET_FILE:gta6rt>" "-DGUEST=$<TARGET_FILE:tiny_bundled_load>"
      "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_loaded_bundled.cmake")
    # atexit handlers, static destructors and the ways out, compared with the same program run natively.
    add_test(NAME run_tiny_atexit COMMAND ${CMAKE_COMMAND}
      "-DLOADER=$<TARGET_FILE:gta6rt>" "-DGUEST=$<TARGET_FILE:tiny_atexit>"
      "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_atexit.cmake")
    add_test(NAME run_tiny_game COMMAND gta6rt --run $<TARGET_FILE:tiny_game> -- hello 42)
    add_test(NAME run_tiny_game_console_paths COMMAND ${CMAKE_COMMAND}
      "-DLOADER=$<TARGET_FILE:gta6rt>" "-DGUEST=$<TARGET_FILE:tiny_game>"
      "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_console_paths.cmake")
    add_test(NAME run_tiny_game_exit_code COMMAND tiny_game_exit_code_test
             "$<TARGET_FILE:gta6rt>" "$<TARGET_FILE:tiny_game>")
    add_test(NAME run_tiny_game_abort_exit_codes COMMAND tiny_game_abort_test
             "$<TARGET_FILE:gta6rt>" "$<TARGET_FILE:tiny_game>")
    # Without relocations it can only run at 0x140000000 (base_reservation.h), so passing means it got there; the
    # game's exact exit code comes back through the relaunch.
    add_test(NAME run_tiny_first_run_ui COMMAND gta6rt --first-run-ui $<TARGET_FILE:tiny_game> -- hello 42 first-run-ui)
    set_tests_properties(run_tiny_first_run_ui PROPERTIES
      ENVIRONMENT "LEONIDA_ARENA_MB=768;LEONIDA_D3D12=;LEONIDA_PIPELINE_COMPILE=;LEONIDA_KEYBOARD_PAD="
      PASS_REGULAR_EXPRESSION "tiny_game: PASS first-run UI caller override"
      FAIL_REGULAR_EXPRESSION "tiny_game: FAIL")
    add_test(NAME run_tiny_game_fixed COMMAND gta6rt --run $<TARGET_FILE:tiny_game_fixed> -- hello 42)
    add_test(NAME run_tiny_game_fixed_exit_code COMMAND tiny_game_exit_code_test
             "$<TARGET_FILE:gta6rt>" "$<TARGET_FILE:tiny_game_fixed>")
    # Two modes at once are refused before anything is mapped (#886). The stand-in game's own arguments are passed,
    # so a gta6rt that ignored the conflict would run it, print "running" and exit 0.
    foreach(modes "dry-run;run" "dry-run;init" "init;run")
      string(REPLACE ";" "_" name "${modes}")
      list(TRANSFORM modes PREPEND "--")
      add_test(NAME gta6rt_rejects_${name} COMMAND gta6rt ${modes} $<TARGET_FILE:tiny_game> -- hello 42)
      set_tests_properties(gta6rt_rejects_${name} PROPERTIES
        PASS_REGULAR_EXPRESSION "usage: gta6rt"
        FAIL_REGULAR_EXPRESSION "running |module\\(s\\) mapped")
    endforeach()
    # The opt-in checks, one test each: thread_local on the game's threads, a C++ exception caught inside the image.
    add_test(NAME run_tiny_game_tls COMMAND gta6rt --run $<TARGET_FILE:tiny_game> -- hello 42 tls)
    add_test(NAME run_tiny_game_eh COMMAND gta6rt --run $<TARGET_FILE:tiny_game> -- hello 42 eh)
    # The /GA build of the same game: thread_local on the entry thread and on a thread it starts, through index-0
    # reads, while index 0 holds gta6rt.exe's own TLS.
    add_test(NAME run_tiny_game_ga_tls COMMAND gta6rt --run $<TARGET_FILE:tiny_game_ga> -- hello 42 tls)
    # BUILD-1457: the stack a game thread gets. gta6rt_stack reads SizeOfStackReserve from the built loader (nothing
    # is run); run_tiny_game_stack asks Windows, on the entry thread and on a thread created with dwStackSize 0.
    add_test(NAME gta6rt_stack COMMAND "${CMAKE_COMMAND}"
             "-DTOOL=$<TARGET_FILE:gta6rt>" "-DWANT=${GTA6RT_STACK}"
             -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_stack_reserve.cmake")
    add_test(NAME run_tiny_game_stack COMMAND gta6rt --run $<TARGET_FILE:tiny_game> -- hello 42 stack)
    # A /GA executable under --run and --init reads its TLS from slot 0, and the trace names gta6rt.exe as the image
    # whose index 0 moved.
    add_test(NAME run_tiny_tls_ga COMMAND ${CMAKE_COMMAND}
      "-DLOADER=$<TARGET_FILE:gta6rt>" "-DGUEST=$<TARGET_FILE:tiny_tls_ga>" "-DTRACE_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_tls_index0.cmake")
    # Clone files are deleted when gta6rt exits normally; only the home clone outlives the run, until the next one.
    add_test(NAME run_tiny_tls_clone_cleanup COMMAND ${CMAKE_COMMAND}
      "-DLOADER=$<TARGET_FILE:gta6rt>" "-DGUEST=$<TARGET_FILE:tiny_tls_clones>" "-DTRACE_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_tls_clone_cleanup.cmake")
    # GAME: is the game folder, absolute, when gta6rt is given a bare image name or `--game-dir .`.
    add_test(NAME run_tiny_game_dir COMMAND ${CMAKE_COMMAND}
      "-DLOADER=$<TARGET_FILE:gta6rt>" "-DGUEST=$<TARGET_FILE:tiny_game>"
      "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_game_dir.cmake")
  endif()
  if(TARGET root_signature_test)
    add_test(NAME root_signature COMMAND root_signature_test)
  endif()
  if(TARGET host_cpu_test)
    add_test(NAME host_cpu COMMAND host_cpu_test)
  endif()
  if(WIN32 AND TARGET root_signature_oracle_test)
    add_test(NAME root_signature_oracle COMMAND root_signature_oracle_test)
  endif()
endif()

if(TARGET gta6rt_d3d12_xs_rootsig)
option(LEONIDA_D3D12_SHIM_TESTS "Build the standalone D3D12 root-signature shim test" OFF)
if(LEONIDA_D3D12_SHIM_TESTS)
  enable_testing()
  add_executable(d3d12_xs_root_signature_test
    test/d3d12_xs_root_signature_test.cpp
    shims/d3d12_xs.cpp
    shims/registry.cpp
    shims/d3d12_xs_device.cpp
    shims/d3d12_xs_pso_stream.cpp
  )
  target_include_directories(d3d12_xs_root_signature_test PRIVATE shims)
  target_compile_definitions(d3d12_xs_root_signature_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(d3d12_xs_root_signature_test PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_options(d3d12_xs_root_signature_test PRIVATE /OPT:NOREF)
  target_link_libraries(d3d12_xs_root_signature_test PRIVATE gta6rt_d3d12_xs_rootsig d3d12 dxgi)
  add_test(NAME d3d12_xs_root_signature COMMAND d3d12_xs_root_signature_test)
  set_tests_properties(d3d12_xs_root_signature PROPERTIES SKIP_RETURN_CODE 77)  # no WARP adapter (Wine)
endif()
endif()

# CPU-only: which SPIR-V capabilities make the standalone GPU tools ask for shaderInt64 (TEST-468). It includes no
# Vulkan header, so it builds and runs without them, unlike the tools below that use it.
add_executable(spirv_capability_test test/spirv_capability_test.cpp)
if(BUILD_TESTING)
  add_test(NAME spirv_capability COMMAND spirv_capability_test)
endif()

# GPU test and pipeline tools load Vulkan at run time, so only the Vulkan headers are needed at build time.
# Nothing is downloaded unless asked: set VULKAN_SDK, install the Vulkan headers, pass
# -DLEONIDA_VULKAN_HEADERS_FETCH=ON to download the pinned Khronos Vulkan-Headers release once at configure time
# (hash-checked), or pass -DFETCHCONTENT_SOURCE_DIR_VULKANHEADERS=<path> to use a local copy without downloading.
option(LEONIDA_VULKAN_HEADERS_FETCH "Download the pinned Khronos Vulkan-Headers release if no Vulkan headers are found" OFF)
option(LEONIDA_REQUIRE_VULKAN "Fail the configure instead of warning when no Vulkan headers are found" OFF)
find_package(Vulkan QUIET)
if(NOT TARGET Vulkan::Headers)
  find_path(Vulkan_INCLUDE_DIR
    NAMES vulkan/vulkan.h
    HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include"
  )
  if(Vulkan_INCLUDE_DIR AND EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan.h")
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    set_target_properties(Vulkan::Headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}"
    )
  elseif(LEONIDA_VULKAN_HEADERS_FETCH OR FETCHCONTENT_SOURCE_DIR_VULKANHEADERS)
    include(FetchContent)
    FetchContent_Declare(VulkanHeaders
      # vulkan-sdk-1.4.363.0 (same commit pinned in .github/workflows/runtime.yml)
      URL https://github.com/KhronosGroup/Vulkan-Headers/archive/6802bb4733b63ed5efd3adb308a6c885ef180ea1.tar.gz
      URL_HASH SHA256=9044fbb356160e43201dc2094acb44c8a906722a63a35f48fc0f051579db9bb6
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(VulkanHeaders)
  endif()
endif()
if(TARGET Vulkan::Headers)
  # The standalone Khronos releases use glslang; older SDKs also provide glslangValidator.
  find_program(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE NAMES glslangValidator glslang
               HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")

  add_executable(vulkan_device_fault_test test/vulkan_device_fault_test.cpp)
  target_link_libraries(vulkan_device_fault_test PRIVATE Vulkan::Headers)
  add_test(NAME vulkan_device_fault COMMAND vulkan_device_fault_test)
  # The AMD breadcrumbs (LEONIDA_DEVICE_MARKERS): seq numbering, marker writes and the likely-hang verdict, on the CPU.
  add_executable(vulkan_device_markers_test test/vulkan_device_markers_test.cpp)
  target_link_libraries(vulkan_device_markers_test PRIVATE Vulkan::Headers)
  add_test(NAME vulkan_device_markers COMMAND vulkan_device_markers_test)
  # The submit lock's owner tracking, waiter count, submit handoff and compile gate (SubmitMutex), on the CPU.
  if(WIN32)
    add_executable(submit_mutex_test test/submit_mutex_test.cpp)
    target_link_libraries(submit_mutex_test PRIVATE Vulkan::Headers)
    target_compile_definitions(submit_mutex_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
      target_compile_options(submit_mutex_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME submit_mutex COMMAND submit_mutex_test)
    set_tests_properties(submit_mutex PROPERTIES TIMEOUT 60)
  endif()

  foreach(tool vk_harness vk_ps_sweep vk_texture_test vk_sampler_test vk_compute_test vk_store_test vk_pipeline_check vk_command_replay_test
          vk_bindless_test vk_image_fuzz vk_descriptor_bench depth24_convert_test vk_predicate_capture_test vk_predicated_copy_test
          vk_xdxr_build_test vk_queue_test vk_sandbox vk_console_heap_gpu_test)
    add_executable(${tool} test/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    if(WIN32)
      target_compile_definitions(${tool} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
  endforeach()
  # The LEONIDA_COMPILE_GATE=auto calibration (src/compile_gate_calibration.h) on this machine's GPU: a verdict within
  # 20 s, adaptive on NVIDIA 621.40. 77 without a Vulkan device.
  add_executable(vk_compile_gate_calibration_test test/vk_compile_gate_calibration_test.cpp)
  target_link_libraries(vk_compile_gate_calibration_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_compile_gate_calibration_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(vk_compile_gate_calibration_test PRIVATE Threads::Threads)
  endif()
  if(MSVC)
    target_compile_options(vk_compile_gate_calibration_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_compile_gate_calibration COMMAND vk_compile_gate_calibration_test)
    set_tests_properties(vk_compile_gate_calibration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
  endif()
  # The ExecuteIndirect argument clamp's compute pass (runtime/src/vulkan_indirect_clamp.h): every argument kind and
  # the counted variants, with garbage records bounded and the batch budget saturating, read back from the scratch
  # the replay would use. 77 without a Vulkan device.
  add_executable(indirect_clamp_test test/indirect_clamp_test.cpp)
  target_link_libraries(indirect_clamp_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(indirect_clamp_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
  if(MSVC)
    target_compile_options(indirect_clamp_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME indirect_clamp COMMAND indirect_clamp_test)
    set_tests_properties(indirect_clamp PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # OpFmaKHR width gate of vk_shader_fma.h (shaderFmaFloat32 / shaderFmaFloat64): no Vulkan instance, no GPU.
  add_executable(vk_shader_fma_test test/vk_shader_fma_test.cpp)
  target_link_libraries(vk_shader_fma_test PRIVATE Vulkan::Headers)
  add_test(NAME vk_shader_fma COMMAND vk_shader_fma_test)
  add_executable(xdxr_builder_validation_test test/xdxr_builder_validation_test.cpp)
  target_link_libraries(xdxr_builder_validation_test PRIVATE Vulkan::Headers)
  if(BUILD_TESTING)
    add_test(NAME xdxr_builder_validation COMMAND xdxr_builder_validation_test)
  endif()
  # LEONIDA_VK_DEVICE names the GPU in every harness (docs/testing.md, "Choosing the GPU"): a filter that matches no
  # device fails the run in vk_descriptor_bench and vk_sandbox too, instead of testing another GPU (TEST-1141).
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    add_test(NAME vk_device_filter_tools COMMAND "${Python3_EXECUTABLE}" -I
             "${CMAKE_CURRENT_SOURCE_DIR}/test/check_device_filter_tools.py" "$<TARGET_FILE:vk_descriptor_bench>"
             "$<TARGET_FILE:vk_sandbox>" "${CMAKE_CURRENT_BINARY_DIR}/vk_device_filter_tools")
    set_tests_properties(vk_device_filter_tools PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  if(MSVC)
    # Keep the checker and its worker threads on the toolchain's C++ runtime under Wine.
    set_property(TARGET vk_pipeline_check PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  # BUILD-1457: a driver compiles on the thread that creates the pipeline, and its compiler can recurse with the
  # shader's nesting. On Windows a thread that asks for no size gets the reserve in the PE header, 1 MB by default:
  # the main thread and every std::thread worker of the checker. AMD's Windows compiler (26.11.2) overflows that
  # on 26 of the corpus's mesh shaders, and 2 MB is still too little for compute shaders nested deeper than 16
  # (docs/pipeline_check.md, "Stack reserve"). Keep the number in step with scripts/build_runtime.ps1.
  set(LEONIDA_PIPELINE_CHECK_STACK 8388608)
  if(WIN32)
    if(MSVC)
      target_link_options(vk_pipeline_check PRIVATE /STACK:${LEONIDA_PIPELINE_CHECK_STACK})
    else()
      target_link_options(vk_pipeline_check PRIVATE -Wl,--stack,${LEONIDA_PIPELINE_CHECK_STACK})
    endif()
  endif()
  if(BUILD_TESTING)
    if(WIN32 AND LEONIDA_BUILD_WINDOWS_RUNTIME)
      # Real Win32 environment and DXGI names, with D3D12 device creation stubbed: no GPU commands or game files.
      # It includes the runtime's D3D12 shim headers, so it needs the same Windows SDK as the runtime.
      add_executable(host_adapter_test test/host_adapter_test.cpp)
      target_link_libraries(host_adapter_test PRIVATE Vulkan::Headers dxgi)
      target_compile_definitions(host_adapter_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      if(MSVC)
        target_compile_options(host_adapter_test PRIVATE /W4 /permissive- /EHsc /utf-8)
      endif()
      add_test(NAME host_adapter COMMAND host_adapter_test)
    endif()
    add_test(NAME vk_pipeline_check_cli COMMAND "${CMAKE_COMMAND}"
             "-DTOOL=$<TARGET_FILE:vk_pipeline_check>"
             "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/vk_pipeline_check_cli_input"
             "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}"
             -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_vk_pipeline_cli.cmake")
    add_test(NAME vk_texture_cube_alpha COMMAND vk_texture_test --self-test-cube-alpha)
    if(WIN32)
      # Reads SizeOfStackReserve from the built file; the program is not run, so it needs no emulator.
      add_test(NAME vk_pipeline_check_stack COMMAND "${CMAKE_COMMAND}"
               "-DTOOL=$<TARGET_FILE:vk_pipeline_check>"
               "-DWANT=${LEONIDA_PIPELINE_CHECK_STACK}"
               -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_stack_reserve.cmake")
    endif()
    add_test(NAME depth24_convert COMMAND depth24_convert_test)
    set_tests_properties(depth24_convert PROPERTIES SKIP_RETURN_CODE 77)
    # Self-checking Vulkan tests with no shaders or game data; each exits 77 without a Vulkan device (TEST-514).
    # indirect_clamp is registered next to its own add_executable above, where tests/test_ctest_registration.py
    # can see it.
    foreach(test vk_command_replay vk_predicated_copy vk_predicate_capture vk_queue vk_console_heap_gpu)
      add_test(NAME ${test} COMMAND ${test}_test)
      set_tests_properties(${test} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
    # The XDXR BVH builder: the GPU writes every case, then tools/xdxr_bvh.py validates and traces them.
    set(LEONIDA_XDXR_OUT "${CMAKE_CURRENT_BINARY_DIR}/xdxr_build_cases")
    file(MAKE_DIRECTORY "${LEONIDA_XDXR_OUT}")
    add_test(NAME xdxr_build COMMAND vk_xdxr_build_test "${LEONIDA_XDXR_OUT}")
    set_tests_properties(xdxr_build PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_SETUP xdxr_build_cases)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
      add_test(NAME xdxr_build_check COMMAND "${Python3_EXECUTABLE}" -I
               "${CMAKE_CURRENT_SOURCE_DIR}/../tools/xdxr_bvh.py" check-build "${LEONIDA_XDXR_OUT}" --rays 1000)
      set_tests_properties(xdxr_build_check PROPERTIES FIXTURES_REQUIRED xdxr_build_cases SKIP_RETURN_CODE 77)
    endif()
  endif()
  # VulkanQueue/VulkanContext query-state synchronization (remove_query_pool vs commit, cross-queue plan/commit)
  # and pending Wait() preservation across transient vkQueueSubmit failures.
  add_executable(vk_queue_query_sync_test test/vk_queue_query_sync_test.cpp)
  target_link_libraries(vk_queue_query_sync_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_queue_query_sync_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(vk_queue_query_sync_test PRIVATE Threads::Threads)
  endif()
  if(MSVC)
    target_compile_options(vk_queue_query_sync_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_queue_query_sync COMMAND vk_queue_query_sync_test)
    set_tests_properties(vk_queue_query_sync PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # Fence and queue teardown with submissions pending: a fence released after an unheld Signal, the bindless upload
  # queue destroyed before another queue submits, the device-fault submit ring forgetting destroyed semaphores.
  add_executable(vk_semaphore_lifetime_test test/vk_semaphore_lifetime_test.cpp)
  target_link_libraries(vk_semaphore_lifetime_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_semaphore_lifetime_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(vk_semaphore_lifetime_test PRIVATE Threads::Threads)
  endif()
  if(MSVC)
    target_compile_options(vk_semaphore_lifetime_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_semaphore_lifetime COMMAND vk_semaphore_lifetime_test)
    set_tests_properties(vk_semaphore_lifetime PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
  endif()
  # The AMD breadcrumbs (LEONIDA_DEVICE_MARKERS) on a real device: after a few dispatches and a wait, the queue's
  # started and finished markers hold the last dispatch's seq. SKIP without VK_AMD_buffer_marker.
  add_executable(vk_device_markers_test test/vk_device_markers_test.cpp)
  target_link_libraries(vk_device_markers_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_device_markers_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
  if(MSVC)
    target_compile_options(vk_device_markers_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_device_markers COMMAND vk_device_markers_test)
    set_tests_properties(vk_device_markers PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
  endif()
  # With LEONIDA_VK_VALIDATION=1: synchronization validation reports a missing barrier (and not a present one), so
  # the other GPU tests' "0 validation errors" was checked by an active layer. 77 without validation or a device.
  add_executable(vk_sync_validation_selftest test/vk_sync_validation_selftest.cpp)
  target_link_libraries(vk_sync_validation_selftest PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_sync_validation_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
  if(MSVC)
    target_compile_options(vk_sync_validation_selftest PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_sync_validation COMMAND vk_sync_validation_selftest)
    set_tests_properties(vk_sync_validation PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    add_executable(vk_overlapping_occlusion_test test/vk_overlapping_occlusion_test.cpp)
    target_link_libraries(vk_overlapping_occlusion_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    if(WIN32)
      target_compile_definitions(vk_overlapping_occlusion_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    foreach(stage vert frag)
      set(source "${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/overlapping_occlusion.${stage}")
      set(output "${CMAKE_CURRENT_BINARY_DIR}/overlapping_occlusion.${stage}.spv")
      add_custom_command(OUTPUT "${output}"
                         COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.0
                                 -o "${output}" "${source}"
                         DEPENDS "${source}" VERBATIM)
      target_sources(vk_overlapping_occlusion_test PRIVATE "${output}")
    endforeach()
    if(BUILD_TESTING)
      add_test(NAME vk_overlapping_occlusion COMMAND vk_overlapping_occlusion_test
               "${CMAKE_CURRENT_BINARY_DIR}/overlapping_occlusion.vert.spv"
               "${CMAKE_CURRENT_BINARY_DIR}/overlapping_occlusion.frag.spv")
      # The same test as a device without pipelineStatisticsQuery runs it (MoltenVK), on whatever device is here.
      add_test(NAME vk_overlapping_occlusion_no_statistics COMMAND vk_overlapping_occlusion_test
               "${CMAKE_CURRENT_BINARY_DIR}/overlapping_occlusion.vert.spv"
               "${CMAKE_CURRENT_BINARY_DIR}/overlapping_occlusion.frag.spv" --no-statistics)
      set_tests_properties(vk_overlapping_occlusion vk_overlapping_occlusion_no_statistics
                           PROPERTIES SKIP_RETURN_CODE 77)
    endif()
  endif()
  # CPU-only: the ranking that makes these harnesses prefer a native ICD over Mesa's Dozen (#915). No GPU needed.
  add_executable(vk_device_pick_test test/vk_device_pick_test.cpp)
  target_link_libraries(vk_device_pick_test PRIVATE Vulkan::Headers)
  if(BUILD_TESTING)
    add_test(NAME vk_device_pick COMMAND vk_device_pick_test)
  endif()
  # Committed BC* texture zero-initialization via vk.zero_buffer or vk.copy_scratch (D3D-220).
  add_executable(vk_bc_zero_init_test test/vk_bc_zero_init_test.cpp)
  target_link_libraries(vk_bc_zero_init_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_bc_zero_init_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
  if(MSVC)
    target_compile_options(vk_bc_zero_init_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_bc_zero_init COMMAND vk_bc_zero_init_test)
    set_tests_properties(vk_bc_zero_init PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # FenceWaitDispatcher concurrency and device-lost regressions (re-entrant callbacks, wakeup on device lost, counter error drain).
  add_executable(vk_fence_dispatcher_sync_test test/vk_fence_dispatcher_sync_test.cpp)
  target_link_libraries(vk_fence_dispatcher_sync_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_fence_dispatcher_sync_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(vk_fence_dispatcher_sync_test PRIVATE Threads::Threads)
  endif()
  if(MSVC)
    target_compile_options(vk_fence_dispatcher_sync_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_fence_dispatcher_sync COMMAND vk_fence_dispatcher_sync_test)
    set_tests_properties(vk_fence_dispatcher_sync PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # A device loss the driver shows only as a timeline counter of UINT64_MAX (driver functions interposed).
  add_executable(vk_fence_lost_by_counter_test test/vk_fence_lost_by_counter_test.cpp)
  target_link_libraries(vk_fence_lost_by_counter_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_fence_lost_by_counter_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(vk_fence_lost_by_counter_test PRIVATE Threads::Threads)
  endif()
  if(MSVC)
    target_compile_options(vk_fence_lost_by_counter_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_fence_lost_by_counter COMMAND vk_fence_lost_by_counter_test)
    set_tests_properties(vk_fence_lost_by_counter PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # The bindless set under view churn (bounded table, spare generations) and failed allocations (host table, in-place
  # rewrite, a degraded flush instead of a refused submission).
  add_executable(vk_bindless_grow_test test/vk_bindless_grow_test.cpp)
  target_link_libraries(vk_bindless_grow_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_bindless_grow_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
  if(MSVC)
    target_compile_options(vk_bindless_grow_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_bindless_grow COMMAND vk_bindless_grow_test)
    set_tests_properties(vk_bindless_grow PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # Null descriptors of every kind through the bindless set's fallbacks: zero T#s and unanswered T#s of each dimension
  # read zeros, and stores and atomics through a null UAV never reach a null SRV (dropped with nullDescriptor). The
  # shader is GLSL; without glslangValidator the test skips.
  add_executable(vk_null_descriptor_test test/vk_null_descriptor_test.cpp)
  target_link_libraries(vk_null_descriptor_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_null_descriptor_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
  if(MSVC)
    target_compile_options(vk_null_descriptor_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    set(_null_descriptors_spv ${CMAKE_CURRENT_BINARY_DIR}/generated/cs_null_descriptors.spv)
    add_custom_command(OUTPUT ${_null_descriptors_spv}
      COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${_null_descriptors_spv}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_null_descriptors.comp
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_null_descriptors.comp
      COMMENT "Compiling the null descriptor test shader" VERBATIM)
    target_sources(vk_null_descriptor_test PRIVATE ${_null_descriptors_spv})
    target_compile_definitions(vk_null_descriptor_test PRIVATE
                               LEONIDA_TEST_NULL_DESCRIPTORS_SPV="${_null_descriptors_spv}")
  else()
    message(STATUS "glslangValidator not found: vk_null_descriptor_test skips")
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_null_descriptor COMMAND vk_null_descriptor_test)
    set_tests_properties(vk_null_descriptor PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # More than 4096 live 1D, 2D-array and 3D images, each read through its bindless index, in a set sized from the
  # device's limits; a full array's refusal reason and indices coming back. The shader is GLSL; without
  # glslangValidator the test skips.
  add_executable(vk_bindless_dimensions_test test/vk_bindless_dimensions_test.cpp)
  target_link_libraries(vk_bindless_dimensions_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_bindless_dimensions_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
  if(MSVC)
    target_compile_options(vk_bindless_dimensions_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    set(_bindless_dimensions_spv ${CMAKE_CURRENT_BINARY_DIR}/generated/cs_bindless_dimensions.spv)
    add_custom_command(OUTPUT ${_bindless_dimensions_spv}
      COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${_bindless_dimensions_spv}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_bindless_dimensions.comp
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_bindless_dimensions.comp
      COMMENT "Compiling the bindless dimensions test shader" VERBATIM)
    target_sources(vk_bindless_dimensions_test PRIVATE ${_bindless_dimensions_spv})
    target_compile_definitions(vk_bindless_dimensions_test PRIVATE
                               LEONIDA_TEST_BINDLESS_DIMENSIONS_SPV="${_bindless_dimensions_spv}")
  else()
    message(STATUS "glslangValidator not found: vk_bindless_dimensions_test skips")
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_bindless_dimensions COMMAND vk_bindless_dimensions_test)
    set_tests_properties(vk_bindless_dimensions PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # CPU-only: the bindless set's array sizes from synthetic device limits.
  add_executable(bindless_sizing_test test/bindless_sizing_test.cpp)
  target_link_libraries(bindless_sizing_test PRIVATE Vulkan::Headers)
  if(BUILD_TESTING)
    add_test(NAME bindless_sizing COMMAND bindless_sizing_test)
  endif()
  # CPU-only: a mesh PSO's output, workgroup and shared-memory needs against synthetic device limits (vulkan_mesh_limits.h).
  add_executable(vulkan_mesh_limits_test test/vulkan_mesh_limits_test.cpp)
  target_link_libraries(vulkan_mesh_limits_test PRIVATE Vulkan::Headers)
  if(BUILD_TESTING)
    add_test(NAME vulkan_mesh_limits COMMAND vulkan_mesh_limits_test)
  endif()
  # CPU-only: Vulkan 1.3 core vs 1.2 KHR/EXT feature probing and proc fallback (runtime/src/vulkan13_features.h).
  add_executable(vulkan13_features_test test/vulkan13_features_test.cpp)
  target_link_libraries(vulkan13_features_test PRIVATE Vulkan::Headers)
  if(MSVC)
    target_compile_options(vulkan13_features_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vulkan13_features COMMAND vulkan13_features_test)
  endif()
  # Buffer registrations keep their binding, VA lookup, and sparse owner in step on host allocation failure.
  add_executable(vk_buffer_registration_oom_test test/vk_buffer_registration_oom_test.cpp)
  target_link_libraries(vk_buffer_registration_oom_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_buffer_registration_oom_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
      target_compile_options(vk_buffer_registration_oom_test PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_buffer_registration_oom COMMAND vk_buffer_registration_oom_test)
  endif()
  # VulkanContext::replay_.host_at resolves aliasing placed upload buffers across the full requested byte range.
  add_executable(vk_host_at_aliasing_test test/vk_host_at_aliasing_test.cpp)
  target_link_libraries(vk_host_at_aliasing_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(vk_host_at_aliasing_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
      target_compile_options(vk_host_at_aliasing_test PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
  endif()
  if(BUILD_TESTING)
    add_test(NAME vk_host_at_aliasing COMMAND vk_host_at_aliasing_test)
  endif()
  # CPU-only: ScratchBuffers chunk alignment, retire/collect/forget, and spare cap (runtime/src/vulkan_scratch.h).
  add_executable(vulkan_scratch_test test/vulkan_scratch_test.cpp)
  target_link_libraries(vulkan_scratch_test PRIVATE Vulkan::Headers)
  if(MSVC)
    target_compile_options(vulkan_scratch_test PRIVATE /W4 /permissive- /utf-8)
  endif()
  if(BUILD_TESTING)
    add_test(NAME vulkan_scratch COMMAND vulkan_scratch_test)
  endif()
  if(WIN32)
    # CPU-only: a completed GPU label is stored into a page the D3D12 layer watches (read-only until written) and
    # dropped for a page that is read-only by the title's own doing (runtime/src/vulkan_gpu_labels.h).
    add_executable(gpu_label_store_test test/gpu_label_store_test.cpp)
    target_link_libraries(gpu_label_store_test PRIVATE Vulkan::Headers)
    target_compile_definitions(gpu_label_store_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if(MSVC)
      target_compile_options(gpu_label_store_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    if(BUILD_TESTING)
      add_test(NAME gpu_label_store COMMAND gpu_label_store_test)
    endif()
  endif()
  if(WIN32 AND BUILD_TESTING)
    # Whole title-region import capability: one host allocation bound as a BDA buffer. The default case uses the
    # measured 185 MiB placeholder size. A machine that cannot import (no importable usage/handle, an import-budget
    # refusal, no host memory to commit it) exits 77 and is a reported capability result.
    add_executable(vk_host_region_import_test test/vk_host_region_import_test.cpp)
    target_link_libraries(vk_host_region_import_test PRIVATE Vulkan::Headers)
    target_compile_definitions(vk_host_region_import_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
      target_compile_options(vk_host_region_import_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME vk_host_region_import COMMAND vk_host_region_import_test 185)
    set_tests_properties(vk_host_region_import PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # The same import under VK_LAYER_KHRONOS_validation with synchronization validation. The binary exits 77 when
    # the layer is not installed, so a machine without the SDK skips this variant instead of failing it.
    add_test(NAME vk_host_region_import_validation COMMAND vk_host_region_import_test 185)
    set_tests_properties(vk_host_region_import_validation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                         ENVIRONMENT "LEONIDA_VK_VALIDATION=1;VK_VALIDATE_SYNC=1")
    # VulkanPresenter tracker lock / failed-submit rollback and capture-buffer WAW synchronization.
    add_executable(vk_presenter_sync_test test/vk_presenter_sync_test.cpp)
    target_link_libraries(vk_presenter_sync_test PRIVATE Vulkan::Headers user32 advapi32)
    target_compile_definitions(vk_presenter_sync_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
      target_compile_options(vk_presenter_sync_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME vk_presenter_sync COMMAND vk_presenter_sync_test)
    add_test(NAME vk_presenter_sparse_capture COMMAND vk_presenter_sync_test --sparse-capture)
    set_tests_properties(vk_presenter_sync vk_presenter_sparse_capture PROPERTIES SKIP_RETURN_CODE 77)
    set(conformance_src ${CMAKE_CURRENT_SOURCE_DIR}/../tools/conformance/native/src)
    add_executable(vk_present_dump_test test/vk_present_dump_test.cpp
                   ${conformance_src}/png_io.cpp ${conformance_src}/diagnostics.cpp)
    target_link_libraries(vk_present_dump_test PRIVATE Vulkan::Headers user32 windowscodecs ole32)
    target_compile_definitions(vk_present_dump_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_test(NAME vk_present_dump COMMAND vk_present_dump_test)
    add_test(NAME vk_present_dump_default COMMAND vk_present_dump_test --default)
    add_test(NAME vk_present_dump_invalid COMMAND vk_present_dump_test --invalid)
    set_tests_properties(vk_present_dump vk_present_dump_default vk_present_dump_invalid PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    add_executable(vk_binary_occlusion_test test/vk_binary_occlusion_test.cpp)
    target_link_libraries(vk_binary_occlusion_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    if(WIN32)
      target_compile_definitions(vk_binary_occlusion_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    foreach(stage vert frag)
      set(source "${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/binary_occlusion.${stage}")
      set(output "${CMAKE_CURRENT_BINARY_DIR}/binary_occlusion.${stage}.spv")
      add_custom_command(OUTPUT "${output}"
                         COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.0
                                 -o "${output}" "${source}"
                         DEPENDS "${source}" VERBATIM)
      target_sources(vk_binary_occlusion_test PRIVATE "${output}")
    endforeach()
    if(BUILD_TESTING)
      add_test(NAME vk_binary_occlusion COMMAND vk_binary_occlusion_test
               "${CMAKE_CURRENT_BINARY_DIR}/binary_occlusion.vert.spv"
               "${CMAKE_CURRENT_BINARY_DIR}/binary_occlusion.frag.spv")
      set_tests_properties(vk_binary_occlusion PROPERTIES SKIP_RETURN_CODE 77)
    endif()
  endif()
  # D3D12 COM objects over the Vulkan backend. Their method stubs are generated from the SDK's d3d12.h at build
  # time (tools/gen_d3d12_stubs.py), so nothing derived from the SDK header is checked in.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  # The generator finds d3d12.h through INCLUDE, which a Developer Prompt sets. A cross build (for example
  # runtime/cmake/clang-cl-xwin.cmake) has no such prompt, so it is handed the header CMake finds.
  set(_stubs_header)
  set(_stubs_header_ok TRUE)
  if(TARGET gta6rt AND CMAKE_CROSSCOMPILING)
    find_file(LEONIDA_D3D12_HEADER d3d12.h)
    if(LEONIDA_D3D12_HEADER)
      set(_stubs_header --header "${LEONIDA_D3D12_HEADER}")
    else()
      set(_stubs_header_ok FALSE)
      message(STATUS "d3d12.h not found (set CMAKE_INCLUDE_PATH): the D3D12 COM objects are not built")
    endif()
  endif()
  if(TARGET gta6rt AND Python3_FOUND AND _stubs_header_ok)
    set(LEONIDA_STUBS ${CMAKE_CURRENT_BINARY_DIR}/generated/d3d12_stubs.h)
    add_custom_command(
      OUTPUT ${LEONIDA_STUBS}
      COMMAND "${Python3_EXECUTABLE}" -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_d3d12_stubs.py --out ${LEONIDA_STUBS}
              ${_stubs_header}
              ID3D12GraphicsCommandList8 ID3D12CommandQueue ID3D12CommandAllocator ID3D12Fence1
              ID3D12Device11 ID3D12DescriptorHeap ID3D12Heap ID3D12Resource ID3D12QueryHeap ID3D12RootSignature
              ID3D12PipelineState ID3D12CommandSignature ID3D12PipelineLibrary1 ID3D12ShaderCacheSession
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_d3d12_stubs.py ${LEONIDA_D3D12_HEADER}
      COMMENT "Generating D3D12 COM stubs"
      VERBATIM)
    # Optional NVIDIA Nsight Aftermath GPU crash dumps (LEONIDA_AFTERMATH=1, src/vulkan_aftermath.h). Nothing of the SDK
    # is in the repository or linked: -DLEONIDA_AFTERMATH_SDK=<dir> points at an unpacked Nsight Aftermath SDK (include/,
    # lib/x64/GFSDK_Aftermath_Lib.x64.dll). Its headers are used at build time; the DLL is loaded at run time, from
    # LEONIDA_AFTERMATH_DLL or else that SDK's lib/x64. Without the option the functions do nothing.
    set(LEONIDA_AFTERMATH_SDK "" CACHE PATH "Nsight Aftermath SDK directory (include/, lib/x64/) for LEONIDA_AFTERMATH=1")
    add_library(gta6rt_aftermath STATIC src/vulkan_aftermath.cpp)
    target_include_directories(gta6rt_aftermath PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(gta6rt_aftermath PUBLIC Vulkan::Headers)
    target_compile_definitions(gta6rt_aftermath PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(gta6rt_aftermath PRIVATE /W4 /permissive- /utf-8)
    if(LEONIDA_AFTERMATH_SDK)
      file(TO_CMAKE_PATH "${LEONIDA_AFTERMATH_SDK}" _aftermath_sdk)
      if(NOT EXISTS "${_aftermath_sdk}/include/GFSDK_Aftermath_GpuCrashDumpDecoding.h")
        message(FATAL_ERROR "LEONIDA_AFTERMATH_SDK=${_aftermath_sdk}: no include/GFSDK_Aftermath_GpuCrashDumpDecoding.h")
      endif()
      execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" describe --always --dirty
                      OUTPUT_VARIABLE _aftermath_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      if(NOT _aftermath_version)
        set(_aftermath_version "unknown")
      endif()
      target_include_directories(gta6rt_aftermath PRIVATE "${_aftermath_sdk}/include")
      target_compile_definitions(gta6rt_aftermath PRIVATE LEONIDA_HAVE_AFTERMATH
        LEONIDA_AFTERMATH_DLL_DEFAULT="${_aftermath_sdk}/lib/x64/GFSDK_Aftermath_Lib.x64.dll"
        LEONIDA_AFTERMATH_APP_VERSION="${_aftermath_version}")
      message(STATUS "Nsight Aftermath SDK: ${_aftermath_sdk} (crash dumps with LEONIDA_AFTERMATH=1)")
    endif()

    # xmem.cpp and registry.cpp: d3d12_vulkan_device.cpp's compute-input probe asks the xmem shim for the other views
    # of a title address (rt::xmem::geometry_aliases), and xmem.cpp registers its table through the shim registry, so
    # the test targets that link this library need both. gta6rt compiles the two files itself and never pulls these
    # members. psapi is what xmem.cpp's working-set call needs.
    add_library(gta6rt_d3d12_com STATIC shims/d3d12_com.cpp shims/d3d12_vulkan_device.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp shims/xbox_present.cpp shims/xmem.cpp shims/registry.cpp
                shims/dxgi_com.cpp shims/package_splash.cpp ${LEONIDA_STUBS})   # d3d12_vulkan_device.cpp reads the PC device behind the proxy (#224)
    target_include_directories(gta6rt_d3d12_com PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(gta6rt_d3d12_com PUBLIC Vulkan::Headers gta6rt_root_signature gta6rt_aftermath PRIVATE ole32 windowscodecs xmllite shlwapi user32 gdi32 psapi)
    target_compile_definitions(gta6rt_d3d12_com PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # /bigobj: a Debug build of d3d12_vulkan_device.cpp has more sections than a COFF object holds by default
    # (fatal error C1128), because without optimisation every inline function keeps its own COMDAT.
    target_compile_options(gta6rt_d3d12_com PRIVATE /W4 /permissive- /utf-8 /bigobj)

    # A compute shader that writes through a bogus buffer device address on an Aftermath-enabled device: the dump appears
    # and the decoder names the shader. It loses the device on purpose, so it runs only with LEONIDA_AFTERMATH_FAULT_TEST=1
    # (else exit 77, as without the DLL or an NVIDIA GPU).
    if(LEONIDA_AFTERMATH_SDK AND Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      set(_aftermath_spv ${CMAKE_CURRENT_BINARY_DIR}/generated/aftermath_fault.comp.spv)
      add_custom_command(OUTPUT ${_aftermath_spv}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${_aftermath_spv}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/aftermath_fault.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/aftermath_fault.comp COMMENT "Compiling aftermath_fault.comp" VERBATIM)
      add_executable(vk_aftermath_test test/vk_aftermath_test.cpp ${_aftermath_spv})
      target_link_libraries(vk_aftermath_test PRIVATE gta6rt_aftermath)
      target_compile_definitions(vk_aftermath_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
                                 LEONIDA_AFTERMATH_FAULT_SPV="${_aftermath_spv}")
      target_compile_options(vk_aftermath_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME vk_aftermath COMMAND vk_aftermath_test)
      set_tests_properties(vk_aftermath PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    add_executable(d3d12_com_test test/d3d12_com_test.cpp)
    target_link_libraries(d3d12_com_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_com_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_com_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_com COMMAND d3d12_com_test)
    foreach(_policy IN ITEMS off unset raw uppercase off-with-suffix)
      add_test(NAME d3d12_indirect_policy_${_policy} COMMAND d3d12_com_test --indirect-policy ${_policy})
    endforeach()
    set_tests_properties(d3d12_com PROPERTIES SKIP_RETURN_CODE 77)

    # Where the device finds its translated shaders (shader_path.txt, a path that is not UTF-8). No GPU.
    add_executable(d3d12_shader_path_test test/d3d12_shader_path_test.cpp)
    target_link_libraries(d3d12_shader_path_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_shader_path_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_shader_path_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_shader_path COMMAND d3d12_shader_path_test)

    # Multi-threaded concurrent submissions across two D3D12 queues + resource creation/destruction churn.
    add_executable(d3d12_multithread_test test/d3d12_multithread_test.cpp)
    target_link_libraries(d3d12_multithread_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_multithread_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_multithread_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_multithread COMMAND d3d12_multithread_test)
    # 300 s, not 120: the test's 60 s fence waits need headroom when other GPU tests share the device
    # (two concurrent `ctest -j 6` suites pushed it to 91-120 s; TEST-514 follow-up).
    set_tests_properties(d3d12_multithread PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    # Several threads make the first CheckFeatureSupport(D3D12_OPTIONS1) call of a new device at the same time.
    add_executable(d3d12_options1_first_use_test test/d3d12_options1_first_use_test.cpp)
    target_link_libraries(d3d12_options1_first_use_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_options1_first_use_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_options1_first_use_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_options1_first_use COMMAND d3d12_options1_first_use_test)
    set_tests_properties(d3d12_options1_first_use PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # A queued Wait must not block a Signal submitted by another D3D12 queue.
    add_executable(d3d12_queue_wait_test test/d3d12_queue_wait_test.cpp)
    target_link_libraries(d3d12_queue_wait_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_queue_wait_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_queue_wait_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_queue_wait COMMAND d3d12_queue_wait_test)
    add_executable(d3d12_noop_copy_test test/d3d12_noop_copy_test.cpp)
    target_link_libraries(d3d12_noop_copy_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_noop_copy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_noop_copy_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_noop_copy COMMAND d3d12_noop_copy_test)
    set_tests_properties(d3d12_noop_copy PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
    set_tests_properties(d3d12_queue_wait PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_test(NAME d3d12_queue_wait_timeout COMMAND d3d12_queue_wait_test --timeout-control)
    set_tests_properties(d3d12_queue_wait_timeout PROPERTIES SKIP_RETURN_CODE 77 WILL_FAIL TRUE TIMEOUT 10)
    # Textures share blocks of device memory, and a reused range neither leaks old texels nor touches its neighbours.
    add_executable(d3d12_texture_memory_test test/d3d12_texture_memory_test.cpp)
    target_link_libraries(d3d12_texture_memory_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_texture_memory_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_texture_memory_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_texture_memory COMMAND d3d12_texture_memory_test)
    set_tests_properties(d3d12_texture_memory PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    # The pipelines a submission needs, compiled on the device's workers: the same image as without them. The second
    # time with the pipelines compiled when they are created, which leaves the variants.
    # The compile workers of PipelineCompiler (LEONIDA_COMPILE_THREADS, LEONIDA_COMPILE_HEAVY_MAX): their rules with jobs
    # that wait at gates, no device.
    add_executable(d3d12_compile_pool_test test/d3d12_compile_pool_test.cpp)
    target_link_libraries(d3d12_compile_pool_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_compile_pool_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_compile_pool_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_compile_pool COMMAND d3d12_compile_pool_test)
    set_tests_properties(d3d12_compile_pool PROPERTIES TIMEOUT 120)
    # RT-PIPELINE-PREWARM-ORDER phase 2: the plan read from an order log (src/pipeline_prewarm.h) and its warm-ups on the
    # compile workers, with jobs that wait at gates; no device.
    add_executable(pipeline_prewarm_test test/pipeline_prewarm_test.cpp)
    target_link_libraries(pipeline_prewarm_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(pipeline_prewarm_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(pipeline_prewarm_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME pipeline_prewarm COMMAND pipeline_prewarm_test)
    set_tests_properties(pipeline_prewarm PROPERTIES TIMEOUT 120)
    add_executable(d3d12_pipeline_workers_test test/d3d12_pipeline_workers_test.cpp)
    target_link_libraries(d3d12_pipeline_workers_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_pipeline_workers_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_pipeline_workers_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_pipeline_workers COMMAND d3d12_pipeline_workers_test)
    add_test(NAME d3d12_pipeline_workers_eager COMMAND d3d12_pipeline_workers_test)
    set_tests_properties(d3d12_pipeline_workers d3d12_pipeline_workers_eager PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    # One large pipeline after another: the jobs of the pool are then known (d3d12_big_compute has the budget).
    set_tests_properties(d3d12_pipeline_workers PROPERTIES ENVIRONMENT "LEONIDA_SERIAL_BIG_COMPILES=1")
    set_tests_properties(d3d12_pipeline_workers_eager PROPERTIES
                         ENVIRONMENT "LEONIDA_LAZY_PIPELINES=0;LEONIDA_LAZY_COMPUTE=0;LEONIDA_SERIAL_BIG_COMPILES=1")
    # LEONIDA_ASYNC_PIPELINES=1 with LEONIDA_ASYNC_MIN_BYTES over every module and the compile worker held: with
    # LEONIDA_ASYNC_MIN_BYTES_GRAPHICS=0 the draws are skipped while every dispatch builds its pipeline in place; unset,
    # the draws build theirs in place too (src/async_pipeline_size.h).
    add_test(NAME d3d12_pipeline_workers_async_graphics_zero COMMAND d3d12_pipeline_workers_test --async-graphics-zero)
    add_test(NAME d3d12_pipeline_workers_async_graphics_unset COMMAND d3d12_pipeline_workers_test --async-graphics-unset)
    set_tests_properties(d3d12_pipeline_workers_async_graphics_zero d3d12_pipeline_workers_async_graphics_unset
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    # Large pipelines compiled within a memory budget, with real large compute modules: the same values for each setting
    # (read once for each process). 260 MiB holds three of the modules, 1 MiB none. The modules' compile runs on the test's
    # threads, so they get the loader's stack (docs/pipeline_check.md, "Stack reserve").
    add_executable(d3d12_big_compute_test test/d3d12_big_compute_test.cpp)
    target_link_libraries(d3d12_big_compute_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_big_compute_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_big_compute_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(d3d12_big_compute_test PRIVATE /STACK:${GTA6RT_STACK})
    add_test(NAME d3d12_big_compute COMMAND d3d12_big_compute_test)
    add_test(NAME d3d12_big_compute_budget COMMAND d3d12_big_compute_test)
    add_test(NAME d3d12_big_compute_tight COMMAND d3d12_big_compute_test)
    add_test(NAME d3d12_big_compute_serial COMMAND d3d12_big_compute_test)
    add_test(NAME d3d12_big_compute_unlimited COMMAND d3d12_big_compute_test)
    add_test(NAME d3d12_big_compute_warmup COMMAND d3d12_big_compute_test warmup)
    add_test(NAME d3d12_big_compute_eager COMMAND d3d12_big_compute_test eager)
    set_tests_properties(d3d12_big_compute d3d12_big_compute_budget d3d12_big_compute_tight d3d12_big_compute_serial
                         d3d12_big_compute_unlimited d3d12_big_compute_warmup d3d12_big_compute_eager
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    set_tests_properties(d3d12_big_compute_budget PROPERTIES ENVIRONMENT "LEONIDA_BIG_COMPILE_BUDGET_MB=260")
    set_tests_properties(d3d12_big_compute_tight d3d12_big_compute_warmup PROPERTIES
                         ENVIRONMENT "LEONIDA_BIG_COMPILE_BUDGET_MB=1")
    set_tests_properties(d3d12_big_compute_eager PROPERTIES
                         ENVIRONMENT "LEONIDA_BIG_COMPILE_BUDGET_MB=1;LEONIDA_LAZY_COMPUTE=0")
    set_tests_properties(d3d12_big_compute_serial PROPERTIES  # neither of these two has a budget
                         ENVIRONMENT "LEONIDA_SERIAL_BIG_COMPILES=1;LEONIDA_BIG_COMPILE_BUDGET_MB=4096")
    set_tests_properties(d3d12_big_compute_unlimited PROPERTIES
                         ENVIRONMENT "LEONIDA_SERIAL_BIG_COMPILES=0;LEONIDA_BIG_COMPILE_BUDGET_MB=1")
    # ExecuteCommandLists refuses a list whose type isn't the queue's (debug-layer message 836).
    add_executable(d3d12_queue_list_type_test test/d3d12_queue_list_type_test.cpp)
    target_link_libraries(d3d12_queue_list_type_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_queue_list_type_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_queue_list_type_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_queue_list_type COMMAND d3d12_queue_list_type_test)
    set_tests_properties(d3d12_queue_list_type PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    # Cross-device ownership validation: Signal, Wait, SetEventOnMultipleFenceCompletion, ExecuteCommandLists, ExecuteBundle.
    add_executable(d3d12_cross_device_validation_test test/d3d12_cross_device_validation_test.cpp)
    target_link_libraries(d3d12_cross_device_validation_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_cross_device_validation_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_cross_device_validation_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_cross_device_validation COMMAND d3d12_cross_device_validation_test)
    set_tests_properties(d3d12_cross_device_validation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    # Wait-before-Signal with a texture and a buffer handed over between the COPY, COMPUTE and DIRECT queues, in every
    # submission order, on the layer and on the host runtime (docs/d3d12_vulkan.md section 4.4).
    add_executable(d3d12_queue_handover_test test/d3d12_queue_handover_test.cpp)
    target_link_libraries(d3d12_queue_handover_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_queue_handover_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_queue_handover_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_queue_handover COMMAND d3d12_queue_handover_test)
    set_tests_properties(d3d12_queue_handover PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 400
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    # The same on a caller-owned device: two VkQueues handed to the layer (the D3D12 queues share them), and one
    # (every D3D12 queue shares it, where a Wait ahead of its Signal would never progress on the GPU).
    add_test(NAME d3d12_queue_handover_two_queues COMMAND d3d12_queue_handover_test --two-queues)
    add_test(NAME d3d12_queue_handover_shared_queue COMMAND d3d12_queue_handover_test --shared-queue)
    set_tests_properties(d3d12_queue_handover_two_queues d3d12_queue_handover_shared_queue PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 400 FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    # More D3D12 queues than Vulkan queues: they share VkQueues, keeping fences and Wait-before-Signal (section 4.4).
    # The _one_queue runs make every GPU look like the Intel UHD 770 (one family, one VkQueue): every queue shares it.
    add_executable(d3d12_queue_share_test test/d3d12_queue_share_test.cpp)
    target_link_libraries(d3d12_queue_share_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_queue_share_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_queue_share_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_queue_share COMMAND d3d12_queue_share_test)
    add_test(NAME d3d12_queue_share_one_queue COMMAND d3d12_queue_share_test)
    set_tests_properties(d3d12_queue_share d3d12_queue_share_one_queue PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    add_test(NAME d3d12_queue_wait_one_queue COMMAND d3d12_queue_wait_test)
    set_tests_properties(d3d12_queue_wait_one_queue PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    set_tests_properties(d3d12_queue_share_one_queue d3d12_queue_wait_one_queue PROPERTIES
                         ENVIRONMENT "LEONIDA_VK_MAX_QUEUES_PER_FAMILY=1;LEONIDA_VK_GRAPHICS_FAMILY_ONLY=1")
    # A misspelt case or count is refused with the usage line (exit code 2) before a device exists, instead of
    # passing with nothing run. No GPU needed.
    set(_share_bad_arguments "case_typo|--case|typo" "iterations_zero|--iterations|0"
        "iterations_word|--iterations|banana" "iterations_too_many|--iterations|99999999999")
    foreach(_bad IN LISTS _share_bad_arguments)
      string(REPLACE "|" ";" _bad "${_bad}")
      list(POP_FRONT _bad _bad_name)
      add_test(NAME d3d12_queue_share_rejects_${_bad_name} COMMAND d3d12_queue_share_test ${_bad})
      set_tests_properties(d3d12_queue_share_rejects_${_bad_name} PROPERTIES TIMEOUT 60
                           PASS_REGULAR_EXPRESSION "\nusage: d3d12_queue_share_test ")
    endforeach()

    # XDXR acceleration structures through BuildRaytracingAccelerationStructure (docs/d3d12_vulkan.md 4.8): the
    # device builds them, tools/xdxr_bvh.py checks them.
    add_executable(d3d12_xdxr_rt_test test/d3d12_xdxr_rt_test.cpp test/xmem_geometry_aliases_stub.cpp)
    target_link_libraries(d3d12_xdxr_rt_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_xdxr_rt_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_xdxr_rt_test PRIVATE /W4 /permissive- /utf-8)
    set(LEONIDA_D3D12_XDXR_OUT "${CMAKE_CURRENT_BINARY_DIR}/d3d12_xdxr_cases")
    file(MAKE_DIRECTORY "${LEONIDA_D3D12_XDXR_OUT}")
    add_test(NAME d3d12_xdxr_rt COMMAND d3d12_xdxr_rt_test "${LEONIDA_D3D12_XDXR_OUT}")
    set_tests_properties(d3d12_xdxr_rt PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 FIXTURES_SETUP d3d12_xdxr_cases)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
      add_test(NAME d3d12_xdxr_rt_check COMMAND "${Python3_EXECUTABLE}" -I
               "${CMAKE_CURRENT_SOURCE_DIR}/../tools/xdxr_bvh.py" check-build "${LEONIDA_D3D12_XDXR_OUT}" --rays 1000)
      set_tests_properties(d3d12_xdxr_rt_check PROPERTIES FIXTURES_REQUIRED d3d12_xdxr_cases SKIP_RETURN_CODE 77)
    endif()

    # XboxBvhBuilder.dll served by the layer's builder (RT-5, boot stop 18): the seven exports the executable imports,
    # through the shim registry as the title binds them; the structures are checked like d3d12_xdxr_rt's.
    set(LEONIDA_XBOX_BVH_BUILDER_OUT "${CMAKE_CURRENT_BINARY_DIR}/xbox_bvh_builder_cases")
    add_executable(xbox_bvh_builder_test test/xbox_bvh_builder_test.cpp shims/xbox_bvh_builder.cpp shims/registry.cpp)
    target_link_libraries(xbox_bvh_builder_test PRIVATE gta6rt_d3d12_com)
    # LEONIDA_VULKAN_DEVICE: the shim hands its rehydration scratch to the layer's list, as in gta6rt.
    target_compile_definitions(xbox_bvh_builder_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX LEONIDA_VULKAN_DEVICE)
    target_compile_options(xbox_bvh_builder_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(xbox_bvh_builder_test PRIVATE /OPT:NOREF)
    add_test(NAME xbox_bvh_builder COMMAND xbox_bvh_builder_test "${LEONIDA_XBOX_BVH_BUILDER_OUT}")
    set_tests_properties(xbox_bvh_builder PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 FIXTURES_SETUP xbox_bvh_builder_cases)
    add_test(NAME xbox_bvh_builder_console COMMAND xbox_bvh_builder_test --console)
    set_tests_properties(xbox_bvh_builder_console PROPERTIES ENVIRONMENT "LEONIDA_BVH_BUILDER=console")
    if(Python3_FOUND)
      add_test(NAME xbox_bvh_builder_check COMMAND "${Python3_EXECUTABLE}" -I
               "${CMAKE_CURRENT_SOURCE_DIR}/../tools/xdxr_bvh.py" check-build "${LEONIDA_XBOX_BVH_BUILDER_OUT}" --rays 1000)
      set_tests_properties(xbox_bvh_builder_check PROPERTIES FIXTURES_REQUIRED xbox_bvh_builder_cases SKIP_RETURN_CODE 77)
    endif()

    # GCL8's QueryInterface and its one method need an allocator and a list, not a Vulkan device.
    add_executable(d3d12_gcl8_test test/d3d12_gcl8_test.cpp)
    target_link_libraries(d3d12_gcl8_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_gcl8_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_gcl8_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_gcl8 COMMAND d3d12_gcl8_test)
    # Python runs on the host; a cross-compiled fixture runs through the emulator (Wine), which the script prepends.
    # The interpreter is named by path, because ctest would put the emulator in front of an executable target.
    set(_trace_runner)
    foreach(_part IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
      list(APPEND _trace_runner "--runner=${_part}")  # one word each, even one that starts with "-" (xvfb-run -a)
    endforeach()
    add_test(NAME d3d12_trace COMMAND "${Python3_EXECUTABLE}" -I
             "${CMAKE_CURRENT_SOURCE_DIR}/test/check_d3d12_trace.py" "$<TARGET_FILE:d3d12_gcl8_test>" ${_trace_runner})

    # The Vulkan device behind the Xbox-ABI proxy, end to end.
    add_executable(d3d12_vulkan_device_test test/d3d12_vulkan_device_test.cpp)
    target_link_libraries(d3d12_vulkan_device_test PRIVATE gta6rt_d3d12_com dxguid)  # IID_ID3D12MetaCommand (#1440)
    target_compile_definitions(d3d12_vulkan_device_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_vulkan_device_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_vulkan_device COMMAND d3d12_vulkan_device_test)

    add_executable(d3d12_buffer_tiles_test test/d3d12_buffer_tiles_test.cpp)
    target_link_libraries(d3d12_buffer_tiles_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_buffer_tiles_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_buffer_tiles_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_buffer_tiles COMMAND d3d12_buffer_tiles_test)
    set_tests_properties(d3d12_buffer_tiles PROPERTIES SKIP_RETURN_CODE 77
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    add_executable(d3d12_texture_tiles_test test/d3d12_texture_tiles_test.cpp)
    target_link_libraries(d3d12_texture_tiles_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_texture_tiles_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_texture_tiles_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_texture_tiles COMMAND d3d12_texture_tiles_test)
    set_tests_properties(d3d12_texture_tiles PROPERTIES SKIP_RETURN_CODE 77
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")

    # DXR acceleration-structure prebuild and command-list recording validation parity test.
    add_executable(d3d12_dxr_validation_test test/d3d12_dxr_validation_test.cpp test/xmem_geometry_aliases_stub.cpp)
    target_link_libraries(d3d12_dxr_validation_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_dxr_validation_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_dxr_validation_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_dxr_validation COMMAND d3d12_dxr_validation_test)
    add_test(NAME d3d12_dxr_validation_khr COMMAND d3d12_dxr_validation_test)
    set_tests_properties(d3d12_dxr_validation d3d12_dxr_validation_khr PROPERTIES SKIP_RETURN_CODE 77
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    set_tests_properties(d3d12_dxr_validation_khr PROPERTIES ENVIRONMENT "LEONIDA_RT_AS=khr")

    # D3D12 permits UpdateTileMappings on DIRECT/COPY/COMPUTE and a sparse buffer must remain
    # accessible across their selected Vulkan families. The optional API-dump check catches an
    # EXCLUSIVE VkBuffer even on drivers that happen to return correct bytes without ownership barriers.
    add_executable(d3d12_sparse_queue_family_test test/d3d12_sparse_queue_family_test.cpp)
    target_link_libraries(d3d12_sparse_queue_family_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_sparse_queue_family_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_sparse_queue_family_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_sparse_queue_family COMMAND d3d12_sparse_queue_family_test)
    set_tests_properties(d3d12_sparse_queue_family PROPERTIES SKIP_RETURN_CODE 77
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    if(Python3_FOUND AND NOT CMAKE_CROSSCOMPILING)
      add_test(NAME d3d12_sparse_queue_family_contract COMMAND "${Python3_EXECUTABLE}" -I
               "${CMAKE_CURRENT_SOURCE_DIR}/test/check_sparse_queue_family.py"
               "$<TARGET_FILE:d3d12_sparse_queue_family_test>")
      set_tests_properties(d3d12_sparse_queue_family_contract PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 65)
    endif()
    set_tests_properties(d3d12_vulkan_device PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(d3d12_texture1d_test test/d3d12_texture1d_test.cpp)
    target_link_libraries(d3d12_texture1d_test PRIVATE gta6rt_d3d12_com dxguid)
    target_compile_definitions(d3d12_texture1d_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_texture1d_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_texture1d COMMAND d3d12_texture1d_test)
    set_tests_properties(d3d12_texture1d PROPERTIES SKIP_RETURN_CODE 77)

    # Placed 1D and 3D textures against the host runtime and WARP, and a copy round trip through them.
    add_executable(d3d12_placed_dimensions_test test/d3d12_placed_dimensions_test.cpp)
    target_link_libraries(d3d12_placed_dimensions_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_placed_dimensions_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_placed_dimensions_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_placed_dimensions COMMAND d3d12_placed_dimensions_test)
    set_tests_properties(d3d12_placed_dimensions PROPERTIES SKIP_RETURN_CODE 77
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")

    # FORMAT_SUPPORT of the depth- and stencil-plane view formats and comparison sampling, against the host runtime.
    add_executable(d3d12_depth_view_format_support_test test/d3d12_depth_view_format_support_test.cpp)
    target_link_libraries(d3d12_depth_view_format_support_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_depth_view_format_support_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_depth_view_format_support_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_depth_view_format_support COMMAND d3d12_depth_view_format_support_test)
    set_tests_properties(d3d12_depth_view_format_support PROPERTIES SKIP_RETURN_CODE 77)

    # A placed buffer keeps its heap's arena range after the application releases the heap.
    add_executable(d3d12_placed_heap_lifetime_test test/d3d12_placed_heap_lifetime_test.cpp)
    target_link_libraries(d3d12_placed_heap_lifetime_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_placed_heap_lifetime_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_placed_heap_lifetime_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_placed_heap_lifetime COMMAND d3d12_placed_heap_lifetime_test)
    set_tests_properties(d3d12_placed_heap_lifetime PROPERTIES SKIP_RETURN_CODE 77
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")

    # ClearUnorderedAccessViewUint/Float on buffers and textures, read back through D3D12.
    add_executable(d3d12_uav_clear_test test/d3d12_uav_clear_test.cpp)
    target_link_libraries(d3d12_uav_clear_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_uav_clear_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_uav_clear_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_uav_clear COMMAND d3d12_uav_clear_test)
    set_tests_properties(d3d12_uav_clear PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(d3d12_short_uav_clear_test test/d3d12_short_uav_clear_test.cpp)
    target_link_libraries(d3d12_short_uav_clear_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_short_uav_clear_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_short_uav_clear_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_short_uav_clear COMMAND d3d12_short_uav_clear_test)
    set_tests_properties(d3d12_short_uav_clear PROPERTIES SKIP_RETURN_CODE 77
      ENVIRONMENT "LEONIDA_HOST_ADAPTER=warp")

    # Emulation of R9G9B9E5_SHAREDEXP UAV textures as mutable R32_UINT on GPUs without E5B9G9R9 storage:
    # format support, 1D/2D/3D/reserved creation, SRV/UAV T#s, WARP differential UAV clears, cross-backing copies,
    # and set-0 storage write -> E5B9G9R9 sampled HDR round-trip.
    add_executable(d3d12_d24_support_test test/d3d12_d24_support_test.cpp)
    target_link_libraries(d3d12_d24_support_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_d24_support_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_d24_support_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_d24_support COMMAND d3d12_d24_support_test)
    set_tests_properties(d3d12_d24_support PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(d3d12_b5g5r5a1_uav_test test/d3d12_b5g5r5a1_uav_test.cpp)
    target_link_libraries(d3d12_b5g5r5a1_uav_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_b5g5r5a1_uav_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_b5g5r5a1_uav_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_b5g5r5a1_uav COMMAND d3d12_b5g5r5a1_uav_test)
    set_tests_properties(d3d12_b5g5r5a1_uav PROPERTIES SKIP_RETURN_CODE 77)
    # B4G4R4A4 render targets as RGBA8 images where no 4444 format is a colour attachment (NVIDIA).
    add_executable(d3d12_b4g4r4a4_target_test test/d3d12_b4g4r4a4_target_test.cpp)
    target_link_libraries(d3d12_b4g4r4a4_target_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_b4g4r4a4_target_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_b4g4r4a4_target_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_b4g4r4a4_target COMMAND d3d12_b4g4r4a4_target_test)
    set_tests_properties(d3d12_b4g4r4a4_target PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(d3d12_srgb_uav_test test/d3d12_srgb_uav_test.cpp)
    target_link_libraries(d3d12_srgb_uav_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_srgb_uav_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_srgb_uav_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_srgb_uav COMMAND d3d12_srgb_uav_test)
    set_tests_properties(d3d12_srgb_uav PROPERTIES SKIP_RETURN_CODE 77)  # native sRGB storage, no Vulkan GPU, or Wine
    add_executable(d3d12_r9g9b9e5_uav_test test/d3d12_r9g9b9e5_uav_test.cpp)
    target_link_libraries(d3d12_r9g9b9e5_uav_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_r9g9b9e5_uav_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_r9g9b9e5_uav_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_r9g9b9e5_uav COMMAND d3d12_r9g9b9e5_uav_test)
    set_tests_properties(d3d12_r9g9b9e5_uav PROPERTIES SKIP_RETURN_CODE 77)
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      set(LEONIDA_TEST_CS_R9G9B9E5_UAV ${CMAKE_CURRENT_BINARY_DIR}/generated/cs_r9g9b9e5_uav.spv)
      add_custom_command(
        OUTPUT ${LEONIDA_TEST_CS_R9G9B9E5_UAV}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${LEONIDA_TEST_CS_R9G9B9E5_UAV}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_r9g9b9e5_uav.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_r9g9b9e5_uav.comp
        COMMENT "Compiling cs_r9g9b9e5_uav.comp"
        VERBATIM)
      target_sources(d3d12_r9g9b9e5_uav_test PRIVATE ${LEONIDA_TEST_CS_R9G9B9E5_UAV})
      target_compile_definitions(d3d12_r9g9b9e5_uav_test PRIVATE
        LEONIDA_TEST_CS_R9G9B9E5_UAV_SPV="${LEONIDA_TEST_CS_R9G9B9E5_UAV}")
    endif()

    # Debug markers as VK_EXT_debug_utils labels, and private data on every object (D3D-24).
    add_executable(d3d12_debug_markers_test test/d3d12_debug_markers_test.cpp)
    target_link_libraries(d3d12_debug_markers_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_debug_markers_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_debug_markers_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_debug_markers COMMAND d3d12_debug_markers_test)
    set_tests_properties(d3d12_debug_markers PROPERTIES SKIP_RETURN_CODE 77)

    # Existing-memory heaps and lifetime trackers: the arguments the host refuses (OpenExistingHeapFromAddress,
    # OpenExistingHeapFromFileMapping, CreateLifetimeTracker).
    add_executable(d3d12_existing_heap_test test/d3d12_existing_heap_test.cpp)
    target_link_libraries(d3d12_existing_heap_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_existing_heap_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_existing_heap_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_existing_heap COMMAND d3d12_existing_heap_test)
    add_test(NAME d3d12_host_prefix COMMAND d3d12_existing_heap_test --host-prefix-only)
    set_tests_properties(d3d12_host_prefix PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(d3d12_existing_heap PROPERTIES SKIP_RETURN_CODE 77)
    # LEONIDA_TEXTURE_REFRESH_ASYNC=1 (set by the test itself): a mapped plane's first submission reads its bytes.
    add_test(NAME d3d12_mapped_plane_async COMMAND d3d12_existing_heap_test --mapped-plane-async)
    set_tests_properties(d3d12_mapped_plane_async PROPERTIES SKIP_RETURN_CODE 77)
    # The same with write watching off: the guarded cases only, the concurrent writers among them.
    add_test(NAME d3d12_existing_heap_no_write_watch COMMAND d3d12_existing_heap_test)
    set_tests_properties(d3d12_existing_heap_no_write_watch PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "LEONIDA_WRITE_WATCH=0")
    # And with it on whatever the host's default (off under Wine).
    add_test(NAME d3d12_existing_heap_write_watch COMMAND d3d12_existing_heap_test)
    set_tests_properties(d3d12_existing_heap_write_watch PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "LEONIDA_WRITE_WATCH=1")
    # Slot 133 on untouched xmem physical pages: committed through the pages hook, then imported. xmem.cpp and
    # registry.cpp are compiled in, as xmem_test does, to reach the registered XMem* exports.
    add_executable(d3d12_xmem_placed_test test/d3d12_xmem_placed_test.cpp shims/xmem.cpp shims/registry.cpp)
    target_link_libraries(d3d12_xmem_placed_test PRIVATE gta6rt_d3d12_com d3d12 dxgi psapi)
    target_compile_definitions(d3d12_xmem_placed_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_xmem_placed_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(d3d12_xmem_placed_test PRIVATE /OPT:NOREF)
    add_test(NAME d3d12_xmem_placed COMMAND d3d12_xmem_placed_test)
    add_test(NAME xbox_linear_texture_map COMMAND d3d12_xmem_placed_test --linear-map)
    add_test(NAME xbox_linear_gpu_staging COMMAND d3d12_xmem_placed_test --linear-staging)
    # Resource::Map asks the linear mapping of a slot 133 texture first only with LEONIDA_LINEAR_MAP_FIRST=1.
    set_tests_properties(xbox_linear_gpu_staging PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
                         ENVIRONMENT "LEONIDA_LINEAR_MAP_FIRST=1")
    set_tests_properties(xbox_linear_texture_map PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
                         ENVIRONMENT "LEONIDA_LINEAR_MAP_FIRST=1")
    set_tests_properties(d3d12_xmem_placed PROPERTIES SKIP_RETURN_CODE 77)

    # Fences and timing (D3D-30): multiple-fence waits, clock calibration, adapter LUID, custom heap properties.
    add_executable(d3d12_fence_timing_test test/d3d12_fence_timing_test.cpp)
    target_link_libraries(d3d12_fence_timing_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_fence_timing_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_fence_timing_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_fence_timing COMMAND d3d12_fence_timing_test)
    set_tests_properties(d3d12_fence_timing PROPERTIES SKIP_RETURN_CODE 77)
    # CPU subresource copies through the staging queue wait for their own submission, not the direct queue (D9).
    add_executable(d3d12_staged_copy_wait_test test/d3d12_staged_copy_wait_test.cpp)
    target_link_libraries(d3d12_staged_copy_wait_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_staged_copy_wait_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_staged_copy_wait_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_staged_copy_wait COMMAND d3d12_staged_copy_wait_test)
    set_tests_properties(d3d12_staged_copy_wait PROPERTIES SKIP_RETURN_CODE 77)
    # Fence events the host runtime sets or refuses (released fences, no fences), compared with the host devices.
    add_executable(d3d12_fence_events_test test/d3d12_fence_events_test.cpp)
    target_link_libraries(d3d12_fence_events_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_fence_events_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_fence_events_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_fence_events COMMAND d3d12_fence_events_test)
    set_tests_properties(d3d12_fence_events PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(d3d12_fence_device_lost_counter_test test/d3d12_fence_device_lost_counter_test.cpp)
    target_link_libraries(d3d12_fence_device_lost_counter_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_fence_device_lost_counter_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_fence_device_lost_counter_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_fence_device_lost_counter COMMAND d3d12_fence_device_lost_counter_test)
    set_tests_properties(d3d12_fence_device_lost_counter PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    # Xbox-ABI queue and fence proxies behind the device proxy, on the host runtime (WARP, a hardware adapter) and
    # on the Vulkan device: forwarding, refusal past the public chain, fence unwrapping. 77: no device at all.
    add_executable(queue_proxy_test test/queue_proxy_test.cpp)
    target_link_libraries(queue_proxy_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(queue_proxy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(queue_proxy_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    add_test(NAME queue_proxy COMMAND queue_proxy_test)
    set_tests_properties(queue_proxy PROPERTIES SKIP_RETURN_CODE 77)
    # Xbox-ABI command allocator proxies (device slot 9) on the Vulkan device, and the unwrapped PC allocator on the
    # host runtime: forwarding, refused tail, guard region, lists and GPU copies on the proxy. 77: no device at all.
    add_executable(allocator_proxy_test test/allocator_proxy_test.cpp)
    target_link_libraries(allocator_proxy_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(allocator_proxy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(allocator_proxy_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME allocator_proxy COMMAND allocator_proxy_test)
    set_tests_properties(allocator_proxy PROPERTIES SKIP_RETURN_CODE 77)
    # IDXGIDevice1 on the device proxy (the GDKX boot step: QueryInterface, GetAdapter, EnumOutputs), on the host
    # runtime and on the Vulkan device: COM identity with the device, the adapter's LUID. 77: no device at all.
    add_executable(dxgi_device_test test/dxgi_device_test.cpp)
    target_link_libraries(dxgi_device_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(dxgi_device_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dxgi_device_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME dxgi_device COMMAND dxgi_device_test)
    set_tests_properties(dxgi_device PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(dxgi_host_answers_test test/dxgi_host_answers_test.cpp)
    target_link_libraries(dxgi_host_answers_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(dxgi_host_answers_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dxgi_host_answers_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME dxgi_host_answers COMMAND dxgi_host_answers_test)
    set_tests_properties(dxgi_host_answers PROPERTIES SKIP_RETURN_CODE 77)
    # The countdown behind those event-backed waits, with no GPU: the full unsigned count, no wrap past zero.
    add_executable(d3d12_fence_waiter_test test/d3d12_fence_waiter_test.cpp)
    target_compile_definitions(d3d12_fence_waiter_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_fence_waiter_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_fence_waiter COMMAND d3d12_fence_waiter_test)
    # The 1/2/3 method variants (D3D-28) against the methods they wrap, on the Vulkan device directly.
    add_executable(d3d12_method_variants_test test/d3d12_method_variants_test.cpp)
    target_link_libraries(d3d12_method_variants_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_method_variants_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_method_variants_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_method_variants COMMAND d3d12_method_variants_test)
    # ID3D12Device9::CreateShaderCacheSession, ID3D12ShaderCacheSession and ShaderCacheControl (D3D-34).
    add_executable(d3d12_shader_cache_test test/d3d12_shader_cache_test.cpp)
    target_link_libraries(d3d12_shader_cache_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_shader_cache_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_shader_cache_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_shader_cache COMMAND d3d12_shader_cache_test)
    set_tests_properties(d3d12_shader_cache PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(d3d12_method_variants PROPERTIES SKIP_RETURN_CODE 77)
    # ID3D12Object private data (GetPrivateData, SetPrivateData, SetPrivateDataInterface, SetName) compared with
    # the host runtime's answers on the device and four child objects.
    add_executable(d3d12_private_data_test test/d3d12_private_data_test.cpp)
    target_link_libraries(d3d12_private_data_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_private_data_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_private_data_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_private_data COMMAND d3d12_private_data_test)
    set_tests_properties(d3d12_private_data PROPERTIES SKIP_RETURN_CODE 77)

    # CreateSampler2 (D3D-28) against CreateSampler, and its refusals against the host's PC runtime.
    add_executable(d3d12_sampler2_test test/d3d12_sampler2_test.cpp)
    target_link_libraries(d3d12_sampler2_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_sampler2_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_sampler2_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_sampler2 COMMAND d3d12_sampler2_test)
    set_tests_properties(d3d12_sampler2 PROPERTIES SKIP_RETURN_CODE 77)

    # Descriptors through the proxy: copies between heaps, what texture views and samplers write into their slots,
    # and (where glslangValidator is found) a compute shader the test runs on the device's bindless set, which
    # finds a T# and an S# through the hash table and samples. The shader is GLSL, compiled here.
    add_executable(d3d12_descriptor_test test/d3d12_descriptor_test.cpp)
    target_link_libraries(d3d12_descriptor_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_descriptor_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_descriptor_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_descriptor COMMAND d3d12_descriptor_test)
    set_tests_properties(d3d12_descriptor PROPERTIES SKIP_RETURN_CODE 77)

    # Console descriptor heap T# binding rules and per-reason refusal tracking (RT-UNBOUND-TSHARP-REASONS).
    add_executable(d3d12_unbound_tsharp_rules_test test/d3d12_unbound_tsharp_rules_test.cpp)
    target_link_libraries(d3d12_unbound_tsharp_rules_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_unbound_tsharp_rules_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_unbound_tsharp_rules_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_unbound_tsharp_rules COMMAND d3d12_unbound_tsharp_rules_test)
    set_tests_properties(d3d12_unbound_tsharp_rules PROPERTIES SKIP_RETURN_CODE 77)
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      function(descriptor_test_shader shader definition)
        set(source ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${shader})
        set(binary ${CMAKE_CURRENT_BINARY_DIR}/generated/${definition}.spv)
        add_custom_command(OUTPUT ${binary}
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 ${ARGN} -o ${binary} ${source}
          DEPENDS ${source} COMMENT "Compiling descriptor test shader ${shader}" VERBATIM)
        target_sources(d3d12_descriptor_test PRIVATE ${binary})
        target_compile_definitions(d3d12_descriptor_test PRIVATE ${definition}="${binary}")
      endfunction()
      descriptor_test_shader(cs_sample.comp LEONIDA_TEST_CS_SAMPLE_SPV)
      descriptor_test_shader(cs_sample.comp LEONIDA_TEST_GATHER_ALPHA_SPV -DTEST_GATHER_ALPHA=1)
      descriptor_test_shader(cs_sample.comp LEONIDA_TEST_MS_FLOAT_SPV -DTEST_MS_FLOAT=1)
      descriptor_test_shader(cs_sample.comp LEONIDA_TEST_MS_UINT_SPV -DTEST_MS_UINT=1)
      descriptor_test_shader(cs_sample.comp LEONIDA_TEST_MS_SINT_SPV -DTEST_MS_SINT=1)
      descriptor_test_shader(msaa_seed.vert LEONIDA_TEST_MSAA_VERT_SPV)
      descriptor_test_shader(msaa_seed.frag LEONIDA_TEST_MSAA_FRAG_SPV)
    else()
      message(STATUS "glslangValidator not found: d3d12_descriptor_test skips its shader")
    endif()

    add_executable(d3d12_predicated_copy_test test/d3d12_predicated_copy_test.cpp)
    target_link_libraries(d3d12_predicated_copy_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_predicated_copy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_predicated_copy_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_predicated_copy COMMAND d3d12_predicated_copy_test)
    set_tests_properties(d3d12_predicated_copy PROPERTIES SKIP_RETURN_CODE 77)
    # A DISPATCH-only ExecuteIndirect signature with a GPU count buffer: native Windows WARP versus Vulkan GPU,
    # including a GPU-written count that changes between two calls in the same command list.
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      set(_indirect_count_spv ${CMAKE_CURRENT_BINARY_DIR}/generated/cs_indirect_dispatch_count.spv)
      add_custom_command(OUTPUT ${_indirect_count_spv}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.2
          -o ${_indirect_count_spv} ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_indirect_dispatch_count.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_indirect_dispatch_count.comp
        COMMENT "Compiling the counted indirect dispatch test shader" VERBATIM)
      add_custom_target(gta6rt_indirect_count_shader DEPENDS ${_indirect_count_spv})
      add_executable(d3d12_indirect_dispatch_count_test test/d3d12_indirect_dispatch_count_test.cpp)
      add_dependencies(d3d12_indirect_dispatch_count_test gta6rt_indirect_count_shader)
      target_link_libraries(d3d12_indirect_dispatch_count_test PRIVATE gta6rt_d3d12_com d3d12 dxgi version)
      target_compile_definitions(d3d12_indirect_dispatch_count_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        LEONIDA_TEST_INDIRECT_COUNT_SPV="${_indirect_count_spv}")
      target_compile_options(d3d12_indirect_dispatch_count_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_indirect_dispatch_count COMMAND d3d12_indirect_dispatch_count_test)
      add_test(NAME d3d12_indirect_group_budget COMMAND d3d12_indirect_dispatch_count_test --clamp)
      set_tests_properties(d3d12_indirect_group_budget PROPERTIES SKIP_RETURN_CODE 77
        ENVIRONMENT "LEONIDA_INDIRECT_GROUP_BUDGET=2")
      set_tests_properties(d3d12_indirect_dispatch_count PROPERTIES SKIP_RETURN_CODE 77)

      # A single ordinary compute PSO and direct dispatch for cold/warm production journal comparisons;
      # only the authored two-counter shader is shared with the indirect test.
      set(_journal_second_spv ${CMAKE_CURRENT_BINARY_DIR}/generated/cs_compute_journal_v2.spv)
      add_custom_command(OUTPUT ${_journal_second_spv}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.2 -DTEST_COUNTER_TERM=2u
          -o ${_journal_second_spv} ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_indirect_dispatch_count.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_indirect_dispatch_count.comp
        COMMENT "Compiling the second authored compute journal control" VERBATIM)
      # The first shader comes from gta6rt_indirect_count_shader: naming its output here too would give Make a second
      # rule for the same file, and the two glslangValidator runs raced ("Failed to open file").
      add_custom_target(gta6rt_compute_journal_shader DEPENDS ${_journal_second_spv})
      add_executable(d3d12_compute_journal_test test/d3d12_compute_journal_test.cpp)
      add_dependencies(d3d12_compute_journal_test gta6rt_indirect_count_shader gta6rt_compute_journal_shader)
      target_link_libraries(d3d12_compute_journal_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_compute_journal_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        LEONIDA_TEST_COUNTER_SPV="${_indirect_count_spv}" LEONIDA_TEST_COUNTER_V2_SPV="${_journal_second_spv}")
      target_compile_options(d3d12_compute_journal_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_compute_journal COMMAND d3d12_compute_journal_test)
      set_tests_properties(d3d12_compute_journal PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()

    # The cache vk_pipeline_check saves has to hit on the runtime's device (docs/pipeline_check.md): the test
    # compiles an embedded synthetic compute shader through a same-named copy of the checker, then creates the
    # same pipeline on the runtime's device from the saved cache.
    add_executable(pipeline_cache_seed_test test/pipeline_cache_seed_test.cpp)
    add_dependencies(pipeline_cache_seed_test vk_pipeline_check)
    target_link_libraries(pipeline_cache_seed_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(pipeline_cache_seed_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(pipeline_cache_seed_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME pipeline_cache_seed COMMAND pipeline_cache_seed_test $<TARGET_FILE:vk_pipeline_check>)
    set_tests_properties(pipeline_cache_seed PROPERTIES SKIP_RETURN_CODE 77)

    # Test descriptor creation and clears in CTest without shader tools. The GPU runner passes the translated
    # cs_texture_uav.spv to also exercise dispatches through set 0 and compare texels with Windows WARP.
    add_executable(d3d12_texture_uav_test test/d3d12_texture_uav_test.cpp)
    target_link_libraries(d3d12_texture_uav_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_texture_uav_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_texture_uav_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_texture_uav COMMAND d3d12_texture_uav_test --descriptors-only)
    set_tests_properties(d3d12_texture_uav PROPERTIES SKIP_RETURN_CODE 77)

    # What Close returns after one recorded call, against WARP; today's differences are listed in the test.
    add_executable(d3d12_close_parity_test test/d3d12_close_parity_test.cpp)
    target_link_libraries(d3d12_close_parity_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_close_parity_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_close_parity_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_close_parity COMMAND d3d12_close_parity_test)
    set_tests_properties(d3d12_close_parity PROPERTIES SKIP_RETURN_CODE 77)

    # ExecuteIndirect recording-time Close() parity against live WARP and hardware D3D12, plus GPU dispatch sanity.
    add_executable(d3d12_execute_indirect_validation_test test/d3d12_execute_indirect_validation_test.cpp)
    target_link_libraries(d3d12_execute_indirect_validation_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_execute_indirect_validation_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_execute_indirect_validation_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_execute_indirect_validation COMMAND d3d12_execute_indirect_validation_test)
    set_tests_properties(d3d12_execute_indirect_validation PROPERTIES
      SKIP_RETURN_CODE 77
      FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")

    # Set{Compute,Graphics}Root* closed-list, zero-count null, CBV alignment, and foreign root signature parity.
    add_executable(d3d12_root_args_validation_test test/d3d12_root_args_validation_test.cpp)
    target_link_libraries(d3d12_root_args_validation_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_root_args_validation_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_root_args_validation_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_root_args_validation COMMAND d3d12_root_args_validation_test)
    set_tests_properties(d3d12_root_args_validation PROPERTIES
      SKIP_RETURN_CODE 77
      FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")

    # ClearState predication reset (GPU readback) + SetPredication / AtomicCopyBuffer Close() parity against WARP & HW.
    add_executable(d3d12_predication_atomic_copy_test test/d3d12_predication_atomic_copy_test.cpp)
    target_link_libraries(d3d12_predication_atomic_copy_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_predication_atomic_copy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_predication_atomic_copy_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_predication_atomic_copy COMMAND d3d12_predication_atomic_copy_test)
    set_tests_properties(d3d12_predication_atomic_copy PROPERTIES
      SKIP_RETURN_CODE 77
      FAIL_REGULAR_EXPRESSION "Validation Error;VUID-"
    )

    # Differential regression: run explicitly (requires Vulkan hardware and Windows WARP).
    add_executable(d3d12_clear_test test/d3d12_clear_test.cpp)
    target_link_libraries(d3d12_clear_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_clear_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_clear_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_rgb9e5_rtv COMMAND d3d12_clear_test --rgb9e5)
    set_tests_properties(d3d12_rgb9e5_rtv PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME d3d12_clear COMMAND d3d12_clear_test)
    add_test(NAME d3d12_clear_restricted_depth COMMAND d3d12_clear_test --restricted-depth)
    set_tests_properties(d3d12_clear d3d12_clear_restricted_depth PROPERTIES SKIP_RETURN_CODE 77)

    # Synthetic typed-view readbacks against Windows WARP; run explicitly on a Vulkan GPU.
    add_executable(d3d12_typeless_rtv_test test/d3d12_typeless_rtv_test.cpp)
    target_link_libraries(d3d12_typeless_rtv_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_typeless_rtv_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_typeless_rtv_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_typeless_rtv COMMAND d3d12_typeless_rtv_test)
    # 1D render targets and depth buffers: creation against the host and WARP, clears read back against WARP.
    add_executable(d3d12_texture1d_target_test test/d3d12_texture1d_target_test.cpp)
    target_link_libraries(d3d12_texture1d_target_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_texture1d_target_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_texture1d_target_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_texture1d_target COMMAND d3d12_texture1d_target_test)
    set_tests_properties(d3d12_texture1d_target PROPERTIES SKIP_RETURN_CODE 77)  # no Vulkan GPU, or Wine
    add_executable(d3d12_a8_dimensions_test test/d3d12_a8_dimensions_test.cpp)
    target_link_libraries(d3d12_a8_dimensions_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_a8_dimensions_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_a8_dimensions_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_a8_dimensions COMMAND d3d12_a8_dimensions_test)
    set_tests_properties(d3d12_a8_dimensions PROPERTIES SKIP_RETURN_CODE 77)  # no Vulkan GPU, or Wine

    # ArraySize -1 in array RTVs and DSVs (every slice from the first on), against the host on the same adapter
    # and WARP.
    add_executable(d3d12_view_slices_test test/d3d12_view_slices_test.cpp)
    target_link_libraries(d3d12_view_slices_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_view_slices_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_view_slices_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_view_slices COMMAND d3d12_view_slices_test)
    set_tests_properties(d3d12_view_slices PROPERTIES SKIP_RETURN_CODE 77)

    # Typed RTV clears and draws of every TYPELESS color family against the host on the same adapter and WARP
    # (D3D-275). The draws need glslangValidator for their GLSL stand-ins of translated shaders.
    add_executable(d3d12_typeless_families_test test/d3d12_typeless_families_test.cpp)
    target_link_libraries(d3d12_typeless_families_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_typeless_families_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_typeless_families_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_typeless_families COMMAND d3d12_typeless_families_test)
    set_tests_properties(d3d12_typeless_families PROPERTIES SKIP_RETURN_CODE 77)
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      foreach(pair "vs_fullscreen.vert;LEONIDA_TYPELESS_VS_SPV;typeless_vs.spv"
                   "ps_typeless_float.frag;LEONIDA_TYPELESS_PS_FLOAT_SPV;typeless_ps_float.spv"
                   "ps_typeless_uint.frag;LEONIDA_TYPELESS_PS_UINT_SPV;typeless_ps_uint.spv")
        list(GET pair 0 shader)
        list(GET pair 1 definition)
        list(GET pair 2 name)
        set(binary ${CMAKE_CURRENT_BINARY_DIR}/generated/${name})
        add_custom_command(OUTPUT ${binary}
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${binary}
                  ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${shader}
          DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${shader}
          COMMENT "Compiling the typeless draw test shader ${shader}" VERBATIM)
        target_sources(d3d12_typeless_families_test PRIVATE ${binary})
        target_compile_definitions(d3d12_typeless_families_test PRIVATE ${definition}="${binary}")
      endforeach()
    else()
      message(STATUS "glslangValidator not found: d3d12_typeless_families_test runs its clears only")
    endif()

    # Fixed-function state (blend, depth, stencil) swept against the host D3D12 device on the same adapter. Needs
    # glslangValidator for the GLSL stand-ins of translated shaders.
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      add_executable(d3d12_state_sweep_test test/d3d12_state_sweep_test.cpp)
      target_link_libraries(d3d12_state_sweep_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
      target_compile_definitions(d3d12_state_sweep_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(d3d12_state_sweep_test PRIVATE /W4 /permissive- /utf-8)
      foreach(pair "vs_fullscreen.vert;LEONIDA_SWEEP_VS_SPV;sweep_vs.spv"
                   "ps_sweep.frag;LEONIDA_SWEEP_PS_SPV;sweep_ps.spv")
        list(GET pair 0 shader)
        list(GET pair 1 definition)
        list(GET pair 2 name)
        set(binary ${CMAKE_CURRENT_BINARY_DIR}/generated/${name})
        add_custom_command(OUTPUT ${binary}
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${binary}
                  ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${shader}
          DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${shader}
          COMMENT "Compiling the state sweep test shader ${shader}" VERBATIM)
        target_sources(d3d12_state_sweep_test PRIVATE ${binary})
        target_compile_definitions(d3d12_state_sweep_test PRIVATE ${definition}="${binary}")
      endforeach()
      add_test(NAME d3d12_state_sweep COMMAND d3d12_state_sweep_test)
      set_tests_properties(d3d12_state_sweep PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # 96-bit R32G32B32 textures (D3D-62): footprints and every copy path against Windows WARP.
    add_executable(d3d12_rgb96_test test/d3d12_rgb96_test.cpp)
    target_link_libraries(d3d12_rgb96_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_rgb96_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_rgb96_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_rgb96 COMMAND d3d12_rgb96_test)
    set_tests_properties(d3d12_rgb96 PROPERTIES SKIP_RETURN_CODE 77)  # no Vulkan GPU or no WARP adapter
    set_tests_properties(d3d12_typeless_rtv PROPERTIES SKIP_RETURN_CODE 77)

    # Depth/stencil ResolveSubresourceRegion against the host runtime and WARP; run explicitly on a Vulkan GPU.
    add_executable(d3d12_depth_resolve_test test/d3d12_depth_resolve_test.cpp)
    target_link_libraries(d3d12_depth_resolve_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_depth_resolve_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_depth_resolve_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_depth_resolve COMMAND d3d12_depth_resolve_test)
    set_tests_properties(d3d12_depth_resolve PROPERTIES SKIP_RETURN_CODE 77)
    # The edge cases (TEST-848) draw with a GLSL stand-in for a translated vertex shader; without glslangValidator
    # only the clear cases run.
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      set(binary ${CMAKE_CURRENT_BINARY_DIR}/generated/depth_edge_vs.spv)
      add_custom_command(OUTPUT ${binary}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${binary}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/vs_depth_edge.vert
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/vs_depth_edge.vert
        COMMENT "Compiling the depth resolve test shader vs_depth_edge.vert" VERBATIM)
      target_sources(d3d12_depth_resolve_test PRIVATE ${binary})
      target_compile_definitions(d3d12_depth_resolve_test PRIVATE LEONIDA_DEPTH_EDGE_VS_SPV="${binary}")
    endif()

    # ID3D12Resource::GetDesc against the host runtime and WARP; run explicitly on a Vulkan GPU.
    add_executable(d3d12_resource_desc_test test/d3d12_resource_desc_test.cpp)
    target_link_libraries(d3d12_resource_desc_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_resource_desc_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_resource_desc_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_resource_desc COMMAND d3d12_resource_desc_test)

    # Multi-viewport & multi-scissor rendering (RSSetViewports / RSSetScissorRects 1..16) against host D3D12.
    add_executable(d3d12_multi_viewport_test test/d3d12_multi_viewport_test.cpp)
    target_link_libraries(d3d12_multi_viewport_test PRIVATE gta6rt_d3d12_com d3d12 dxgi dxguid)
    target_compile_definitions(d3d12_multi_viewport_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_multi_viewport_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_multi_viewport COMMAND d3d12_multi_viewport_test)
    set_tests_properties(d3d12_multi_viewport PROPERTIES SKIP_RETURN_CODE 77
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
    set_tests_properties(d3d12_resource_desc PROPERTIES SKIP_RETURN_CODE 77)

    # Typeless depth-stencil textures without ALLOW_DEPTH_STENCIL, copied from a depth buffer, against WARP.
    add_executable(d3d12_typeless_depth_test test/d3d12_typeless_depth_test.cpp)
    target_link_libraries(d3d12_typeless_depth_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_typeless_depth_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_typeless_depth_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_typeless_depth COMMAND d3d12_typeless_depth_test)
    set_tests_properties(d3d12_typeless_depth PROPERTIES SKIP_RETURN_CODE 77)

    # Manifest selection, aspect-fit decoding and Vulkan upload of a synthetic package splash.
    add_executable(package_splash_test test/package_splash_test.cpp)
    target_link_libraries(package_splash_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(package_splash_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(package_splash_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME package_splash COMMAND package_splash_test)
    set_tests_properties(package_splash PROPERTIES SKIP_RETURN_CODE 77)

    # WriteToSubresource/ReadFromSubresource arguments and bytes, compared with the host runtime.
    add_executable(d3d12_subresource_copy_test test/d3d12_subresource_copy_test.cpp)
    target_link_libraries(d3d12_subresource_copy_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_subresource_copy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_subresource_copy_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_subresource_copy COMMAND d3d12_subresource_copy_test)
    set_tests_properties(d3d12_subresource_copy PROPERTIES SKIP_RETURN_CODE 77)

    # MakeResident, Evict, SetResidencyPriority and EnqueueMakeResident, compared with the host runtime.
    add_executable(d3d12_residency_test test/d3d12_residency_test.cpp)
    target_link_libraries(d3d12_residency_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_residency_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_residency_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_residency COMMAND d3d12_residency_test)
    set_tests_properties(d3d12_residency PROPERTIES SKIP_RETURN_CODE 77)

    # ID3D12Resource::GetHeapProperties for committed, placed and reserved resources, compared with the host runtime.
    add_executable(d3d12_heap_properties_test test/d3d12_heap_properties_test.cpp)
    target_link_libraries(d3d12_heap_properties_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_heap_properties_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_heap_properties_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_heap_properties COMMAND d3d12_heap_properties_test)
    set_tests_properties(d3d12_heap_properties PROPERTIES SKIP_RETURN_CODE 77)
    # The title HUD's VRAM figure from VK_EXT_memory_budget (rt::d3d12::vulkan_vram_budget) on a real device.
    add_executable(d3d12_vram_budget_test test/d3d12_vram_budget_test.cpp)
    target_link_libraries(d3d12_vram_budget_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_vram_budget_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_vram_budget_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_vram_budget COMMAND d3d12_vram_budget_test)
    set_tests_properties(d3d12_vram_budget PROPERTIES SKIP_RETURN_CODE 77)

    # CreatePipelineLibrary and ID3D12PipelineLibrary1 arguments, compared with the host runtime.
    add_executable(d3d12_pipeline_library_host_test test/d3d12_pipeline_library_host_test.cpp)
    target_link_libraries(d3d12_pipeline_library_host_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_pipeline_library_host_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_pipeline_library_host_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_pipeline_library_host COMMAND d3d12_pipeline_library_host_test)
    set_tests_properties(d3d12_pipeline_library_host PROPERTIES SKIP_RETURN_CODE 77)

    # Milestone M0: a window, 60 cleared frames presented through the proxy on Vulkan.
    add_executable(xbox_cbv_vector_test test/xbox_cbv_vector_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp)
    target_link_libraries(xbox_cbv_vector_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(xbox_cbv_vector_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX LEONIDA_VULKAN_DEVICE)
    add_test(NAME xbox_cbv_vectors COMMAND xbox_cbv_vector_test)
    set_tests_properties(xbox_cbv_vectors PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
    # With LEONIDA_CBV_RAW=1 a view made on the device itself keeps the byte-address form.
    add_test(NAME xbox_cbv_vectors_raw COMMAND xbox_cbv_vector_test)
    set_tests_properties(xbox_cbv_vectors_raw PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20 ENVIRONMENT "LEONIDA_CBV_RAW=1")
    add_executable(xbox_depth_storage_test test/xbox_depth_storage_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp)
    target_link_libraries(xbox_depth_storage_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(xbox_depth_storage_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX LEONIDA_VULKAN_DEVICE)
    add_test(NAME xbox_depth_storage COMMAND xbox_depth_storage_test)
    add_test(NAME xbox_d16s8_storage COMMAND xbox_depth_storage_test)
    # The console's D16S8 family keeps its own path where the GPU has no D24S8 attachment, and is the D24S8 family
    # where it has one; LEONIDA_CONSOLE_D16S8=native asks for the D16/S8 path on every GPU (pc_format).
    set_tests_properties(xbox_d16s8_storage PROPERTIES
                         ENVIRONMENT "LEONIDA_TEST_D16S8_UAV=1;LEONIDA_CONSOLE_D16S8=native" SKIP_RETURN_CODE 77 TIMEOUT 30)
    set_tests_properties(xbox_depth_storage PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    # Made as D24S8 (the default on GPUs with D24S8 attachments, e.g. NVIDIA), a D16S8 depth-stencil texture keeps
    # its UAVs through the widened R16_TYPELESS + R8_UINT planes and Depth24Converter.
    add_test(NAME xbox_d16s8_d24_storage COMMAND xbox_depth_storage_test)
    set_tests_properties(xbox_d16s8_d24_storage PROPERTIES
                         ENVIRONMENT "LEONIDA_TEST_D16S8_UAV=1;LEONIDA_CONSOLE_D16S8=d24" SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_executable(xbox_d16s8_surface_test test/xbox_d16s8_surface_test.cpp)
    target_link_libraries(xbox_d16s8_surface_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(xbox_d16s8_surface_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_test(NAME xbox_d16s8_surface COMMAND xbox_d16s8_surface_test)
    set_tests_properties(xbox_d16s8_surface PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20
                         ENVIRONMENT "LEONIDA_CONSOLE_D16S8=native")
    add_executable(xbox_gpu_label_test test/xbox_gpu_label_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp)
    target_link_libraries(xbox_gpu_label_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(xbox_gpu_label_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX LEONIDA_VULKAN_DEVICE)
    add_test(NAME xbox_gpu_label_completion COMMAND xbox_gpu_label_test)
    set_tests_properties(xbox_gpu_label_completion PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
    # The list values' write fallbacks (src/value_write_sink.h): an arena offset lands in the arena, a PAGE_NOACCESS
    # page of xmem's through xmem's own mapping. xmem.cpp and registry.cpp are compiled in for the xmem.dll exports.
    add_executable(xbox_list_value_fallback_test test/xbox_list_value_fallback_test.cpp shims/d3d12_xs_device.cpp
                   shims/d3d12_xs_pso_stream.cpp shims/xmem.cpp shims/registry.cpp)
    target_link_libraries(xbox_list_value_fallback_test PRIVATE gta6rt_d3d12_com psapi)
    target_compile_definitions(xbox_list_value_fallback_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX LEONIDA_VULKAN_DEVICE)
    target_link_options(xbox_list_value_fallback_test PRIVATE /OPT:NOREF)
    add_test(NAME xbox_list_value_fallback COMMAND xbox_list_value_fallback_test)
    set_tests_properties(xbox_list_value_fallback PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_executable(d3d12_present_test test/d3d12_present_test.cpp)
    target_link_libraries(d3d12_present_test PRIVATE gta6rt_d3d12_com user32 gdi32 dxgi)
    target_compile_definitions(d3d12_present_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_present_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_present COMMAND d3d12_present_test)
    set_tests_properties(d3d12_present PROPERTIES SKIP_RETURN_CODE 77)

    # Per-pipeline --f16-interp-fallback linking (SHADER-378): 16-param VS + colliding PS rendered on Vulkan.
    add_executable(d3d12_f16_halves_test test/d3d12_f16_halves_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp)
    target_link_libraries(d3d12_f16_halves_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_f16_halves_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_f16_halves_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_f16_halves COMMAND d3d12_f16_halves_test)
    set_tests_properties(d3d12_f16_halves PROPERTIES SKIP_RETURN_CODE 77)

    # PresentX and the frame events on the Vulkan device (shims/xbox_present.h), checked on swapchain pixels.
    add_executable(xbox_present_test test/xbox_present_test.cpp)
    target_link_libraries(xbox_present_test PRIVATE gta6rt_d3d12_com user32)
    target_compile_definitions(xbox_present_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(xbox_present_test PRIVATE /W4 /permissive- /utf-8)
    # Needs a Vulkan GPU and a window; exits 77 (skipped) without a Vulkan device.
    add_test(NAME xbox_present COMMAND xbox_present_test)
    # The test pins plane 1's composition on its overlay flag (section 4.5). The runtime's default blends plane 1 over
    # plane 0 whatever its flags (696d00286: the retail title presents both planes with Flags 0);
    # LEONIDA_PRESENT_PLANE1=0 is that rule's switch back.
    set_tests_properties(xbox_present PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_PRESENT_PLANE1=0")

    # The presentation the device proxy makes on the title's first PresentX, driven through the device's frame-event
    # slots and the queue's PresentX slot. The proxy is compiled into this test with LEONIDA_VULKAN_DEVICE, as in
    # gta6rt; the library's copy, built without it, is then not linked.
    add_executable(xs_present_lazy_test test/xs_present_lazy_test.cpp shims/d3d12_xs_device.cpp)
    target_link_libraries(xs_present_lazy_test PRIVATE gta6rt_d3d12_com d3d12 dxgi user32)
    target_compile_definitions(xs_present_lazy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX LEONIDA_VULKAN_DEVICE)
    target_compile_options(xs_present_lazy_test PRIVATE /W4 /permissive- /utf-8)
    # Needs a Vulkan GPU that can present to a window; exits 77 (skipped) otherwise.
    add_test(NAME xs_present_lazy COMMAND xs_present_lazy_test)
    set_tests_properties(xs_present_lazy PROPERTIES SKIP_RETURN_CODE 77)

    # TEST-528: a real boot-movie frame (the package's Bink2Scarlett.dll) uploaded through the Xbox-ABI proxy and
    # presented with PresentX, checked against the decoded BGRA. Package-gated like bink_decode_test: skips (77)
    # unless LEONIDA_GAME_DIR and LEONIDA_MOVIE are set; no game data is written into the repo.
    add_executable(d3d12_boot_movie_test
      test/d3d12_boot_movie_test.cpp
      src/pe_loader.cpp
      shims/registry.cpp
      shims/xmem.cpp
    )
    target_link_libraries(d3d12_boot_movie_test PRIVATE gta6rt_d3d12_com user32 psapi)
    target_compile_definitions(d3d12_boot_movie_test PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(d3d12_boot_movie_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(d3d12_boot_movie_test PRIVATE /OPT:NOREF)
    add_dependencies(d3d12_boot_movie_test gta6rt_tls_host)
    add_test(NAME d3d12_boot_movie COMMAND d3d12_boot_movie_test)
    set_tests_properties(d3d12_boot_movie PROPERTIES SKIP_RETURN_CODE 77)

    # First light through D3D12 (docs/first_light.md): the real model and translated VS/PS that tools/first_light.py
    # wrote to LEONIDA_FIRST_LIGHT_DIR, drawn through the device proxy. Skips (77) unless that variable is set.
    add_executable(d3d12_first_light_test test/d3d12_first_light_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp)
    target_link_libraries(d3d12_first_light_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_first_light_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(d3d12_first_light_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_first_light COMMAND d3d12_first_light_test)
    set_tests_properties(d3d12_first_light PROPERTIES SKIP_RETURN_CODE 77)

    # M1.5 (D3D-53): a draw whose VS and PS are translator output. test/shaders/vs_m15.asm and ps_m15.asm are assembled
    # with llvm-mc and translated by tools/rdna2spirv.py through test/shaders/build.py, which also writes the FSSM
    # programs their index entries are keyed by. Needs llvm-mc with the AMDGPU target (LLVM_MC in the environment, as
    # for the other shader tests, or llvm-mc on PATH or in C:/msys64/ucrt64/bin) and the Vulkan SDK's spirv-as and
    # spirv-val; without them the test is not built.
    if(NOT LEONIDA_LLVM_MC)
      if(DEFINED ENV{LLVM_MC})
        set(LEONIDA_LLVM_MC "$ENV{LLVM_MC}")
      else()
        find_program(LEONIDA_LLVM_MC llvm-mc HINTS "C:/msys64/ucrt64/bin")
      endif()
    endif()
    find_program(LEONIDA_SPIRV_AS spirv-as HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    find_program(LEONIDA_SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    if(LEONIDA_LLVM_MC AND LEONIDA_SPIRV_AS AND LEONIDA_SPIRV_VAL)
      set(LEONIDA_M15_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/m15_shaders)
      get_filename_component(LEONIDA_SPIRV_TOOLS_DIR ${LEONIDA_SPIRV_AS} DIRECTORY)
      file(TO_NATIVE_PATH "${LEONIDA_SPIRV_TOOLS_DIR}" LEONIDA_SPIRV_TOOLS_NATIVE)
      set(LEONIDA_M15_FILES ${LEONIDA_M15_DIR}/spirv/index.json ${LEONIDA_M15_DIR}/fssm/vs_m15.fssm
                            ${LEONIDA_M15_DIR}/fssm/ps_m15.fssm ${LEONIDA_M15_DIR}/fssm/ps_m15_dual.fssm)
      add_custom_command(
        OUTPUT ${LEONIDA_M15_FILES}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${LEONIDA_M15_DIR}
        COMMAND ${CMAKE_COMMAND} -E env "LLVM_MC=${LEONIDA_LLVM_MC}" "LEONIDA_SPIRV_TOOLS=${LEONIDA_SPIRV_TOOLS_NATIVE}"
                $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/build.py
                ${LEONIDA_M15_DIR} vs_m15 ps_m15 ps_m15_dual
        DEPENDS ${LEONIDA_TRANSLATOR_DEPS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/vs_m15.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m15.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m15_dual.asm
        COMMENT "Translating vs_m15, ps_m15 and ps_m15_dual for the translated draw test"
        VERBATIM)
      add_executable(d3d12_translated_draw_test test/d3d12_translated_draw_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp
                     ${LEONIDA_M15_FILES})
      target_link_libraries(d3d12_translated_draw_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_translated_draw_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_M15_DIR="${LEONIDA_M15_DIR}")
      target_compile_options(d3d12_translated_draw_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_translated_draw COMMAND d3d12_translated_draw_test)
      add_test(NAME xbox_native_base_draw COMMAND d3d12_translated_draw_test --native-pso)
      add_test(NAME xbox_derived_mask_zero COMMAND d3d12_translated_draw_test --derived-zero)
      add_test(NAME xbox_derived_mask_restore COMMAND d3d12_translated_draw_test --derived-restore)

      set_tests_properties(d3d12_translated_draw PROPERTIES SKIP_RETURN_CODE 77)
      # Dual-source blending (RT-DUAL-SOURCE-BLEND): with lazy pipelines (the default) and built at creation.
      add_test(NAME d3d12_dual_source_draw COMMAND d3d12_translated_draw_test --dual-source)
      add_test(NAME d3d12_dual_source_draw_eager COMMAND d3d12_translated_draw_test --dual-source)
      set_tests_properties(d3d12_dual_source_draw d3d12_dual_source_draw_eager PROPERTIES SKIP_RETURN_CODE 77)
      set_tests_properties(d3d12_dual_source_draw_eager PROPERTIES ENVIRONMENT "LEONIDA_LAZY_PIPELINES=0")

      # The console's rectangle and quad lists (src/rect_list_geometry.h), through the same translated vs_m15 / ps_m15.
      # The files are d3d12_translated_draw_test's: a second target that lists them runs the translation a second
      # time, at once with the first under a parallel build, and each removes the folder the other is writing.
      add_executable(d3d12_rect_list_test test/d3d12_rect_list_test.cpp shims/d3d12_xs_device.cpp)
      add_dependencies(d3d12_rect_list_test d3d12_translated_draw_test)
      target_link_libraries(d3d12_rect_list_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_rect_list_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_M15_DIR="${LEONIDA_M15_DIR}")
      target_compile_options(d3d12_rect_list_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_rect_list COMMAND d3d12_rect_list_test)
      set_tests_properties(d3d12_rect_list PROPERTIES SKIP_RETURN_CODE 77)

      # Indexed quad lists (src/quad_index_expand.h): a vertex shader that reads the instance id and a pixel shader
      # with a flat colour input, translated like vs_m15 and ps_m15.
      set(LEONIDA_QUAD_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/quad_shaders)
      set(LEONIDA_QUAD_FILES ${LEONIDA_QUAD_DIR}/spirv/index.json ${LEONIDA_QUAD_DIR}/fssm/vs_quad_inst.fssm
                             ${LEONIDA_QUAD_DIR}/fssm/ps_quad_flat.fssm)
      add_custom_command(
        OUTPUT ${LEONIDA_QUAD_FILES}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${LEONIDA_QUAD_DIR}
        COMMAND ${CMAKE_COMMAND} -E env "LLVM_MC=${LEONIDA_LLVM_MC}" "LEONIDA_SPIRV_TOOLS=${LEONIDA_SPIRV_TOOLS_NATIVE}"
                $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/build.py
                ${LEONIDA_QUAD_DIR} vs_quad_inst ps_quad_flat
        DEPENDS ${LEONIDA_TRANSLATOR_DEPS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/vs_quad_inst.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_quad_flat.asm
        COMMENT "Translating vs_quad_inst and ps_quad_flat for the indexed quad list test"
        VERBATIM)
      add_executable(d3d12_quad_index_test test/d3d12_quad_index_test.cpp shims/d3d12_xs_device.cpp
                     ${LEONIDA_QUAD_FILES})
      target_link_libraries(d3d12_quad_index_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_quad_index_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_QUAD_DIR="${LEONIDA_QUAD_DIR}")
      target_compile_options(d3d12_quad_index_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_quad_index COMMAND d3d12_quad_index_test)
      # vs_quad_inst reads its root arguments in the pack() layout (from s0), which is LEONIDA_GRAPHICS_STAGE_LAYOUT=0.
      set_tests_properties(d3d12_quad_index PROPERTIES SKIP_RETURN_CODE 77
                           ENVIRONMENT "LEONIDA_GRAPHICS_STAGE_LAYOUT=0")

      # vk_bindless_test (the bindless set and its table at the Vulkan level, no D3D12) takes the translated vs_ids,
      # ps_tex and ps_imgatomic; it was only run by scripts/run_gpu_tests.py and check.py until D3D-314 needed it.
      set(LEONIDA_BINDLESS_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/bindless_shaders)
      set(LEONIDA_BINDLESS_FILES ${LEONIDA_BINDLESS_DIR}/spirv/vs_ids.spv ${LEONIDA_BINDLESS_DIR}/spirv/ps_tex.spv
                                 ${LEONIDA_BINDLESS_DIR}/spirv/ps_imgatomic.spv)
      add_custom_command(
        OUTPUT ${LEONIDA_BINDLESS_FILES}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${LEONIDA_BINDLESS_DIR}
        COMMAND ${CMAKE_COMMAND} -E env "LLVM_MC=${LEONIDA_LLVM_MC}" "LEONIDA_SPIRV_TOOLS=${LEONIDA_SPIRV_TOOLS_NATIVE}"
                $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/build.py
                ${LEONIDA_BINDLESS_DIR} vs_ids ps_tex ps_imgatomic
        DEPENDS ${LEONIDA_TRANSLATOR_DEPS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/vs_ids.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_tex.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_imgatomic.asm
        COMMENT "Translating vs_ids, ps_tex and ps_imgatomic for vk_bindless_test"
        VERBATIM)
      add_custom_target(vk_bindless_shaders ALL DEPENDS ${LEONIDA_BINDLESS_FILES})
      add_dependencies(vk_bindless_test vk_bindless_shaders)
      add_test(NAME vk_bindless_ps_tex COMMAND vk_bindless_test ${LEONIDA_BINDLESS_DIR}/spirv/vs_ids.spv
               ${LEONIDA_BINDLESS_DIR}/spirv/ps_tex.spv)
      add_test(NAME vk_bindless_ps_imgatomic COMMAND vk_bindless_test ${LEONIDA_BINDLESS_DIR}/spirv/vs_ids.spv
               ${LEONIDA_BINDLESS_DIR}/spirv/ps_imgatomic.spv)
      set_tests_properties(vk_bindless_ps_tex vk_bindless_ps_imgatomic PROPERTIES SKIP_RETURN_CODE 77)

      # M2: a textured quad. vs_m2.asm and ps_m2.asm (the PS samples a T#/S# it loads from two descriptor tables) go
      # through the same build; the test creates the heaps, the SRV, the sampler and the root signature through the
      # D3D12 API only.
      set(LEONIDA_M2_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/m2_shaders)
      set(LEONIDA_M2_FILES ${LEONIDA_M2_DIR}/spirv/index.json ${LEONIDA_M2_DIR}/fssm/vs_m2.fssm
                           ${LEONIDA_M2_DIR}/fssm/ps_m2.fssm ${LEONIDA_M2_DIR}/fssm/ps_m2_uoff.fssm
                           ${LEONIDA_M2_DIR}/fssm/ps_m2_literal.fssm)
      add_custom_command(
        OUTPUT ${LEONIDA_M2_FILES}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${LEONIDA_M2_DIR}
        COMMAND ${CMAKE_COMMAND} -E env "LLVM_MC=${LEONIDA_LLVM_MC}" "LEONIDA_SPIRV_TOOLS=${LEONIDA_SPIRV_TOOLS_NATIVE}"
                $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/build.py
                ${LEONIDA_M2_DIR} vs_m2 ps_m2 ps_m2_uoff ps_m2_literal
        DEPENDS ${LEONIDA_TRANSLATOR_DEPS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/vs_m2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m2_uoff.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m2_literal.asm
        COMMENT "Translating vs_m2 and ps_m2 for the textured draw test"
        VERBATIM)
      add_executable(d3d12_m2_textured_test test/d3d12_m2_textured_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp
                     ${LEONIDA_M2_FILES})
      target_link_libraries(d3d12_m2_textured_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_m2_textured_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_M2_DIR="${LEONIDA_M2_DIR}")
      target_compile_options(d3d12_m2_textured_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_m2_textured COMMAND d3d12_m2_textured_test)
      add_test(NAME xbox_visible_texture_views COMMAND d3d12_m2_textured_test)
      set_tests_properties(xbox_visible_texture_views PROPERTIES ENVIRONMENT "LEONIDA_TEST_VISIBLE_VIEWS=1" SKIP_RETURN_CODE 77 TIMEOUT 25)
      add_test(NAME xbox_staged_texture_views COMMAND d3d12_m2_textured_test)
      set_tests_properties(xbox_staged_texture_views PROPERTIES ENVIRONMENT "LEONIDA_TEST_STAGED_VIEWS=1" SKIP_RETURN_CODE 77 TIMEOUT 25)
      add_test(NAME xbox_placed_texture_first_use COMMAND d3d12_m2_textured_test)
      set_tests_properties(xbox_placed_texture_first_use PROPERTIES ENVIRONMENT "LEONIDA_TEST_PLACED_FIRST_USE=1" SKIP_RETURN_CODE 77 TIMEOUT 25)
      add_test(NAME xbox_raw_2d_stripes COMMAND d3d12_m2_textured_test)
      set_tests_properties(xbox_raw_2d_stripes PROPERTIES ENVIRONMENT "LEONIDA_TEST_LARGE_RAW_TEXTURE=1" SKIP_RETURN_CODE 77 TIMEOUT 35)
      add_test(NAME xbox_raw_2d_surface COMMAND d3d12_m2_textured_test)
      set_tests_properties(xbox_raw_2d_surface PROPERTIES ENVIRONMENT "LEONIDA_TEST_RAW_TEXTURE=1" SKIP_RETURN_CODE 77 TIMEOUT 25)
      add_test(NAME xbox_packed4444_clear_sample COMMAND d3d12_m2_textured_test)
      set_tests_properties(xbox_packed4444_clear_sample PROPERTIES ENVIRONMENT "LEONIDA_TEST_FIXED4=1" SKIP_RETURN_CODE 77)
      add_test(NAME d3d12_m2_sampler_refresh COMMAND d3d12_m2_textured_test --refresh-samplers)
      add_test(NAME d3d12_m2_unbounded_sampler_refresh COMMAND d3d12_m2_textured_test --refresh-unbounded-samplers)
      set_tests_properties(d3d12_m2_sampler_refresh d3d12_m2_unbounded_sampler_refresh PROPERTIES
                           SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_DESC_MIRROR=1")
      add_test(NAME d3d12_m2_sampler_refresh_off COMMAND d3d12_m2_textured_test --refresh-samplers-off)
      set_tests_properties(d3d12_m2_sampler_refresh_off PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_DESC_MIRROR=0")
      set_tests_properties(d3d12_m2_textured PROPERTIES SKIP_RETURN_CODE 77)

      # M2, formats: BC1 and BC7 textures with mip chains and an R8G8B8A8 2D array through the same descriptor path.
      # vs_m2.asm with ps_m2.asm (2D) and ps_m2_array.asm (2D array, slice 1.0 of the view).
      set(LEONIDA_M2F_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/m2f_shaders)
      set(LEONIDA_M2F_FILES ${LEONIDA_M2F_DIR}/spirv/index.json ${LEONIDA_M2F_DIR}/fssm/vs_m2.fssm
                            ${LEONIDA_M2F_DIR}/fssm/ps_m2.fssm ${LEONIDA_M2F_DIR}/fssm/ps_m2_array.fssm)
      add_custom_command(
        OUTPUT ${LEONIDA_M2F_FILES}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${LEONIDA_M2F_DIR}
        COMMAND ${CMAKE_COMMAND} -E env "LLVM_MC=${LEONIDA_LLVM_MC}" "LEONIDA_SPIRV_TOOLS=${LEONIDA_SPIRV_TOOLS_NATIVE}"
                $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/build.py
                ${LEONIDA_M2F_DIR} vs_m2 ps_m2 ps_m2_array
        DEPENDS ${LEONIDA_TRANSLATOR_DEPS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/vs_m2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m2_array.asm
        COMMENT "Translating vs_m2, ps_m2 and ps_m2_array for the M2 formats test"
        VERBATIM)
      add_executable(d3d12_m2_formats_test test/d3d12_m2_formats_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp
                     ${LEONIDA_M2F_FILES})
      target_link_libraries(d3d12_m2_formats_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_m2_formats_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_M2F_DIR="${LEONIDA_M2F_DIR}")
      target_compile_options(d3d12_m2_formats_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_m2_formats COMMAND d3d12_m2_formats_test)
      set_tests_properties(d3d12_m2_formats PROPERTIES SKIP_RETURN_CODE 77)

      # M3 (synthetic part): frames with two render targets and a depth/stencil buffer, an occlusion query, resource
      # barriers and a second pass that samples the first one's target, three frames in flight with a fence each.
      # Pass 1's two-output PS and its single-output occlusion probe go through the same translator build.
      set(LEONIDA_M3_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/m3_shaders)
      set(LEONIDA_M3_FILES ${LEONIDA_M3_DIR}/spirv/index.json ${LEONIDA_M3_DIR}/fssm/vs_m3.fssm
                           ${LEONIDA_M3_DIR}/fssm/ps_m3.fssm ${LEONIDA_M3_DIR}/fssm/vs_m2.fssm
                           ${LEONIDA_M3_DIR}/fssm/ps_m3_post.fssm ${LEONIDA_M3_DIR}/fssm/ps_m3_query.fssm)
      add_custom_command(
        OUTPUT ${LEONIDA_M3_FILES}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${LEONIDA_M3_DIR}
        COMMAND ${CMAKE_COMMAND} -E env "LLVM_MC=${LEONIDA_LLVM_MC}" "LEONIDA_SPIRV_TOOLS=${LEONIDA_SPIRV_TOOLS_NATIVE}"
                $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/build.py
                ${LEONIDA_M3_DIR} vs_m3 ps_m3 vs_m2 ps_m3_post ps_m3_query
        DEPENDS ${LEONIDA_TRANSLATOR_DEPS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/vs_m3.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m3.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/vs_m2.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m3_post.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/ps_m3_query.asm
        COMMENT "Translating M3 frame and single-output occlusion-probe shaders"
        VERBATIM)
      add_executable(d3d12_m3_frame_test test/d3d12_m3_frame_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp ${LEONIDA_M3_FILES})
      target_link_libraries(d3d12_m3_frame_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_m3_frame_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_M3_DIR="${LEONIDA_M3_DIR}")
      target_compile_options(d3d12_m3_frame_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_m3_frame COMMAND d3d12_m3_frame_test)
      add_test(NAME d3d12_m3_first_pass COMMAND d3d12_m3_frame_test --first-pass-only)
      set_tests_properties(d3d12_m3_frame d3d12_m3_first_pass PROPERTIES SKIP_RETURN_CODE 77)

      # M4 (synthetic part): compute PSOs, a UAV table (structured and raw buffers, an R32_UINT storage image), an indirect
      # dispatch from arguments a first dispatch wrote, and a copy queue that streams the input in and reads the results
      # back across queue fences. cs_m4_fill.asm and cs_m4_indirect.asm go through the same build; build.py publishes
      # their synthetic FSSM programs (the compute header and the words their "fssm-data" comments give), which the
      # test wraps in shader containers.
      set(LEONIDA_M4_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/m4_shaders)
      set(LEONIDA_M4_FILES ${LEONIDA_M4_DIR}/spirv/index.json ${LEONIDA_M4_DIR}/fssm/cs_m4_fill.fssm
                           ${LEONIDA_M4_DIR}/fssm/cs_m4_indirect.fssm)
      add_custom_command(
        OUTPUT ${LEONIDA_M4_FILES}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${LEONIDA_M4_DIR}
        # LEONIDA_USER_DATA_SHIFT=4: the fixtures read their user data from s0; a D3D12 compute PSO gets its root
        # arguments from s4 (rt::rootsig::pack_stage, D3D-448), so they are built with every SGPR 4 higher.
        COMMAND ${CMAKE_COMMAND} -E env "LLVM_MC=${LEONIDA_LLVM_MC}" "LEONIDA_SPIRV_TOOLS=${LEONIDA_SPIRV_TOOLS_NATIVE}"
                LEONIDA_USER_DATA_SHIFT=4
                $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/build.py
                ${LEONIDA_M4_DIR} cs_m4_fill cs_m4_indirect
        DEPENDS ${LEONIDA_TRANSLATOR_DEPS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/cs_m4_fill.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/cs_m4_indirect.asm
        COMMENT "Translating cs_m4_fill and cs_m4_indirect for the compute test"
        VERBATIM)
      add_executable(d3d12_m4_compute_test test/d3d12_m4_compute_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp ${LEONIDA_M4_FILES})
      target_link_libraries(d3d12_m4_compute_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_m4_compute_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_M4_DIR="${LEONIDA_M4_DIR}")
      target_compile_options(d3d12_m4_compute_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_m4_compute COMMAND d3d12_m4_compute_test)
      set_tests_properties(d3d12_m4_compute PROPERTIES SKIP_RETURN_CODE 77)
      # LEONIDA_ASYNC_PIPELINES=1 with LEONIDA_ASYNC_MIN_BYTES over both modules and the compile worker held: the
      # dispatches build their pipelines in place, as with asynchronous builds off (src/async_pipeline_size.h).
      add_test(NAME d3d12_m4_compute_async_small_waits COMMAND d3d12_m4_compute_test --async-small-waits)
      set_tests_properties(d3d12_m4_compute_async_small_waits PROPERTIES SKIP_RETURN_CODE 77)

      # Typed buffer UAVs and UAV counters: cs_buf_typed.asm writes typed views through V#s with a FORMAT, and
      # cs_buf_counter.asm appends to and consumes from a structured view whose counter is the second V# of its slot;
      # the test also compares typed-buffer and counter clears with the host runtime on the layer's adapter.
      set(LEONIDA_BUFFER_UAV_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/buffer_uav_shaders)
      set(LEONIDA_BUFFER_UAV_FILES ${LEONIDA_BUFFER_UAV_DIR}/spirv/index.json
                                   ${LEONIDA_BUFFER_UAV_DIR}/fssm/cs_buf_typed.fssm
                                   ${LEONIDA_BUFFER_UAV_DIR}/fssm/cs_buf_counter.fssm)
      add_custom_command(
        OUTPUT ${LEONIDA_BUFFER_UAV_FILES}
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${LEONIDA_BUFFER_UAV_DIR}
        # LEONIDA_USER_DATA_SHIFT=4: root arguments from s4 in a D3D12 compute PSO (see the M4 shaders above).
        COMMAND ${CMAKE_COMMAND} -E env "LLVM_MC=${LEONIDA_LLVM_MC}" "LEONIDA_SPIRV_TOOLS=${LEONIDA_SPIRV_TOOLS_NATIVE}"
                LEONIDA_USER_DATA_SHIFT=4
                $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/build.py
                ${LEONIDA_BUFFER_UAV_DIR} cs_buf_typed cs_buf_counter
        DEPENDS ${LEONIDA_TRANSLATOR_DEPS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/cs_buf_typed.asm
                ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/cs_buf_counter.asm
        COMMENT "Translating cs_buf_typed and cs_buf_counter for the buffer UAV test"
        VERBATIM)
      add_executable(d3d12_buffer_uav_test test/d3d12_buffer_uav_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp ${LEONIDA_BUFFER_UAV_FILES})
      target_link_libraries(d3d12_buffer_uav_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_buffer_uav_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_BUFFER_UAV_DIR="${LEONIDA_BUFFER_UAV_DIR}")
      target_compile_options(d3d12_buffer_uav_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_buffer_uav COMMAND d3d12_buffer_uav_test)
      set_tests_properties(d3d12_buffer_uav PROPERTIES SKIP_RETURN_CODE 77)
    else()
      message(STATUS "llvm-mc (set LLVM_MC) or the SDK's spirv-as/spirv-val not found: d3d12_translated_draw_test, d3d12_m2_textured_test, d3d12_m2_formats_test, d3d12_m3_frame_test, d3d12_m4_compute_test and d3d12_buffer_uav_test are not built")
    endif()
    # ID3D12Resource::GetHeapProperties and ID3D12DescriptorHeap::GetDesc (node masks, resolved heap flags) against the
    # host runtime and WARP; run explicitly on a Vulkan GPU.
    add_executable(d3d12_node_mask_test test/d3d12_node_mask_test.cpp)
    target_link_libraries(d3d12_node_mask_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_node_mask_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_node_mask_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_node_mask COMMAND d3d12_node_mask_test)

    # Console Xbox CreateDescriptorHeap extended ABI (issue #313 comment 7107265058): the title passes address/size
    # after the PC desc and expects GetCPU/GPUDescriptorHandleForHeapStart() == address. Synthetic test; no title-memory
    # aliasing / §3.3 backing coverage.
    add_executable(d3d12_console_descriptor_heap_test test/d3d12_console_descriptor_heap_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp)
    target_link_libraries(d3d12_console_descriptor_heap_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    # LEONIDA_VULKAN_DEVICE: slot 14 creates the console heap on the Vulkan device it links, as in gta6rt.
    target_compile_definitions(d3d12_console_descriptor_heap_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX LEONIDA_VULKAN_DEVICE)
    target_compile_options(d3d12_console_descriptor_heap_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_console_descriptor_heap COMMAND d3d12_console_descriptor_heap_test)
    set_tests_properties(d3d12_console_descriptor_heap PROPERTIES SKIP_RETURN_CODE 77)
    # Buffer descriptors naming memory nothing backs are written zero-size (created views, inline table V#s, console
    # heap copies), and resource heaps are followed by null slots.
    add_executable(d3d12_unmapped_vsharp_test test/d3d12_unmapped_vsharp_test.cpp shims/d3d12_xs_device.cpp)
    target_link_libraries(d3d12_unmapped_vsharp_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_unmapped_vsharp_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX LEONIDA_VULKAN_DEVICE)
    target_compile_options(d3d12_unmapped_vsharp_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_unmapped_vsharp COMMAND d3d12_unmapped_vsharp_test)
    set_tests_properties(d3d12_unmapped_vsharp PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_CONSOLE_FIX_US=0")
    add_executable(d3d12_indirect_zero_test test/d3d12_indirect_zero_test.cpp)
    target_link_libraries(d3d12_indirect_zero_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_indirect_zero_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_indirect_zero_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_indirect_zero COMMAND d3d12_indirect_zero_test)
    set_tests_properties(d3d12_indirect_zero PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(d3d12_node_mask PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(d3d12_indirect_zero PROPERTIES SKIP_RETURN_CODE 77)

    # CopyResource / CopyTextureRegion between BC and its uncompressed equivalent (and depth <-> colour) against the
    # host runtime and WARP; run explicitly on a Vulkan GPU.
    add_executable(d3d12_copy_xfmt_test test/d3d12_copy_xfmt_test.cpp)
    target_link_libraries(d3d12_copy_xfmt_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_copy_xfmt_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_copy_xfmt_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_copy_xfmt COMMAND d3d12_copy_xfmt_test)
    set_tests_properties(d3d12_copy_xfmt PROPERTIES SKIP_RETURN_CODE 77)

    # Copies between an A8_UNORM render target (VK_FORMAT_A8_UNORM_KHR) and an R8_UNORM-backed A8_UNORM texture.
    add_executable(d3d12_a8_copy_test test/d3d12_a8_copy_test.cpp)
    target_link_libraries(d3d12_a8_copy_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_a8_copy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_a8_copy_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_a8_copy COMMAND d3d12_a8_copy_test)
    set_tests_properties(d3d12_a8_copy PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(d3d12_stencil_copy_test test/d3d12_stencil_copy_test.cpp)
    target_link_libraries(d3d12_stencil_copy_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_stencil_copy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_stencil_copy_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_stencil_copy COMMAND d3d12_stencil_copy_test)
    set_tests_properties(d3d12_stencil_copy PROPERTIES SKIP_RETURN_CODE 77
      ENVIRONMENT "LEONIDA_HOST_ADAPTER=warp")

    # Query heaps and query calls by list type (Close results, CreateQueryHeap node masks, timestamp heap kinds)
    # against the host runtime and WARP; run explicitly on a Vulkan GPU.
    add_executable(d3d12_query_rules_test test/d3d12_query_rules_test.cpp)
    target_link_libraries(d3d12_query_rules_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_query_rules_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_query_rules_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_query_rules COMMAND d3d12_query_rules_test)
    set_tests_properties(d3d12_query_rules PROPERTIES SKIP_RETURN_CODE 77)

    # Vulkan-backed IDXGIFactory4 / IDXGISwapChain3 presentation over a Win32 HWND.
    add_executable(dxgi_swapchain_test test/dxgi_swapchain_test.cpp)
    target_link_libraries(dxgi_swapchain_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(dxgi_swapchain_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dxgi_swapchain_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME dxgi_swapchain COMMAND dxgi_swapchain_test)
    set_tests_properties(dxgi_swapchain PROPERTIES SKIP_RETURN_CODE 77)

    # The layer's DXGI against the host runtime's (hardware adapter, or WARP with PROBE_WARP=1): IDXGIAdapter4,
    # GetRestrictToOutput without an output, ResizeBuffers flag bits and null out-pointers.
    add_executable(dxgi_host_parity_test test/dxgi_host_parity_test.cpp)
    target_link_libraries(dxgi_host_parity_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(dxgi_host_parity_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dxgi_host_parity_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME dxgi_host_parity COMMAND dxgi_host_parity_test)
    set_tests_properties(dxgi_host_parity PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

    # Host-parity test for DXGI waitable swap chain frame-latency semaphore behavior.
    add_executable(dxgi_waitable_latency_test test/dxgi_waitable_latency_test.cpp)
    target_link_libraries(dxgi_waitable_latency_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(dxgi_waitable_latency_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dxgi_waitable_latency_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME dxgi_waitable_latency COMMAND dxgi_waitable_latency_test)
    set_tests_properties(dxgi_waitable_latency PROPERTIES
      SKIP_RETURN_CODE 77 TIMEOUT 120
      FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")

    # Synthetic (no GPU): VulkanSwapchain's DXGI BufferCount contract when Vulkan returns more images than the
    # app asked for, or the surface minimum exceeds the request (D3D-389).
    add_executable(dxgi_swapchain_count_test test/dxgi_swapchain_count_test.cpp)
    target_link_libraries(dxgi_swapchain_count_test PRIVATE Vulkan::Headers)
    target_compile_definitions(dxgi_swapchain_count_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dxgi_swapchain_count_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME dxgi_swapchain_count COMMAND dxgi_swapchain_count_test)

    # ExecuteIndirect with CountBuffer (vkCmdDrawIndirectCount) + optional BLAS/TLAS/RayQueryKHR GPU test.
    add_executable(d3d12_indirect_rt_test test/d3d12_indirect_rt_test.cpp)
    target_link_libraries(d3d12_indirect_rt_test PRIVATE gta6rt_d3d12_com)
    target_compile_definitions(d3d12_indirect_rt_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_indirect_rt_test PRIVATE /W4 /permissive- /utf-8)
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      set(LEONIDA_TEST_VS_TRI ${CMAKE_CURRENT_BINARY_DIR}/generated/vs_tri.spv)
      set(LEONIDA_TEST_FS_SOLID ${CMAKE_CURRENT_BINARY_DIR}/generated/fs_solid.spv)
      set(LEONIDA_TEST_CS_RAYQUERY ${CMAKE_CURRENT_BINARY_DIR}/generated/cs_rayquery.spv)
      add_custom_command(
        OUTPUT ${LEONIDA_TEST_VS_TRI}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${LEONIDA_TEST_VS_TRI}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/vs_tri.vert
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/vs_tri.vert
        COMMENT "Compiling vs_tri.vert"
        VERBATIM)
      add_custom_command(
        OUTPUT ${LEONIDA_TEST_FS_SOLID}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${LEONIDA_TEST_FS_SOLID}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/fs_solid.frag
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/fs_solid.frag
        COMMENT "Compiling fs_solid.frag"
        VERBATIM)
      add_custom_command(
        OUTPUT ${LEONIDA_TEST_CS_RAYQUERY}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${LEONIDA_TEST_CS_RAYQUERY}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_rayquery.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_rayquery.comp
        COMMENT "Compiling cs_rayquery.comp"
        VERBATIM)
      target_sources(d3d12_indirect_rt_test PRIVATE
                     ${LEONIDA_TEST_VS_TRI} ${LEONIDA_TEST_FS_SOLID} ${LEONIDA_TEST_CS_RAYQUERY})
      target_compile_definitions(d3d12_indirect_rt_test PRIVATE
        LEONIDA_TEST_VS_TRI_SPV="${LEONIDA_TEST_VS_TRI}"
        LEONIDA_TEST_FS_SOLID_SPV="${LEONIDA_TEST_FS_SOLID}"
        LEONIDA_TEST_CS_RAYQUERY_SPV="${LEONIDA_TEST_CS_RAYQUERY}")
    endif()
    add_test(NAME d3d12_indirect_rt COMMAND d3d12_indirect_rt_test)
    set_tests_properties(d3d12_indirect_rt PROPERTIES SKIP_RETURN_CODE 77)
    # Upload heaps in system memory, as on a GPU without Resizable BAR: their VAs can lie below the arena and wrap.
    add_test(NAME d3d12_indirect_rt_upload_sysmem COMMAND d3d12_indirect_rt_test)
    set_tests_properties(d3d12_indirect_rt_upload_sysmem PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "LEONIDA_UPLOAD_SYSMEM=1")

    # The compute path: root signature, compute PSO from a translator-style index, root arguments, dispatch. Its
    # shader is GLSL written to the translator's push-constant/address contract, compiled here.
    if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      set(LEONIDA_TEST_CS ${CMAKE_CURRENT_BINARY_DIR}/generated/cs_user_data.spv)
      add_custom_command(
        OUTPUT ${LEONIDA_TEST_CS}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${LEONIDA_TEST_CS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_user_data.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_user_data.comp
        COMMENT "Compiling the compute test shader"
        VERBATIM)
      add_executable(d3d12_compute_test test/d3d12_compute_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp ${LEONIDA_TEST_CS})
      target_link_libraries(d3d12_compute_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_compute_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_CS_SPV="${LEONIDA_TEST_CS}")
      target_compile_options(d3d12_compute_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_compute COMMAND d3d12_compute_test)
      set_tests_properties(d3d12_compute PROPERTIES SKIP_RETURN_CODE 77)
      # Device teardown with background pipeline builds queued or running: dropped, waited for, the worker joined.
      add_executable(d3d12_compile_teardown_test test/d3d12_compile_teardown_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp ${LEONIDA_TEST_CS})
      target_link_libraries(d3d12_compile_teardown_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_compile_teardown_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_CS_SPV="${LEONIDA_TEST_CS}")
      target_compile_options(d3d12_compile_teardown_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_compile_teardown COMMAND d3d12_compile_teardown_test)
      set_tests_properties(d3d12_compile_teardown PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
      # The same with one compile worker, the layout before the pool (LEONIDA_COMPILE_THREADS=1).
      add_test(NAME d3d12_compile_teardown_one_worker COMMAND d3d12_compile_teardown_test)
      set_tests_properties(d3d12_compile_teardown_one_worker PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
                           ENVIRONMENT "LEONIDA_COMPILE_THREADS=1")
      # A buffer at the title's own VA (device slot 133) read by the same shader through a root view and a V#.
      add_executable(d3d12_title_va_test test/d3d12_title_va_test.cpp shims/d3d12_xs_device.cpp
                     test/xmem_geometry_aliases_stub.cpp ${LEONIDA_TEST_CS})
      target_link_libraries(d3d12_title_va_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_title_va_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_CS_SPV="${LEONIDA_TEST_CS}")
      target_compile_options(d3d12_title_va_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_title_va COMMAND d3d12_title_va_test)
      add_test(NAME d3d12_title_va_shadow COMMAND d3d12_title_va_test)
      set_tests_properties(d3d12_title_va_shadow PROPERTIES ENVIRONMENT "LEONIDA_TEST_CONSOLE_SHADOW=1;LEONIDA_GEOMETRY_MB=0" SKIP_RETURN_CODE 77)
      add_test(NAME d3d12_geometry_views COMMAND d3d12_title_va_test)
      set_tests_properties(d3d12_geometry_views PROPERTIES ENVIRONMENT "LEONIDA_GEOMETRY_MB=16" SKIP_RETURN_CODE 77)
      set_tests_properties(d3d12_title_va PROPERTIES SKIP_RETURN_CODE 77)
      # Past LEONIDA_TITLE_WINDOWS_MB a view of the title's memory is never its raw VA (#1537): with no other backing it
      # is written zero-size, and a dispatch through it reads zeros.
      add_test(NAME d3d12_title_window_cap COMMAND d3d12_title_va_test --window-cap)
      set_tests_properties(d3d12_title_window_cap PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_TITLE_WINDOWS_MB=0")
      # A window is a read-side mirror refreshed before every submission: what a shader writes into it goes back to
      # the title's pages at the next refresh, and the title's own newer words still win (#1537, WRITE_INVALID).
      add_test(NAME d3d12_title_window_write_back COMMAND d3d12_title_va_test --window-writeback)
      set_tests_properties(d3d12_title_window_write_back PROPERTIES SKIP_RETURN_CODE 77)
      # No window nothing reaches: a stream-pool request just before an extent stops where it starts, and a window a
      # slot-133 buffer replaced is retired at the next submission, its bytes returned and its memory freed.
      add_test(NAME d3d12_title_window_orphans COMMAND d3d12_title_va_test --window-orphans)
      set_tests_properties(d3d12_title_window_orphans PROPERTIES SKIP_RETURN_CODE 77)
      # The same with no per-submission copy back: a retired window's GPU writes reach the title's pages only when it
      # is freed, on the bytes no live copy changed (a buffer over the same 256-byte piece wrote the others).
      add_test(NAME d3d12_title_window_orphans_free COMMAND d3d12_title_va_test --window-orphans)
      set_tests_properties(d3d12_title_window_orphans_free PROPERTIES SKIP_RETURN_CODE 77
                           ENVIRONMENT "LEONIDA_TITLE_WRITEBACK=0")
      # A write-back copy that faults on a page another guard (a geometry watch cell) keeps read-only: the fault
      # handler asks the tracker whose copy faulted too, which must not take its lock again on that thread (#2418).
      add_test(NAME d3d12_title_window_write_back_fault COMMAND d3d12_title_va_test --window-writeback-fault)
      set_tests_properties(d3d12_title_window_write_back_fault PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
      # The same fault with a later run of the write-back in the tile it reopened: that run's GPU words go back too.
      add_test(NAME d3d12_title_window_write_back_fault_tile COMMAND d3d12_title_va_test --window-writeback-fault-tile)
      set_tests_properties(d3d12_title_window_write_back_fault_tile PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
      # The same return copy for a slot-133 buffer's shadow (the write-back of a compute result the title reads again).
      add_test(NAME d3d12_placed_write_back COMMAND d3d12_title_va_test --placed-writeback)
      set_tests_properties(d3d12_placed_write_back PROPERTIES SKIP_RETURN_CODE 77)
      # Two shadows over one page share their sums: a newly filled one must still follow the title's bytes, with
      # LEONIDA_SHADOW_KEEP on and off (review by lixonik).
      add_test(NAME d3d12_shadow_keep_refill COMMAND d3d12_title_va_test --shadow-keep-refill)
      set_tests_properties(d3d12_shadow_keep_refill PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_SHADOW_KEEP=1")
      add_test(NAME d3d12_shadow_keep_refill_off COMMAND d3d12_title_va_test --shadow-keep-refill)
      set_tests_properties(d3d12_shadow_keep_refill_off PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_SHADOW_KEEP=0")
      add_test(NAME d3d12_api_descriptor_copy COMMAND d3d12_title_va_test --api-descriptor-copy)
      set_tests_properties(d3d12_api_descriptor_copy PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_DESC_MIRROR=1")
      add_test(NAME d3d12_api_descriptor_copy_off COMMAND d3d12_title_va_test --api-descriptor-copy-off)
      set_tests_properties(d3d12_api_descriptor_copy_off PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_DESC_MIRROR=0")
      add_test(NAME d3d12_rebound_tables COMMAND d3d12_title_va_test --rebound-tables)
      set_tests_properties(d3d12_rebound_tables PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_DESC_MIRROR=1")
      add_test(NAME d3d12_rebound_tables_off COMMAND d3d12_title_va_test --rebound-tables-off)
      set_tests_properties(d3d12_rebound_tables_off PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_DESC_MIRROR=0")
      add_test(NAME d3d12_rebound_second_device COMMAND d3d12_title_va_test --rebound-second-device)
      set_tests_properties(d3d12_rebound_second_device PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_DESC_MIRROR=1")
      # Compute PSOs from FSPX programs (docs/fspx.md): an ACCC container at slot 62 and a bare kind-6 program.
      add_executable(d3d12_fspx_compute_test test/d3d12_fspx_compute_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp ${LEONIDA_TEST_CS})
      target_link_libraries(d3d12_fspx_compute_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_fspx_compute_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_CS_SPV="${LEONIDA_TEST_CS}")
      target_compile_options(d3d12_fspx_compute_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_fspx_compute COMMAND d3d12_fspx_compute_test)
      set_tests_properties(d3d12_fspx_compute PROPERTIES SKIP_RETURN_CODE 77)
      # Milestone M1: a VS+PS pipeline and indexed draws, shaders written to the same contract.
      set(LEONIDA_TEST_VS ${CMAKE_CURRENT_BINARY_DIR}/generated/vs_draw.spv)
      set(LEONIDA_TEST_PS ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_draw.spv)
      set(LEONIDA_TEST_PS_UINT ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_draw_uint.spv)
      set(LEONIDA_TEST_VS_FULLSCREEN ${CMAKE_CURRENT_BINARY_DIR}/generated/vs_fullscreen.spv)
      set(LEONIDA_TEST_VS_INT ${CMAKE_CURRENT_BINARY_DIR}/generated/vs_draw_int.spv)
      set(LEONIDA_TEST_GS_DOUBLE ${CMAKE_CURRENT_BINARY_DIR}/generated/gs_double.spv)
      foreach(pair "vs_draw.vert;${LEONIDA_TEST_VS}" "ps_draw.frag;${LEONIDA_TEST_PS}"
                   "ps_draw_uint.frag;${LEONIDA_TEST_PS_UINT}" "vs_fullscreen.vert;${LEONIDA_TEST_VS_FULLSCREEN}" "vs_draw_int.vert;${LEONIDA_TEST_VS_INT}"
                   "gs_double.geom;${LEONIDA_TEST_GS_DOUBLE}")
        list(GET pair 0 source)
        list(GET pair 1 output)
        add_custom_command(
          OUTPUT ${output}
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${output}
                  ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          COMMENT "Compiling the draw test shader ${source}"
          VERBATIM)
      endforeach()
      add_executable(d3d12_draw_test test/d3d12_draw_test.cpp test/d3d12_stateful_indirect_pixels.cpp
                     shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp ${LEONIDA_TEST_VS}
                     ${LEONIDA_TEST_PS} ${LEONIDA_TEST_PS_UINT} ${LEONIDA_TEST_VS_FULLSCREEN} ${LEONIDA_TEST_VS_INT})
      target_link_libraries(d3d12_draw_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_draw_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_VS_SPV="${LEONIDA_TEST_VS}" LEONIDA_TEST_PS_SPV="${LEONIDA_TEST_PS}"
                                 LEONIDA_TEST_PS_UINT_SPV="${LEONIDA_TEST_PS_UINT}"
                                 LEONIDA_TEST_VS_FULLSCREEN_SPV="${LEONIDA_TEST_VS_FULLSCREEN}"
                                 LEONIDA_TEST_VS_INT_SPV="${LEONIDA_TEST_VS_INT}")
      target_compile_options(d3d12_draw_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_draw COMMAND d3d12_draw_test)
      add_test(NAME xbox_geometry_upload_batch COMMAND d3d12_draw_test)
      set_tests_properties(xbox_geometry_upload_batch PROPERTIES ENVIRONMENT "LEONIDA_TEST_GEOMETRY_BATCH=1" SKIP_RETURN_CODE 77 TIMEOUT 40)
      add_test(NAME d3d12_stateful_indirect_pixels COMMAND d3d12_draw_test --stateful-indirect-pixels)
      add_test(NAME d3d12_stateful_indirect_multi_pixels COMMAND d3d12_draw_test --stateful-indirect-multi-pixels)
      add_test(NAME d3d12_stateful_indirect_counted_pixels COMMAND d3d12_draw_test --stateful-indirect-counted-pixels)
      # A device without robust indexed views can still validate each ordinary DRAW pixel oracle.
      add_test(NAME d3d12_stateful_indirect_nonindexed_pixels COMMAND d3d12_draw_test --stateful-indirect-pixels --nonindexed-only)
      add_test(NAME d3d12_stateful_indirect_nonindexed_multi_pixels COMMAND d3d12_draw_test --stateful-indirect-multi-pixels --nonindexed-only)
      add_test(NAME d3d12_stateful_indirect_nonindexed_counted_pixels COMMAND d3d12_draw_test --stateful-indirect-counted-pixels --nonindexed-only)
      set_tests_properties(d3d12_stateful_indirect_pixels d3d12_stateful_indirect_multi_pixels d3d12_stateful_indirect_counted_pixels
                           d3d12_stateful_indirect_nonindexed_pixels d3d12_stateful_indirect_nonindexed_multi_pixels
                           d3d12_stateful_indirect_nonindexed_counted_pixels PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
      add_test(NAME d3d12_title_index_window COMMAND d3d12_draw_test --title-index-window)
      add_test(NAME d3d12_title_vertex_window COMMAND d3d12_draw_test --title-vertex-window)
      # LEONIDA_STUB_PIPELINES: a pipeline with no translated shader is a stub when the switch is on.
      add_executable(d3d12_stub_pipelines_test test/d3d12_stub_pipelines_test.cpp)
      target_link_libraries(d3d12_stub_pipelines_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_stub_pipelines_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(d3d12_stub_pipelines_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_stub_pipelines COMMAND d3d12_stub_pipelines_test)
      set_tests_properties(d3d12_stub_pipelines PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
      # MVP-GPU-04: a stub graphics pipeline is real (src/stub_pipeline.h); draws with =1 and =color, read back.
      add_executable(d3d12_stub_draw_test test/d3d12_stub_draw_test.cpp shims/d3d12_xs_device.cpp
                     shims/d3d12_xs_pso_stream.cpp)
      target_link_libraries(d3d12_stub_draw_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_stub_draw_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(d3d12_stub_draw_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_stub_draw COMMAND d3d12_stub_draw_test)
      set_tests_properties(d3d12_stub_draw PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
      set_tests_properties(d3d12_draw d3d12_title_index_window d3d12_title_vertex_window PROPERTIES SKIP_RETURN_CODE 77)
      # Graphics & compute PSO descriptor validation parity against live Windows D3D12 (WARP and HW).
      add_executable(d3d12_pso_validation_test test/d3d12_pso_validation_test.cpp ${LEONIDA_TEST_VS}
                     ${LEONIDA_TEST_PS} ${LEONIDA_TEST_PS_UINT} ${LEONIDA_TEST_VS_FULLSCREEN} ${LEONIDA_TEST_CS})
      target_link_libraries(d3d12_pso_validation_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
      target_compile_definitions(d3d12_pso_validation_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_VS_SPV="${LEONIDA_TEST_VS}" LEONIDA_TEST_PS_SPV="${LEONIDA_TEST_PS}"
                                 LEONIDA_TEST_PS_UINT_SPV="${LEONIDA_TEST_PS_UINT}"
                                 LEONIDA_TEST_VS_FULLSCREEN_SPV="${LEONIDA_TEST_VS_FULLSCREEN}"
                                 LEONIDA_TEST_CS_SPV="${LEONIDA_TEST_CS}")
      target_compile_options(d3d12_pso_validation_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_pso_validation COMMAND d3d12_pso_validation_test)
      # Runs with the compute warm-up thread on: it used to outlive the device this test releases (the process died
      # after its PASS line); a released device now drops or waits for its builds and joins
      # the thread (d3d12_compile_teardown_test).
      set_tests_properties(d3d12_pso_validation PROPERTIES SKIP_RETURN_CODE 77
                           FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
      # Conservative raster Tier 1, with geometric coverage bounds and host D3D12 references.
      set(LEONIDA_CONSERVATIVE_VS ${CMAKE_CURRENT_BINARY_DIR}/generated/vs_conservative.spv)
      set(LEONIDA_CONSERVATIVE_PS ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_conservative.spv)
      foreach(pair "vs_conservative.vert;${LEONIDA_CONSERVATIVE_VS}" "ps_conservative.frag;${LEONIDA_CONSERVATIVE_PS}")
        list(GET pair 0 source)
        list(GET pair 1 output)
        add_custom_command(OUTPUT ${output}
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${output}
                  ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source} VERBATIM)
      endforeach()
      add_executable(d3d12_conservative_raster_test test/d3d12_conservative_raster_test.cpp
        ${LEONIDA_CONSERVATIVE_VS} ${LEONIDA_CONSERVATIVE_PS})
      target_link_libraries(d3d12_conservative_raster_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
      target_compile_definitions(d3d12_conservative_raster_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        LEONIDA_CONSERVATIVE_VS="${LEONIDA_CONSERVATIVE_VS}" LEONIDA_CONSERVATIVE_PS="${LEONIDA_CONSERVATIVE_PS}")
      target_compile_options(d3d12_conservative_raster_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_conservative_raster COMMAND d3d12_conservative_raster_test)
      set_tests_properties(d3d12_conservative_raster PROPERTIES SKIP_RETURN_CODE 77
        ENVIRONMENT "LEONIDA_HOST_ADAPTER=warp")
      set(LEONIDA_CONSERVATIVE_TIER2_VS ${CMAKE_CURRENT_BINARY_DIR}/generated/vs_conservative_tier2.spv)
      set(LEONIDA_CONSERVATIVE_TIER2_PS ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_conservative_tier2.spv)
      foreach(pair "vs_conservative_tier2.vert;${LEONIDA_CONSERVATIVE_TIER2_VS}" "ps_conservative_tier2.frag;${LEONIDA_CONSERVATIVE_TIER2_PS}")
        list(GET pair 0 source)
        list(GET pair 1 output)
        add_custom_command(OUTPUT ${output}
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${output}
                  ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source} VERBATIM)
      endforeach()
      add_executable(d3d12_conservative_tier2_test test/d3d12_conservative_tier2_test.cpp
        ${LEONIDA_CONSERVATIVE_TIER2_VS} ${LEONIDA_CONSERVATIVE_TIER2_PS})
      target_link_libraries(d3d12_conservative_tier2_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
      target_compile_definitions(d3d12_conservative_tier2_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        LEONIDA_CONSERVATIVE_TIER2_VS="${LEONIDA_CONSERVATIVE_TIER2_VS}" LEONIDA_CONSERVATIVE_TIER2_PS="${LEONIDA_CONSERVATIVE_TIER2_PS}")
      target_compile_options(d3d12_conservative_tier2_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_conservative_tier2 COMMAND d3d12_conservative_tier2_test)
      set_tests_properties(d3d12_conservative_tier2 PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_HOST_ADAPTER=warp")
      set(LEONIDA_CONSERVATIVE_TIER3_PS ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_conservative_tier3.spv)
      add_custom_command(OUTPUT ${LEONIDA_CONSERVATIVE_TIER3_PS}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${LEONIDA_CONSERVATIVE_TIER3_PS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/ps_conservative_tier3.frag
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/ps_conservative_tier3.frag VERBATIM)
      add_executable(d3d12_conservative_tier3_test test/d3d12_conservative_tier3_test.cpp
        ${LEONIDA_CONSERVATIVE_VS} ${LEONIDA_CONSERVATIVE_TIER3_PS})
      target_link_libraries(d3d12_conservative_tier3_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
      target_compile_definitions(d3d12_conservative_tier3_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        LEONIDA_CONSERVATIVE_VS="${LEONIDA_CONSERVATIVE_VS}"
        LEONIDA_CONSERVATIVE_TIER3_PS="${LEONIDA_CONSERVATIVE_TIER3_PS}")
      target_compile_options(d3d12_conservative_tier3_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME inner_coverage_oracle COMMAND d3d12_conservative_tier3_test --oracle-only)
      add_test(NAME d3d12_inner_coverage_guards COMMAND d3d12_conservative_tier3_test --guards-only)
      add_test(NAME d3d12_inner_coverage_native COMMAND d3d12_conservative_tier3_test --native-only)
      add_test(NAME d3d12_conservative_tier3 COMMAND d3d12_conservative_tier3_test)
      set_tests_properties(d3d12_inner_coverage_guards d3d12_inner_coverage_native d3d12_conservative_tier3
        PROPERTIES SKIP_RETURN_CODE 77)
      add_executable(d3d12_msaa_draw_test test/d3d12_msaa_draw_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp
                     ${LEONIDA_TEST_VS} ${LEONIDA_TEST_PS})
      target_link_libraries(d3d12_msaa_draw_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_msaa_draw_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_VS_SPV="${LEONIDA_TEST_VS}" LEONIDA_TEST_PS_SPV="${LEONIDA_TEST_PS}")
      target_compile_options(d3d12_msaa_draw_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_msaa_draw COMMAND d3d12_msaa_draw_test)
      set_tests_properties(d3d12_msaa_draw PROPERTIES SKIP_RETURN_CODE 77)
      # Geometry shaders: a VS+GS+PS pipeline whose GS emits each triangle twice.
      add_executable(d3d12_geometry_test test/d3d12_geometry_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp
                     ${LEONIDA_TEST_VS} ${LEONIDA_TEST_GS_DOUBLE} ${LEONIDA_TEST_PS})
      target_link_libraries(d3d12_geometry_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_geometry_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_VS_SPV="${LEONIDA_TEST_VS}" LEONIDA_TEST_GS_SPV="${LEONIDA_TEST_GS_DOUBLE}"
                                 LEONIDA_TEST_PS_SPV="${LEONIDA_TEST_PS}")
      target_compile_options(d3d12_geometry_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_geometry COMMAND d3d12_geometry_test)
      set_tests_properties(d3d12_geometry PROPERTIES SKIP_RETURN_CODE 77)
      # Directly indexed heaps for draws: the VS, PS and mesh stage read the heap dwords after the explicit root
      # arguments. The mesh shader needs Vulkan 1.2 (SPIR-V 1.5), as the translator targets: 1.3 emits LocalSizeId.
      set(LEONIDA_TEST_VS_USER_DATA_WINDOW ${CMAKE_CURRENT_BINARY_DIR}/generated/vs_user_data_window.spv)
      set(LEONIDA_TEST_PS_USER_DATA_WINDOW ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_user_data_window.spv)
      set(LEONIDA_TEST_PS_INDIRECT_HEAP ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_indirect_heap_resource.spv)
      set(LEONIDA_TEST_MS_USER_DATA_WINDOW ${CMAKE_CURRENT_BINARY_DIR}/generated/ms_user_data_window.spv)
      set(LEONIDA_TEST_PS_MESH_USER_DATA_WINDOW ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_mesh_user_data_window.spv)
      foreach(pair "ps_indirect_heap_resource.frag;${LEONIDA_TEST_PS_INDIRECT_HEAP};vulkan1.3"
                   "vs_user_data_window.vert;${LEONIDA_TEST_VS_USER_DATA_WINDOW};vulkan1.3"
                   "ps_user_data_window.frag;${LEONIDA_TEST_PS_USER_DATA_WINDOW};vulkan1.3"
                   "ms_user_data_window.mesh;${LEONIDA_TEST_MS_USER_DATA_WINDOW};vulkan1.2"
                   "ps_mesh_user_data_window.frag;${LEONIDA_TEST_PS_MESH_USER_DATA_WINDOW};vulkan1.3")
        list(GET pair 0 source)
        list(GET pair 1 output)
        list(GET pair 2 target_env)
        add_custom_command(
          OUTPUT ${output}
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env ${target_env} -o ${output}
                  ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          COMMENT "Compiling the direct heaps test shader ${source}"
          VERBATIM)
      endforeach()
      add_executable(d3d12_graphics_direct_heaps_test test/d3d12_graphics_direct_heaps_test.cpp
                     shims/d3d12_xs_device.cpp ${LEONIDA_TEST_VS_USER_DATA_WINDOW} ${LEONIDA_TEST_PS_USER_DATA_WINDOW}
                     ${LEONIDA_TEST_MS_USER_DATA_WINDOW} ${LEONIDA_TEST_PS_MESH_USER_DATA_WINDOW}
                     ${LEONIDA_TEST_PS_INDIRECT_HEAP})
      target_link_libraries(d3d12_graphics_direct_heaps_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_graphics_direct_heaps_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_VS_SPV="${LEONIDA_TEST_VS_USER_DATA_WINDOW}"
                                 LEONIDA_TEST_PS_SPV="${LEONIDA_TEST_PS_USER_DATA_WINDOW}"
                                 LEONIDA_TEST_MS_SPV="${LEONIDA_TEST_MS_USER_DATA_WINDOW}"
                                 LEONIDA_TEST_PS_MESH_SPV="${LEONIDA_TEST_PS_MESH_USER_DATA_WINDOW}"
                                 LEONIDA_TEST_PS_INDIRECT_HEAP="${LEONIDA_TEST_PS_INDIRECT_HEAP}")
      target_compile_options(d3d12_graphics_direct_heaps_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_graphics_direct_heaps COMMAND d3d12_graphics_direct_heaps_test)
      add_test(NAME d3d12_graphics_indirect_heap_resources COMMAND d3d12_graphics_direct_heaps_test --indirect-resources)
      set_tests_properties(d3d12_graphics_indirect_heap_resources PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
      add_test(NAME d3d12_graphics_direct_heaps_off COMMAND d3d12_graphics_direct_heaps_test --off)
      set_tests_properties(d3d12_graphics_direct_heaps d3d12_graphics_direct_heaps_off PROPERTIES SKIP_RETURN_CODE 77)
      set_tests_properties(d3d12_graphics_direct_heaps_off PROPERTIES ENVIRONMENT LEONIDA_GRAPHICS_DIRECT_HEAPS=0)
      # D3D-309: D24 depth bias and 24-bit depth in draws, with and without a pixel shader and with a geometry shader
      # (src/depth24_fragment.h).
      set(LEONIDA_TEST_VS_DEPTH ${CMAKE_CURRENT_BINARY_DIR}/generated/vs_depth.spv)
      set(LEONIDA_TEST_PS_DEPTH_EMPTY ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_depth_empty.spv)
      set(LEONIDA_TEST_PS_DEPTH_WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_depth_write.spv)
      set(LEONIDA_TEST_GS_DEPTH ${CMAKE_CURRENT_BINARY_DIR}/generated/gs_depth.spv)
      foreach(pair "vs_depth.vert;${LEONIDA_TEST_VS_DEPTH}" "ps_depth_empty.frag;${LEONIDA_TEST_PS_DEPTH_EMPTY}"
                   "ps_depth_write.frag;${LEONIDA_TEST_PS_DEPTH_WRITE}" "gs_depth.geom;${LEONIDA_TEST_GS_DEPTH}")
        list(GET pair 0 source)
        list(GET pair 1 output)
        add_custom_command(
          OUTPUT ${output}
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${output}
                  ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          COMMENT "Compiling the D24 draw test shader ${source}"
          VERBATIM)
      endforeach()
      add_executable(d3d12_depth24_draw_test test/d3d12_depth24_draw_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp
                     ${LEONIDA_TEST_VS_DEPTH} ${LEONIDA_TEST_PS_DEPTH_EMPTY} ${LEONIDA_TEST_PS_DEPTH_WRITE}
                     ${LEONIDA_TEST_GS_DEPTH})
      target_link_libraries(d3d12_depth24_draw_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_depth24_draw_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_VS_DEPTH_SPV="${LEONIDA_TEST_VS_DEPTH}"
                                 LEONIDA_TEST_PS_DEPTH_EMPTY_SPV="${LEONIDA_TEST_PS_DEPTH_EMPTY}"
                                 LEONIDA_TEST_PS_DEPTH_WRITE_SPV="${LEONIDA_TEST_PS_DEPTH_WRITE}"
                                 LEONIDA_TEST_GS_DEPTH_SPV="${LEONIDA_TEST_GS_DEPTH}")
      target_compile_options(d3d12_depth24_draw_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_depth24_draw COMMAND d3d12_depth24_draw_test)
      set_tests_properties(d3d12_depth24_draw PROPERTIES SKIP_RETURN_CODE 77)
      # Set 0 from the compute bind point: a compute PSO whose shader declares set-0 bindings samples through the
      # device's bindless set, which nothing but the layer binds.
      set(LEONIDA_TEST_CS_SET0_SAMPLE ${CMAKE_CURRENT_BINARY_DIR}/generated/cs_set0_sample.spv)
      add_custom_command(
        OUTPUT ${LEONIDA_TEST_CS_SET0_SAMPLE}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${LEONIDA_TEST_CS_SET0_SAMPLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_set0_sample.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_set0_sample.comp
        COMMENT "Compiling the set-0 compute test shader"
        VERBATIM)
      add_executable(d3d12_set0_compute_test test/d3d12_set0_compute_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp
                     ${LEONIDA_TEST_CS_SET0_SAMPLE})
      target_link_libraries(d3d12_set0_compute_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_set0_compute_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_CS_SET0_SAMPLE_SPV="${LEONIDA_TEST_CS_SET0_SAMPLE}")
      target_compile_options(d3d12_set0_compute_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_set0_compute COMMAND d3d12_set0_compute_test)
      set_tests_properties(d3d12_set0_compute PROPERTIES SKIP_RETURN_CODE 77)
      set(depth_uav_shader ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/cs_depth_stencil_uav.comp)
      foreach(variant IN ITEMS DEPTH_STENCIL_2D DEPTH_STENCIL_ARRAY DEPTH16_2D DEPTH16_ARRAY)
        set(spv ${CMAKE_CURRENT_BINARY_DIR}/generated/cs_depth_uav_${variant}.spv)
        add_custom_command(OUTPUT ${spv}
          COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -D${variant}=1
                  -o ${spv} ${depth_uav_shader}
          DEPENDS ${depth_uav_shader} VERBATIM)
        list(APPEND depth_uav_shaders ${spv})
        list(APPEND depth_uav_definitions LEONIDA_${variant}_SPV="${spv}")
      endforeach()
      add_executable(d3d12_depth_stencil_uav_test test/d3d12_depth_stencil_uav_test.cpp
                     shims/d3d12_xs_device.cpp ${depth_uav_shaders})
      target_link_libraries(d3d12_depth_stencil_uav_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
      target_compile_definitions(d3d12_depth_stencil_uav_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 ${depth_uav_definitions})
      target_compile_options(d3d12_depth_stencil_uav_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_depth_stencil_uav COMMAND d3d12_depth_stencil_uav_test)
      set_tests_properties(d3d12_depth_stencil_uav PROPERTIES SKIP_RETURN_CODE 77)
      # Bindless table churn on a direct and a compute queue: the table grows and live keys move without a
      # device-idle wait (D3D-314); the same shader, run twice (deferred retirement, and the old wait for timing).
      add_executable(d3d12_bindless_churn_test test/d3d12_bindless_churn_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp
                     ${LEONIDA_TEST_CS_SET0_SAMPLE})
      target_link_libraries(d3d12_bindless_churn_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_bindless_churn_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_CS_SET0_SAMPLE_SPV="${LEONIDA_TEST_CS_SET0_SAMPLE}")
      target_compile_options(d3d12_bindless_churn_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_bindless_churn COMMAND d3d12_bindless_churn_test)
      set_tests_properties(d3d12_bindless_churn PROPERTIES SKIP_RETURN_CODE 77)
      add_test(NAME d3d12_bindless_churn_validated COMMAND d3d12_bindless_churn_test --validated)
      set_tests_properties(d3d12_bindless_churn_validated PROPERTIES SKIP_RETURN_CODE 77)
      add_executable(d3d12_pipeline_library_test test/d3d12_pipeline_library_test.cpp
                     ${LEONIDA_TEST_CS} ${LEONIDA_TEST_VS} ${LEONIDA_TEST_PS})
      target_link_libraries(d3d12_pipeline_library_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_pipeline_library_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_CS_SPV="${LEONIDA_TEST_CS}"
                                 LEONIDA_TEST_VS_SPV="${LEONIDA_TEST_VS}"
                                 LEONIDA_TEST_PS_SPV="${LEONIDA_TEST_PS}")
      target_compile_options(d3d12_pipeline_library_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_pipeline_library COMMAND d3d12_pipeline_library_test)
      set_tests_properties(d3d12_pipeline_library PROPERTIES SKIP_RETURN_CODE 77)
      # ExecuteBundle: what a bundle's draws inherit and hand back, against the host runtime and WARP.
      add_executable(d3d12_bundle_test test/d3d12_bundle_test.cpp ${LEONIDA_TEST_VS} ${LEONIDA_TEST_PS})
      target_link_libraries(d3d12_bundle_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
      target_compile_definitions(d3d12_bundle_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
                                 LEONIDA_TEST_VS_SPV="${LEONIDA_TEST_VS}" LEONIDA_TEST_PS_SPV="${LEONIDA_TEST_PS}")
      target_compile_options(d3d12_bundle_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_bundle COMMAND d3d12_bundle_test)
      add_test(NAME d3d12_bundle_indirect_warp COMMAND d3d12_bundle_test --indirect-bundle-warp)
      add_test(NAME d3d12_bundle_indirect_layer COMMAND d3d12_bundle_test --indirect-bundle-layer)
      add_test(NAME d3d12_bundle_indirect_off COMMAND d3d12_bundle_test --indirect-bundle-off)
      set_tests_properties(d3d12_bundle_indirect_warp d3d12_bundle_indirect_layer d3d12_bundle_indirect_off
                           PROPERTIES SKIP_RETURN_CODE 77)
      set_tests_properties(d3d12_bundle PROPERTIES SKIP_RETURN_CODE 77)
      add_test(NAME d3d12_root_layout_compact COMMAND d3d12_bundle_test --compact)
      add_test(NAME d3d12_root_layout_legacy COMMAND d3d12_bundle_test --legacy)
      set_tests_properties(d3d12_root_layout_compact PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_GRAPHICS_STAGE_LAYOUT=1")
      set_tests_properties(d3d12_root_layout_legacy PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LEONIDA_GRAPHICS_STAGE_LAYOUT=0")
      # Calls a bundle may not make, Close of the bundle and of its parent, against the host runtime and WARP.
      add_executable(d3d12_bundle_forbidden_test test/d3d12_bundle_forbidden_test.cpp)
      target_link_libraries(d3d12_bundle_forbidden_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
      target_compile_definitions(d3d12_bundle_forbidden_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(d3d12_bundle_forbidden_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_bundle_forbidden COMMAND d3d12_bundle_forbidden_test)
      set_tests_properties(d3d12_bundle_forbidden PROPERTIES SKIP_RETURN_CODE 77)
      # A pixel shader with a writable storage image (an image atomic) needs fragmentStoresAndAtomics: refused on a
      # device without it, created on the runtime's own device, under the validation layer where it is installed.
      set(LEONIDA_TEST_PS_ATOMIC ${CMAKE_CURRENT_BINARY_DIR}/generated/ps_image_atomic.spv)
      add_custom_command(
        OUTPUT ${LEONIDA_TEST_PS_ATOMIC}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${LEONIDA_TEST_PS_ATOMIC}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/ps_image_atomic.frag
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/ps_image_atomic.frag
        COMMENT "Compiling the stores-and-atomics test shader ps_image_atomic.frag"
        VERBATIM)
      add_executable(d3d12_stores_atomics_test test/d3d12_stores_atomics_test.cpp ${LEONIDA_TEST_VS_FULLSCREEN}
                     ${LEONIDA_TEST_PS} ${LEONIDA_TEST_PS_ATOMIC})
      target_link_libraries(d3d12_stores_atomics_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_stores_atomics_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_VS_FULLSCREEN_SPV="${LEONIDA_TEST_VS_FULLSCREEN}"
                                 LEONIDA_TEST_PS_SPV="${LEONIDA_TEST_PS}"
                                 LEONIDA_TEST_PS_ATOMIC_SPV="${LEONIDA_TEST_PS_ATOMIC}")
      target_compile_options(d3d12_stores_atomics_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_stores_atomics COMMAND d3d12_stores_atomics_test)
      set_tests_properties(d3d12_stores_atomics PROPERTIES SKIP_RETURN_CODE 77)

      # D3D-29: mesh-shader PSOs from pipeline state streams and DispatchMesh (skips without VK_EXT_mesh_shader).
      set(LEONIDA_TEST_MS ${CMAKE_CURRENT_BINARY_DIR}/generated/ms_draw.spv)
      add_custom_command(
        OUTPUT ${LEONIDA_TEST_MS}
        # Vulkan 1.2 (SPIR-V 1.5), the translator's target: 1.3 would emit LocalSizeId, which needs maintenance4.
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.2 -o ${LEONIDA_TEST_MS}
                ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/ms_draw.mesh
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/ms_draw.mesh
        COMMENT "Compiling the mesh test shader ms_draw.mesh"
        VERBATIM)
      add_executable(d3d12_mesh_test test/d3d12_mesh_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp ${LEONIDA_TEST_MS}
                     ${LEONIDA_TEST_PS} ${LEONIDA_TEST_VS_FULLSCREEN})
      target_link_libraries(d3d12_mesh_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_mesh_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_MS_SPV="${LEONIDA_TEST_MS}" LEONIDA_TEST_PS_SPV="${LEONIDA_TEST_PS}"
                                 LEONIDA_TEST_VS_FULLSCREEN_SPV="${LEONIDA_TEST_VS_FULLSCREEN}")
      target_compile_options(d3d12_mesh_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_mesh COMMAND d3d12_mesh_test)
      set_tests_properties(d3d12_mesh PROPERTIES SKIP_RETURN_CODE 77)
      # The same draws through the heavy pipeline cache (src/heavy_pipeline_cache.h): every build first asks the
      # offline cache without compiling, and a miss is built against the heavy cache in a folder of its own.
      add_test(NAME d3d12_mesh_heavy_cache COMMAND d3d12_mesh_test)
      set_tests_properties(d3d12_mesh_heavy_cache PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT
          "LEONIDA_HEAVY_PIPELINE_CACHE_MS=1;LEONIDA_PIPELINE_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/heavy_cache_test")
    else()
      message(STATUS "glslangValidator not found: d3d12_compute_test, d3d12_draw_test, d3d12_set0_compute_test and d3d12_mesh_test are not built")
    endif()

    # Native D3D12 SV_InnerCoverage reference; its HLSL is compiled at runtime, so glslang is not needed.
    add_executable(d3d12_conservative_tier3_reference_test test/d3d12_conservative_tier3_reference_test.cpp)
    target_link_libraries(d3d12_conservative_tier3_reference_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_conservative_tier3_reference_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_conservative_tier3_reference_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_conservative_tier3_reference COMMAND d3d12_conservative_tier3_reference_test)
    set_tests_properties(d3d12_conservative_tier3_reference PROPERTIES SKIP_RETURN_CODE 77)

    # Subgroup size control in PSOs, end to end: runtime/test/shaders/cs_w64x_readlane.asm translated natively and with
    # --wave64-emulate, and cs_wqm_w32.asm, built here with tools/rdna2spirv.py through test/shaders/build.py. Needs
    # llvm-mc with the AMDGPU target (LLVM_MC in the environment, as for the other shader tests, or llvm-mc on PATH)
    # and the Vulkan SDK's spirv tools; without them the test is not built.
    if(DEFINED ENV{LLVM_MC})
      set(LEONIDA_LLVM_MC "$ENV{LLVM_MC}")
    else()
      find_program(LEONIDA_LLVM_MC llvm-mc HINTS "C:/msys64/ucrt64/bin")
    endif()
    if(LEONIDA_LLVM_MC AND Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
      set(LEONIDA_SG_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/subgroup_shaders)
      set(LEONIDA_SG_INDEXES "")
      foreach(case "w64_native;cs_w64x_readlane;0" "w64_emulated;cs_w64x_readlane;1" "w32_native;cs_wqm_w32;0"
                       "ps_w64_native;ps_w64x_readlane;0")
        list(GET case 0 folder)
        list(GET case 1 shader)
        list(GET case 2 emulate)
        set(index ${LEONIDA_SG_DIR}/${folder}/spirv/index.json)
        add_custom_command(
          OUTPUT ${index}
          # The standalone harness runs these with the V# at s[0:3]; d3d12_subgroup_test passes it as four root
          # constants, which the D3D12 compute layout puts at s[4:7] (LEONIDA_USER_DATA_SHIFT=4, D3D-448).
          COMMAND ${CMAKE_COMMAND} -E env LLVM_MC=${LEONIDA_LLVM_MC} LEONIDA_W64_EMULATE=${emulate}
                  LEONIDA_USER_DATA_SHIFT=4
                  $<TARGET_FILE:Python3::Interpreter> -I ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/build.py
                  ${LEONIDA_SG_DIR}/${folder} ${shader}
          DEPENDS ${LEONIDA_TRANSLATOR_DEPS}
                  ${CMAKE_CURRENT_SOURCE_DIR}/test/shaders/${shader}.asm
          COMMENT "Translating ${shader} for the subgroup size test (${folder})"
          VERBATIM)
        list(APPEND LEONIDA_SG_INDEXES ${index})
      endforeach()
      # The probe shaders that store the subgroup size the driver ran them with, and a one-triangle vertex shader.
      foreach(source cs_sg_probe.comp ps_sg_probe.frag vs_sg_fullscreen.vert)
        set(output ${LEONIDA_SG_DIR}/glsl/${source}.spv)
        add_custom_command(
          OUTPUT ${output}
          COMMAND ${CMAKE_COMMAND} -E make_directory ${LEONIDA_SG_DIR}/glsl
          COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -o ${output}
                  ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/glsl/${source}
          COMMENT "Compiling ${source}"
          VERBATIM)
        list(APPEND LEONIDA_SG_INDEXES ${output})
      endforeach()
      add_executable(d3d12_subgroup_test test/d3d12_subgroup_test.cpp shims/d3d12_xs_device.cpp shims/d3d12_xs_pso_stream.cpp ${LEONIDA_SG_INDEXES})
      target_link_libraries(d3d12_subgroup_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(d3d12_subgroup_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
                                 LEONIDA_TEST_SUBGROUP_SHADERS="${LEONIDA_SG_DIR}")
      target_compile_options(d3d12_subgroup_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME d3d12_subgroup COMMAND d3d12_subgroup_test)
      set_tests_properties(d3d12_subgroup PROPERTIES SKIP_RETURN_CODE 77)
    else()
      message(STATUS "llvm-mc (set LLVM_MC) or glslangValidator not found: d3d12_subgroup_test is not built")
    endif()

    # Clears on every format class against WARP (needs a Vulkan GPU; exit 77 without one).
    add_executable(d3d12_clear_formats_test test/d3d12_clear_formats_test.cpp)
    target_link_libraries(d3d12_clear_formats_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_clear_formats_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_clear_formats_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_clear_formats COMMAND d3d12_clear_formats_test)
    set_tests_properties(d3d12_clear_formats PROPERTIES SKIP_RETURN_CODE 77)

    # SetDescriptorHeaps validation, Reset/ClearState/bundle parity, lifetime pinning, direct-index flags.
    add_executable(d3d12_set_descriptor_heaps_test test/d3d12_set_descriptor_heaps_test.cpp)
    target_link_libraries(d3d12_set_descriptor_heaps_test PRIVATE gta6rt_d3d12_com d3d12 dxgi)
    target_compile_definitions(d3d12_set_descriptor_heaps_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(d3d12_set_descriptor_heaps_test PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME d3d12_set_descriptor_heaps COMMAND d3d12_set_descriptor_heaps_test)
    set_tests_properties(d3d12_set_descriptor_heaps PROPERTIES SKIP_RETURN_CODE 77
                         FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")

    # The loader can create this device instead of the host's (D3D12XboxCreateDevice, LEONIDA_D3D12=vulkan).
    if(TARGET gta6rt)
      target_link_libraries(gta6rt PRIVATE gta6rt_d3d12_com user32)  # user32: the PresentX window
      target_compile_definitions(gta6rt PRIVATE LEONIDA_VULKAN_DEVICE)

      # gta6rt --precompile-pipelines builds the programs LEONIDA_SKIP_COMPUTE names unless --skip-listed is given,
      # and its last line counts both (shims/d3d12_precompile.h). Two synthetic compute modules, three runs.
      if(TARGET gta6rt_compute_journal_shader)
        add_dependencies(gta6rt gta6rt_indirect_count_shader gta6rt_compute_journal_shader)
        add_test(NAME precompile_skip_list COMMAND ${CMAKE_COMMAND}
          "-DGTA6RT=$<TARGET_FILE:gta6rt>" "-DRUNNER=${CMAKE_CROSSCOMPILING_EMULATOR}"
          "-DSPV_A=${_indirect_count_spv}" "-DSPV_B=${_journal_second_spv}"
          "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/precompile_skip_list"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_precompile_skip_list.cmake")
        set_tests_properties(precompile_skip_list PROPERTIES TIMEOUT 240 SKIP_REGULAR_EXPRESSION "SKIP: ")
      endif()

      # A stand-in title that imports d3d12_xs.dll!D3D12XboxCreateDevice and runs a clear and a readback on the
      # device it gets, mapped and run by `gta6rt --run` with LEONIDA_D3D12=vulkan: the loader, the game thread, the
      # proxies and the Vulkan device in one process. check_tiny_frame.py reads the verdict, the binding and the
      # call order from outside; d3d12_com_test is its probe for "no usable Vulkan device" (exit code 77).
      add_library(d3d12_xs_stub SHARED test/fixtures/d3d12_xs_stub.cpp test/fixtures/d3d12_xs_stub.def)
      target_compile_definitions(d3d12_xs_stub PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(d3d12_xs_stub PRIVATE /W4 /permissive- /utf-8)
      set_target_properties(d3d12_xs_stub PROPERTIES
        OUTPUT_NAME d3d12_xs
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/stubs/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/stubs/$<CONFIG>"
      )
      add_executable(tiny_frame test/fixtures/tiny_frame.cpp)
      target_link_libraries(tiny_frame PRIVATE d3d12_xs_stub)
      target_compile_definitions(tiny_frame PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(tiny_frame PRIVATE /W4 /permissive- /utf-8)
      set_target_properties(tiny_frame PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/game/$<CONFIG>"
      )
      if(Python3_FOUND)
        add_test(NAME run_tiny_frame COMMAND "${Python3_EXECUTABLE}" -I
                 "${CMAKE_CURRENT_SOURCE_DIR}/test/check_tiny_frame.py" "$<TARGET_FILE:gta6rt>"
                 "$<TARGET_FILE:tiny_frame>" "--probe=$<TARGET_FILE:d3d12_com_test>" ${_trace_runner})
        set_tests_properties(run_tiny_frame PROPERTIES SKIP_RETURN_CODE 77)
        # The queue test settings are for the tests only: left set, they must not shrink the game's device.
        add_test(NAME run_tiny_frame_queue_test_settings COMMAND "${Python3_EXECUTABLE}" -I
                 "${CMAKE_CURRENT_SOURCE_DIR}/test/check_tiny_frame.py" "$<TARGET_FILE:gta6rt>"
                 "$<TARGET_FILE:tiny_frame>" "--probe=$<TARGET_FILE:d3d12_com_test>" --queue-test-settings
                 ${_trace_runner})
        set_tests_properties(run_tiny_frame_queue_test_settings PROPERTIES SKIP_RETURN_CODE 77)
      endif()
    endif()

    # M1.5 as a `gta6rt --run` guest: test/d3d12_translated_draw_test.cpp built again with LEONIDA_M15_GUEST as
    # tiny_guest_draw.exe, whose only device is the D3D12XboxCreateDevice it imports from d3d12_xs.dll. The import
    # library comes from a failing stub built into fixtures/stubs, away from the guest, so only gta6rt's shim registry
    # can answer it. The guest links nothing from src/ or shims/ and can be
    # copied with its ordinary CRT dependencies, given the m15 folder as its argument. test/run_guest_draw.cmake checks the exit code and the marker line.
    # Opt-in: it needs a Vulkan GPU and the guest cannot tell a missing one from a bug, so default runs are unchanged.
    option(LEONIDA_TEST_GUEST_DRAW
           "Build tiny_guest_draw.exe and run the M1.5 draw through gta6rt --run (needs a Vulkan GPU)" OFF)
    option(LEONIDA_GUEST_SCENE "Build the headless 600-frame synthetic guest test (requires static SDL3)" OFF)
    if(LEONIDA_TEST_GUEST_DRAW OR LEONIDA_GUEST_SCENE)
      # Share the import stub with the tiny_frame clear/readback guest from PR #3488.
      if(NOT TARGET d3d12_xs_stub)
        add_library(d3d12_xs_stub SHARED test/fixtures/d3d12_xs_stub.cpp test/fixtures/d3d12_xs_stub.def)
        target_compile_definitions(d3d12_xs_stub PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(d3d12_xs_stub PRIVATE /W4 /permissive- /utf-8)
        set_target_properties(d3d12_xs_stub PROPERTIES
          OUTPUT_NAME d3d12_xs
          PREFIX ""
          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/stubs/$<CONFIG>"
          ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/stubs/$<CONFIG>"
        )
      endif()
      add_executable(tiny_guest_draw test/d3d12_translated_draw_test.cpp)
      target_link_libraries(tiny_guest_draw PRIVATE d3d12_xs_stub Vulkan::Headers)
      target_compile_definitions(tiny_guest_draw PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX LEONIDA_M15_GUEST)
      target_compile_options(tiny_guest_draw PRIVATE /W4 /permissive- /utf-8)
      set_target_properties(tiny_guest_draw PROPERTIES
        OUTPUT_NAME tiny_guest_draw
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/guest_draw/$<CONFIG>"
      )
      if(LEONIDA_GUEST_SCENE)
        if(NOT TARGET gta6rt OR NOT TARGET SDL3::SDL3-static OR NOT LEONIDA_SDL3)
          message(FATAL_ERROR "LEONIDA_GUEST_SCENE requires static SDL3; configure SDL3_DIR or LEONIDA_SDL3_FETCH")
        endif()
        target_compile_definitions(tiny_guest_draw PRIVATE LEONIDA_GUEST_SCENE)
        target_sources(gta6rt PRIVATE test/scene_input_driver.cpp)
      endif()
      # This guest can use a synthetic fixture produced on another machine. Register the automated test
      # if the tools to generate the fixture are available locally.
      if(CMAKE_HOST_WIN32 AND NOT CMAKE_CROSSCOMPILING AND TARGET d3d12_translated_draw_test AND TARGET gta6rt)
        add_dependencies(tiny_guest_draw d3d12_translated_draw_test gta6rt)
        add_test(NAME d3d12_translated_draw_guest
                 COMMAND ${CMAKE_COMMAND} -DGTA6RT=$<TARGET_FILE:gta6rt> -DGUEST=$<TARGET_FILE:tiny_guest_draw>
                         -DFIXTURE=${LEONIDA_M15_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/test/run_guest_draw.cmake)
        set_tests_properties(d3d12_translated_draw_guest PROPERTIES TIMEOUT 180)
      else()
        message(STATUS "tiny_guest_draw will build. For an external synthetic M1.5 fixture or Wine run see runtime/README.md.")
      endif()
      if(LEONIDA_GUEST_SCENE AND Python3_Interpreter_FOUND AND TARGET d3d12_translated_draw_test)
        set(_scene_wine_args)
        if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
          set(_scene_emulator ${CMAKE_CROSSCOMPILING_EMULATOR})
          list(POP_FRONT _scene_emulator _scene_launcher)
          list(APPEND _scene_wine_args "--wine=${_scene_launcher}")
          foreach(_arg IN LISTS _scene_emulator)
            list(APPEND _scene_wine_args "--wine-arg=${_arg}")
          endforeach()
          add_dependencies(tiny_guest_draw d3d12_translated_draw_test gta6rt)
        endif()
        if((CMAKE_HOST_WIN32 AND NOT CMAKE_CROSSCOMPILING) OR _scene_wine_args)
          add_test(NAME d3d12_guest_scene
                   COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/run_guest_scene.py"
                           --loader $<TARGET_FILE:gta6rt> --guest $<TARGET_FILE:tiny_guest_draw>
                           --shaders "${LEONIDA_M15_DIR}" --negative-controls ${_scene_wine_args})
          set_tests_properties(d3d12_guest_scene PROPERTIES TIMEOUT 900)
        endif()
      endif()
    endif()
    # bvh_builder_test can put the package's XboxBvhBuilder on this device too (LEONIDA_BVH_DEVICE=vulkan).
    if(TARGET bvh_builder_test)
      target_link_libraries(bvh_builder_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(bvh_builder_test PRIVATE LEONIDA_VULKAN_DEVICE)
    endif()
    # fsr_dispatch_test can put the package's FidelityFX DLL on this device too (LEONIDA_FSR_DEVICE=vulkan).
    if(TARGET fsr_dispatch_test)
      target_link_libraries(fsr_dispatch_test PRIVATE gta6rt_d3d12_com)
      target_compile_definitions(fsr_dispatch_test PRIVATE LEONIDA_VULKAN_DEVICE)
    endif()
  elseif(TARGET gta6rt)
    message(STATUS "Python 3 or d3d12.h missing: the D3D12 COM objects and d3d12_com_test are not built")
  endif()

  # Needs only the Vulkan headers, not a GPU.
  if(BUILD_TESTING)
    add_executable(typeless_rtv_test test/typeless_rtv_test.cpp)
    target_link_libraries(typeless_rtv_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(typeless_rtv_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME typeless_rtv COMMAND typeless_rtv_test)
    add_executable(pipeline_cache_snapshot_test test/pipeline_cache_snapshot_test.cpp)
    target_link_libraries(pipeline_cache_snapshot_test PRIVATE Vulkan::Headers)
    add_test(NAME pipeline_cache_snapshot COMMAND pipeline_cache_snapshot_test)
    add_executable(predicated_copy_test test/predicated_copy_test.cpp)
    target_link_libraries(predicated_copy_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(predicated_copy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME predicated_copy COMMAND predicated_copy_test)
    # CopyResource (buffer-to-buffer) and CopyTexture (including depth24_as_float) apply ResourceBinding::buffer_offset (no GPU).
    add_executable(replay_copy_offset_test test/replay_copy_offset_test.cpp)
    target_link_libraries(replay_copy_offset_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_copy_offset_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_copy_offset COMMAND replay_copy_offset_test)
    # KHR CopyRaytracingAccelerationStructure (CLONE, COMPACT) and COMPACTED_SIZE postbuild (no GPU).
    add_executable(replay_khr_as_copy_postbuild_test test/replay_khr_as_copy_postbuild_test.cpp)
    target_link_libraries(replay_khr_as_copy_postbuild_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_khr_as_copy_postbuild_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    if(MSVC)
      target_compile_options(replay_khr_as_copy_postbuild_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME replay_khr_as_copy_postbuild COMMAND replay_khr_as_copy_postbuild_test)
    # PreparedList::wrote_readback and readback_src_stages match the actual producer stage (depth24, rgb96, UAV clear; no GPU).
    add_executable(replay_readback_stages_test test/replay_readback_stages_test.cpp)
    target_link_libraries(replay_readback_stages_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_readback_stages_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_readback_stages COMMAND replay_readback_stages_test)
    # Pipelines not built yet, left to the pool of pipeline_workers.h: what prepare() alone would give (no GPU).
    add_executable(replay_pipeline_compiles_test test/replay_pipeline_compiles_test.cpp)
    target_link_libraries(replay_pipeline_compiles_test PRIVATE Vulkan::Headers Threads::Threads)
    if(WIN32)
      target_compile_definitions(replay_pipeline_compiles_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_pipeline_compiles COMMAND replay_pipeline_compiles_test)
    add_test(NAME replay_pipeline_compiles_strict COMMAND replay_pipeline_compiles_test)
    set_tests_properties(replay_pipeline_compiles_strict PROPERTIES ENVIRONMENT "LEONIDA_STRICT_REPLAY=1")
    # The request hook of ComputePipeline / GraphicsPipeline that pipeline_order.h's recorder hangs on (no GPU).
    add_executable(replay_pipeline_order_hook_test test/replay_pipeline_order_hook_test.cpp)
    target_link_libraries(replay_pipeline_order_hook_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_pipeline_order_hook_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_pipeline_order_hook COMMAND replay_pipeline_order_hook_test)
    # Compiling unbuilt pipelines releases resource_mutex_ and tracker_mutex_ while keeping pipeline_mutex_ (no GPU).
    add_executable(replay_pipeline_compile_unlock_test test/replay_pipeline_compile_unlock_test.cpp)
    target_link_libraries(replay_pipeline_compile_unlock_test PRIVATE Vulkan::Headers Threads::Threads)
    if(WIN32)
      target_compile_definitions(replay_pipeline_compile_unlock_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    if(MSVC)
      target_compile_options(replay_pipeline_compile_unlock_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME replay_pipeline_compile_unlock COMMAND replay_pipeline_compile_unlock_test)
    add_executable(replay_quad_list_test test/replay_quad_list_test.cpp)
    target_link_libraries(replay_quad_list_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_quad_list_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_quad_list COMMAND replay_quad_list_test)
    # emit of an enhanced texture barrier: no heap allocation for one layout piece, one barrier for each piece (no GPU).
    add_executable(replay_texture_barrier_test test/replay_texture_barrier_test.cpp)
    target_link_libraries(replay_texture_barrier_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_texture_barrier_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_texture_barrier COMMAND replay_texture_barrier_test)
    add_executable(clear_color_test test/clear_color_test.cpp)
    target_link_libraries(clear_color_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(clear_color_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME clear_color COMMAND clear_color_test)
    # FramePacer of the Xbox presentation path on a fake clock (src/frame_pacer.h); no GPU, no Vulkan.
    add_executable(frame_pacer_test test/frame_pacer_test.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(frame_pacer_test PRIVATE Threads::Threads)
    if(MSVC)
      target_compile_options(frame_pacer_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME frame_pacer COMMAND frame_pacer_test)

    # Replay refuses a list whose whole-view or implicit zero-clear needs an unset vkCmdClear*Image (no GPU).
    add_executable(replay_clear_gate_test test/replay_clear_gate_test.cpp)
    target_link_libraries(replay_clear_gate_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_clear_gate_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_clear_gate COMMAND replay_clear_gate_test)
    # WriteBufferImmediate suspends conditional rendering around vkCmdUpdateBuffer so Intel drivers do not drop it.
    add_executable(replay_write_immediate_predication_test test/replay_write_immediate_predication_test.cpp)
    target_link_libraries(replay_write_immediate_predication_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_write_immediate_predication_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_write_immediate_predication COMMAND replay_write_immediate_predication_test)
    add_executable(d3d12_state_map_test test/d3d12_state_map_test.cpp)
    target_link_libraries(d3d12_state_map_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(d3d12_state_map_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME d3d12_state_map COMMAND d3d12_state_map_test)
    add_test(NAME d3d12_bounded_index_view COMMAND d3d12_state_map_test --bounded-index-view)
    add_executable(vulkan_title_image_pool_test test/vulkan_title_image_pool_test.cpp)
    target_link_libraries(vulkan_title_image_pool_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(vulkan_title_image_pool_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME vulkan_title_image_pool COMMAND vulkan_title_image_pool_test)
    add_executable(indirect_dispatch_count_alloc_test test/indirect_dispatch_count_alloc_test.cpp)
    target_link_libraries(indirect_dispatch_count_alloc_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(indirect_dispatch_count_alloc_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME indirect_dispatch_count_alloc COMMAND indirect_dispatch_count_alloc_test)
    # IndirectArgumentClamp queue-family sharing, bind-retry cleanup, slot zeroing, offset alignment, and concurrency (no GPU).
    add_executable(indirect_clamp_safety_test test/indirect_clamp_safety_test.cpp)
    target_link_libraries(indirect_clamp_safety_test PRIVATE Vulkan::Headers Threads::Threads)
    if(WIN32)
      target_compile_definitions(indirect_clamp_safety_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME indirect_clamp_safety COMMAND indirect_clamp_safety_test)
    # A buffer UAV clear writes where its resource starts in a shared VkBuffer (no GPU).
    add_executable(replay_uav_clear_offset_test test/replay_uav_clear_offset_test.cpp)
    target_link_libraries(replay_uav_clear_offset_test PRIVATE Vulkan::Headers)
    add_test(NAME replay_uav_clear_offset COMMAND replay_uav_clear_offset_test)
    # ResolveQueryData suspends conditional rendering around un-ended vkCmdFillBuffer runs as well as dispatches (D3D-148).
    add_executable(replay_query_predication_test test/replay_query_predication_test.cpp)
    target_link_libraries(replay_query_predication_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_query_predication_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_query_predication COMMAND replay_query_predication_test)
    # Query edge cases and queue ownership, with CPU mocks (no GPU or game data).
    add_executable(replay_query_edges_test test/replay_query_edges_test.cpp)
    target_link_libraries(replay_query_edges_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_query_edges_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_query_edges COMMAND replay_query_edges_test)
    # Stateful indirect draw topology coverage and per-reason indirect_stats counters (no GPU or game data).
    add_executable(indirect_topology_and_stats_test test/indirect_topology_and_stats_test.cpp)
    target_link_libraries(indirect_topology_and_stats_test PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(indirect_topology_and_stats_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME indirect_topology_and_stats COMMAND indirect_topology_and_stats_test)
    # GFX10 T#/S#/V# descriptor encoding and its Vulkan mappings (src/gfx10_descriptors.h).
    add_executable(gfx10_descriptors_test test/gfx10_descriptors_test.cpp)
    target_link_libraries(gfx10_descriptors_test PRIVATE Vulkan::Headers)
    add_test(NAME gfx10_descriptors COMMAND gfx10_descriptors_test)
    # CPU cost of the D3D12 -> Vulkan replay per command (record, prepare, emit) with no-op Vulkan entry points:
    # no GPU or driver. ctest runs its quick self-test; run it without arguments for the numbers.
    add_executable(replay_cpu_bench test/replay_cpu_bench.cpp)
    target_link_libraries(replay_cpu_bench PRIVATE Vulkan::Headers)
    if(WIN32)
      target_compile_definitions(replay_cpu_bench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME replay_cpu_bench_selftest COMMAND replay_cpu_bench --self-test)
    add_test(NAME vk_descriptor_bench_selftest COMMAND vk_descriptor_bench --self-test)
    # mods.h: mod.json manifests and archetype swaps (docs/modding.md)
    add_executable(mods_test test/mods_test.cpp)
    target_link_libraries(mods_test PRIVATE Vulkan::Headers)
    add_test(NAME mods COMMAND mods_test)
    if(WIN32)
      # code_hooks.h: the remapping thunk and direct-call redirection the archetype hook uses (x64 Windows)
      add_executable(code_hooks_test test/code_hooks_test.cpp)
      target_link_libraries(code_hooks_test PRIVATE Vulkan::Headers)
      target_compile_definitions(code_hooks_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      add_test(NAME code_hooks COMMAND code_hooks_test)
    endif()
    # The subgroup size a pipeline stage asks for, from synthetic index entries and device properties.
    add_executable(subgroup_size_test test/subgroup_size_test.cpp)
    target_link_libraries(subgroup_size_test PRIVATE Vulkan::Headers)
    add_test(NAME subgroup_size COMMAND subgroup_size_test)
    add_executable(conservative_raster_test test/conservative_raster_test.cpp)
    add_test(NAME conservative_raster COMMAND conservative_raster_test)
    # The GPU address arena's size from LEONIDA_ARENA_MB and synthetic device buffer limits.
    add_executable(arena_size_test test/arena_size_test.cpp)
    target_link_libraries(arena_size_test PRIVATE Vulkan::Headers)
    add_test(NAME arena_size COMMAND arena_size_test)
    # The GPU address arena's free-block map: first fit, coalescing, and a walk a nested call re-cuts on the same
    # thread (src/arena_blocks.h).
    add_executable(arena_blocks_test test/arena_blocks_test.cpp)
    if(WIN32 AND MSVC)
      target_compile_options(arena_blocks_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME arena_blocks COMMAND arena_blocks_test)
    # The mem probe's breakdown line: the tally by consumer, the owner scopes and the text (src/memory_breakdown.h).
    add_executable(memory_breakdown_test test/memory_breakdown_test.cpp)
    if(WIN32 AND MSVC)
      target_compile_options(memory_breakdown_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME memory_breakdown COMMAND memory_breakdown_test)
    # The address guard's page table (src/address_guard.h; docs/d3d12_vulkan.md, "Address guard").
    add_executable(address_guard_test test/address_guard_test.cpp)
    if(WIN32 AND MSVC)
      target_compile_options(address_guard_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME address_guard COMMAND address_guard_test)
    add_executable(title_window_plan_bounds_test test/title_window_plan_bounds_test.cpp)
    if(WIN32 AND MSVC)
      target_compile_options(title_window_plan_bounds_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME title_window_plan_bounds COMMAND title_window_plan_bounds_test)
    # What each console resource-heap V# resolved to, and the slots a change touches (src/console_heap_slot_records.h).
    add_executable(console_heap_slot_records_test test/console_heap_slot_records_test.cpp)
    if(WIN32 AND MSVC)
      target_compile_options(console_heap_slot_records_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME console_heap_slot_records COMMAND console_heap_slot_records_test)
    # A slot-133 shadow keeps a GPU write until the title's bytes under it change (src/shadow_keep.h).
    add_executable(shadow_keep_test test/shadow_keep_test.cpp)
    if(WIN32 AND MSVC)
      target_compile_options(shadow_keep_test PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME shadow_keep COMMAND shadow_keep_test)
    # Queue construction cleans its Vulkan handles if holder registration runs out of host memory.
    add_executable(vk_host_texture_order_test test/vk_host_texture_order_test.cpp)
    target_link_libraries(vk_host_texture_order_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    if(WIN32)
      target_compile_definitions(vk_host_texture_order_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    add_test(NAME vk_host_texture_order COMMAND vk_host_texture_order_test)
    add_executable(vk_queue_constructor_oom_test test/vk_queue_constructor_oom_test.cpp)
    target_link_libraries(vk_queue_constructor_oom_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    if(WIN32)
      target_compile_definitions(vk_queue_constructor_oom_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      if(MSVC)
        target_compile_options(vk_queue_constructor_oom_test PRIVATE /W4 /permissive- /EHsc /utf-8)
      endif()
    endif()
    add_test(NAME vk_queue_constructor_oom COMMAND vk_queue_constructor_oom_test)
    # XDXR AccelerationStructurePostbuild flushes COMPUTE_SHADER_BIT -> HOST_BIT for readback visibility.
    add_executable(vk_xdxr_postbuild_readback_test test/vk_xdxr_postbuild_readback_test.cpp)
    target_link_libraries(vk_xdxr_postbuild_readback_test PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    if(WIN32)
      target_compile_definitions(vk_xdxr_postbuild_readback_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      if(MSVC)
        target_compile_options(vk_xdxr_postbuild_readback_test PRIVATE /W4 /permissive- /EHsc /utf-8)
      endif()
    endif()
    add_test(NAME vk_xdxr_postbuild_readback COMMAND vk_xdxr_postbuild_readback_test)

    # Title-placed buffer VAs (device slot 133) and the console VA shaders reach them by (src/title_va_alias.h).
    add_executable(title_va_alias_test test/title_va_alias_test.cpp)
    add_test(NAME title_va_alias COMMAND title_va_alias_test)
    if(WIN32)
      # A refused title window never hands a shader a raw address, and a stream pool's GPU-only alias and CPU view
      # share one mapping (src/title_window_plan.h, src/title_page_watch.h title_gpu_views).
      add_executable(title_window_plan_test test/title_window_plan_test.cpp)
      target_compile_definitions(title_window_plan_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(title_window_plan_test PRIVATE /W4 /permissive- /utf-8)
      add_test(NAME title_window_plan COMMAND title_window_plan_test)
      # Geometry copies refreshed by the 64 KiB tiles the title wrote (shims/d3d12_geometry_views.h).
      add_executable(geometry_views_test test/geometry_views_test.cpp)
      target_link_libraries(geometry_views_test PRIVATE Vulkan::Headers)
      target_compile_definitions(geometry_views_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
      target_compile_options(geometry_views_test PRIVATE /W4 /permissive- /EHsc /utf-8)
      add_test(NAME geometry_views COMMAND geometry_views_test)
    endif()
  endif()
else()
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/vulkan_headers_missing.cmake")
  leonida_vulkan_headers_missing()
endif()

# Timed condition-variable waits on Wine 9 (#3796): every Windows executable and DLL built against the dynamic CRT
# (/MD, the default) gets src/crt_timed_wait.cpp, which keeps msvcprt.lib's statically linked timed wait out of the
# link so that a wait and the mutex it unlocks always use msvcp140's own layout. A target that links the CRT
# statically (MSVC_RUNTIME_LIBRARY MultiThreaded, MultiThreadedDebug) already agrees with itself and is left alone,
# as are the fixtures linked with /NODEFAULTLIB, which have no CRT.
if(WIN32 AND MSVC)
  add_library(gta6rt_crt_timed_wait OBJECT src/crt_timed_wait.cpp)
  target_compile_definitions(gta6rt_crt_timed_wait PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(gta6rt_crt_timed_wait PRIVATE /W4 /permissive- /utf-8)
  # A timed wait is woken by a thread that takes its mutex; shared_mutex and sleep_for still work.
  add_executable(crt_timed_wait_test test/crt_timed_wait_test.cpp)
  target_compile_options(crt_timed_wait_test PRIVATE /W4 /permissive- /EHsc /utf-8)
  add_test(NAME crt_timed_wait COMMAND crt_timed_wait_test)
  set_tests_properties(crt_timed_wait PROPERTIES TIMEOUT 60)
  get_property(_gta6rt_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _gta6rt_targets)
    get_target_property(_type ${_target} TYPE)
    if(NOT _type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
      continue()
    endif()
    get_target_property(_crt ${_target} MSVC_RUNTIME_LIBRARY)
    get_target_property(_link_options ${_target} LINK_OPTIONS)
    if((_crt AND NOT _crt MATCHES "DLL") OR "/NODEFAULTLIB" IN_LIST _link_options)
      continue()  # static CRT, or no CRT at all (the CRT-free fixtures)
    endif()
    target_sources(${_target} PRIVATE $<TARGET_OBJECTS:gta6rt_crt_timed_wait>)
  endforeach()
endif()
