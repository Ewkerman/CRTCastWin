cmake_minimum_required(VERSION 3.20)
project(crtcast_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # no VC++ redist needed

set(OUT_DIR ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${OUT_DIR})
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${OUT_DIR})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFG} ${OUT_DIR})
endforeach()

# The Groovy_MiSTer sender: the same unchanged copy as the Apple apps (CRTCast repo).
set(GROOVY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../shared/Groovy)

# groovymister's Windows sender uses Registered I/O (RIO). With CRTCAST_GROOVY_RIO=OFF, a copy of
# groovymister.cpp with USE_RIO set to 0 is compiled instead (plain sockets, as on macOS/Linux).
# The source in shared/Groovy stays unchanged either way.
option(CRTCAST_GROOVY_RIO "Use groovymister's Registered I/O sender on Windows" ON)
if(CRTCAST_GROOVY_RIO)
    set(GROOVY_CPP ${GROOVY_DIR}/groovymister.cpp)
else()
    file(READ ${GROOVY_DIR}/groovymister.cpp GROOVY_SRC)
    string(REPLACE "#define USE_RIO 1" "#define USE_RIO 0" GROOVY_SRC "${GROOVY_SRC}")
    set(GROOVY_CPP ${CMAKE_BINARY_DIR}/groovy_norio/groovymister.cpp)
    file(WRITE ${GROOVY_CPP} "${GROOVY_SRC}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GROOVY_DIR}/groovymister.cpp)
endif()
message(STATUS "groovymister sender: ${GROOVY_CPP} (RIO ${CRTCAST_GROOVY_RIO})")

add_library(groovy STATIC
    ${GROOVY_CPP}
    ${GROOVY_DIR}/GroovyBridge.cpp
    ${GROOVY_DIR}/lz4/lz4.c
    ${GROOVY_DIR}/lz4/lz4hc.c)
# compat/ provides rio.h for MSVC (see compat/rio.h).
target_include_directories(groovy PUBLIC ${GROOVY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_compile_definitions(groovy PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX
                                          _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
target_compile_options(groovy PRIVATE /W1 /wd4005)
target_link_libraries(groovy PUBLIC ws2_32)

add_library(crtcast_core SHARED
    src/engine.cpp
    src/groovy_rio.cpp
    src/airplay_api.cpp
    src/airplay_video.cpp
    src/airplay_source.cpp
    src/airplay_audio.cpp
    src/test_pattern.cpp)
target_include_directories(crtcast_core PUBLIC include PRIVATE src)
target_compile_definitions(crtcast_core PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX
                                                _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(crtcast_core PRIVATE /W3 /utf-8)
if(CRTCAST_GROOVY_RIO)
    target_compile_definitions(crtcast_core PRIVATE CRTCAST_GROOVY_RIO=1)
else()
    target_compile_definitions(crtcast_core PRIVATE CRTCAST_GROOVY_RIO=0)
endif()
target_link_libraries(crtcast_core PRIVATE groovy winmm avrt gdi32 iphlpapi)

add_executable(crtcast_test tools/crtcast_test.cpp)
target_compile_options(crtcast_test PRIVATE /utf-8)
target_link_libraries(crtcast_test PRIVATE crtcast_core)

# ---------------------------------------------------------------------------------------------
# AirPlay receiver: UxPlay core (GPL-3.0), the same copy as the Apple apps (CRTCast repo), in ../shared/AirPlay/UxPlay.
# Windows-only replacements (like the Apple build's Shims/):
#   airplay/uxplay_win/crypto.c, srp.c   UxPlay's original OpenSSL versions (commit 05b0275)
#   airplay/uxplay_win/mdnsd/            UxPlay's own minimal mDNS responder (its default on Windows/Linux),
#                                        used instead of the Bonjour backend lib/dns_sd (no Bonjour install)
#   airplay/compat_win/                  <unistd.h> and clock_gettime for MSVC
find_package(OpenSSL REQUIRED)
find_package(PThreads4W REQUIRED)
find_path(PLIST_INCLUDE_DIR plist/plist.h REQUIRED)
find_library(PLIST_LIBRARY NAMES plist-2.0 libplist-2.0 REQUIRED)

set(UXPLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../shared/AirPlay/UxPlay)
file(GLOB UXPLAY_SRC ${UXPLAY_DIR}/*.c ${UXPLAY_DIR}/llhttp/*.c ${UXPLAY_DIR}/playfair/*.c)
add_library(uxplay STATIC
    ${UXPLAY_SRC}
    airplay/uxplay_win/crypto.c
    airplay/uxplay_win/srp.c
    airplay/uxplay_win/mdnsd/dnssd_mdnsd.c
    airplay/uxplay_win/mdnsd/mdnsd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/AirPlay/AirPlayBridge.c)
set_target_properties(uxplay PROPERTIES C_STANDARD 11)
target_include_directories(uxplay PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/AirPlay
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/airplay/compat_win ${UXPLAY_DIR} ${UXPLAY_DIR}/llhttp ${UXPLAY_DIR}/playfair
            ${CMAKE_CURRENT_SOURCE_DIR}/airplay/uxplay_win/mdnsd ${PLIST_INCLUDE_DIR})
target_compile_definitions(uxplay PRIVATE WIN32 _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX NOHOLD
    PLIST_210 PLIST_230 LIBPLIST_STATIC OPENSSL_API_COMPAT=0x10101000L __PTW32_STATIC_LIB PTW32_STATIC_LIB
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE _WINSOCK_DEPRECATED_NO_WARNINGS)
target_compile_options(uxplay PRIVATE /W1 /utf-8 /FIuxplay_msvc.h)
target_link_libraries(uxplay PUBLIC OpenSSL::Crypto PThreads4W::PThreads4W ${PLIST_LIBRARY}
    ws2_32 iphlpapi dnsapi crypt32 bcrypt advapi32 user32)

# FFmpeg: avcodec decodes the mirror video (and step 4 audio); scaling is our own (src/scale.h).
find_package(FFMPEG REQUIRED)
target_include_directories(crtcast_core PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(crtcast_core PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(crtcast_core PRIVATE uxplay ${FFMPEG_LIBRARIES})
