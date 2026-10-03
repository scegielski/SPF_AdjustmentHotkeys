cmake_minimum_required(VERSION 3.16)
project(MirrorControls LANGUAGES CXX ASM_MASM)
set(CMAKE_CXX_STANDARD 20)
add_library(SPF_MirrorControls SHARED MirrorControls.cpp SlotHooks.asm)
target_include_directories(SPF_MirrorControls PRIVATE SPF_API)
target_link_libraries(SPF_MirrorControls PRIVATE bcrypt winmm)
target_compile_definitions(SPF_MirrorControls PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SPF_MirrorControls PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
enable_testing()
add_executable(MirrorControlsTests Tests.cpp SlotHooks.asm TestHooks.asm)
target_include_directories(MirrorControlsTests PRIVATE SPF_API)
target_link_libraries(MirrorControlsTests PRIVATE bcrypt winmm)
target_compile_definitions(MirrorControlsTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME MirrorRegression COMMAND MirrorControlsTests)

add_custom_command(TARGET SPF_MirrorControls POST_BUILD
 COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/package/SPF_MirrorControls"
 COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:SPF_MirrorControls>" "${CMAKE_BINARY_DIR}/package/SPF_MirrorControls/"
 COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/sounds" "${CMAKE_BINARY_DIR}/package/SPF_MirrorControls/sounds")
