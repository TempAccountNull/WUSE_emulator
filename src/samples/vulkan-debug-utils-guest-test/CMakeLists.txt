add_executable(vulkan-debug-utils-guest-test main.cpp)
set_property(TARGET vulkan-debug-utils-guest-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(vulkan-debug-utils-guest-test PRIVATE vulkan-headers)
