# vip_test — Plugify C++ plugin: port of the Pisex VIP module "[VIP] VIP Test".
#
# Pure s2sdk: no pisexcompat. All safety rules (map gate, client/SteamID checks, colour tags, KV1
# config, console/chat command registration) live in src/ and are shared by the plugin and the
# test through vip_test_support.
#
#   plugify-vip_test  the plugin shared library (entry "bin/vip_test" -> bin/libvip_test.so)
#   vip_test_tests    engine-free unit tests: the production vip_test.ini through the KV1 reader,
#                     the cooldown decision of the original handler, the cs2-menus colour table,
#                     the map gate and client checks (fake __s2sdk_* bindings), the command
#                     dispatch with a clean unload, and the whole grant/report flow through fake
#                     vip/permissions bindings (build/tests/vip_test_tests)

set(VIP_TEST_VERSION "1.1.0")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*       (s2sdk_bindings)
#   __vip_*         (vip consumer bindings, generated into plugins/vip/include/external/plugify/include)
#   __permissions_* (permissions_bindings; the cooldown cookie lives in the permissions plugin)
# In the monorepo plugins/vip is added first ("vip" < "vip_test"), so vip_bindings already exists;
# a standalone exported repo only carries plugins/vip/include/ headers + vip.cpp and has no
# plugins/vip/CMakeLists.txt - define vip_bindings here when absent.
if(NOT TARGET vip_bindings)
    add_library(vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

# The engine-free half of the plugin: formatting/colour tags, the KV1 reader, the bare chat word,
# the configuration reader (both layouts), the cooldown decision, the map/client guards and the
# command registration. Compiled once into the plugin and once into the test; the __s2sdk_* /
# __vip_* / __permissions_* binding globals are linked by the final targets, never by this library.
add_library(vip_test_support STATIC
        src/text.cpp
        src/kv1.cpp
        src/chat_words.cpp
        src/grant.cpp
        src/config.cpp
        src/log.cpp
        src/guards.cpp
        src/commands.cpp
)
target_include_directories(vip_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(vip_test_support SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_test_support PUBLIC plugify_sdk)
target_compile_options(vip_test_support PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_test_support PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

add_library(plugify-vip_test SHARED
        src/plugin.cpp
        # The binding globals the plugify module fills from the manifest's dependencies; compiled
        # in, never linked as a library.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
        $<TARGET_OBJECTS:permissions_bindings>
)
target_include_directories(plugify-vip_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-vip_test SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-vip_test PRIVATE
        vip_test_support
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-vip_test PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-vip_test PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_test"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-vip_test PROPERTIES OUTPUT_NAME vip_test)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-vip_test PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/vip_test.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vip_test.pplugin"
)

# --- tests --------------------------------------------------------------------------------------
# vip_test_tests: no engine. The plugin TU is compiled into the test (the whole lifecycle and the
# handler are driven directly) and every __s2sdk_* / __vip_* / __permissions_* binding slot is a
# recording fake: the map gate (B1/B2), HasClient()/ClientSteamID64() (C2/B5), the guarded output,
# the console/chat sources and the say/say_team words (C11), the cooldown decision on the
# production values, the permissions cookie flow and a clean unload (A7). No pisexcompat.
add_executable(vip_test_tests
        tests/vip_test_tests.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
        $<TARGET_OBJECTS:permissions_bindings>
)
target_include_directories(vip_test_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vip_test_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_test_tests PRIVATE
        vip_test_support
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(vip_test_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_test_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_test"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
        VIP_TEST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(vip_test_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
