# Stops a configure whose Vulkan headers are older than the Windows runtime needs. Any vulkan/vulkan.h used to be
# accepted, so old headers (conda-forge's vulkan-headers resolves to 1.3.231 next to llvm-tools) configured fine and
# then failed deep in the build: d3d12_com.h(42) "'calibrated': unknown override specifier" (no
# PFN_vkGetCalibratedTimestampsKHR) and 100+ errors in d3d12_vulkan_device.cpp. The minimum is measured by building
# the whole Windows runtime against the Khronos Vulkan-Headers tags (docs/testing.md, "Known problems"): v1.3.294
# builds every target, SDK 1.3.280.0 every target but vk_harness (VK_KHR_pipeline_binary, new in v1.3.294), and
# 1.3.231 stops in the D3D12 layer. The first Vulkan SDK at or above it is 1.3.296.0.
# Kept in its own file so tests/test_cmake_vulkan_headers_version.py can run it with `cmake -P`.
set(LEONIDA_VULKAN_HEADERS_MIN 294)

# include_dirs: the include directories of Vulkan::Headers. Generator expressions such as $<BUILD_INTERFACE:...>
# (FetchContent's target) are unwrapped. A header with no VK_HEADER_VERSION line is left alone.
function(leonida_vulkan_headers_version include_dirs)
  set(core "")
  foreach(dir IN LISTS include_dirs)
    string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" dir "${dir}")
    if(dir MATCHES "^\\$<" OR NOT dir)
      continue()
    endif()
    if(EXISTS "${dir}/vulkan/vulkan_core.h")
      set(core "${dir}/vulkan/vulkan_core.h")
      break()
    endif()
  endforeach()
  if(NOT core)
    return()
  endif()
  file(STRINGS "${core}" line REGEX "^#define VK_HEADER_VERSION [0-9]+" LIMIT_COUNT 1)
  if(NOT line MATCHES "VK_HEADER_VERSION ([0-9]+)")
    return()
  endif()
  set(found "${CMAKE_MATCH_1}")
  set(LEONIDA_VULKAN_HEADER_VERSION "${found}" PARENT_SCOPE)
  if(found LESS LEONIDA_VULKAN_HEADERS_MIN)
    string(CONCAT text
        "Vulkan headers too old: ${core} has VK_HEADER_VERSION ${found}, and the Windows runtime needs "
        "${LEONIDA_VULKAN_HEADERS_MIN} or newer (Khronos Vulkan-Headers v1.3.${LEONIDA_VULKAN_HEADERS_MIN}, or a "
        "Vulkan SDK from 1.3.296.0 on). The build would otherwise stop part way, for very old headers deep in the "
        "D3D12 layer with errors such as \"'calibrated': unknown override specifier\". Point VULKAN_SDK (or "
        "Vulkan_INCLUDE_DIR) at newer headers and delete CMakeCache.txt, which keeps the old lookup, or pass "
        "-DLEONIDA_VULKAN_HEADERS_FETCH=ON with no other headers installed to download the pinned Khronos "
        "Vulkan-Headers release.")
    message(FATAL_ERROR "${text}")
  endif()
endfunction()
