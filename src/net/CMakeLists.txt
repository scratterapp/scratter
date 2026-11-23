# src/net: HTTP for the Apple Music API (libcurl, #104), and later the catalog
# parser (simdjson, #105).
#
# Where libcurl comes from (decided on the roadmap, #6, 2026-11-22):
#   Linux    the system package, through pkg-config (Arch and Ubuntu build it
#            with HTTP/2 and the system's TLS and CA certificates).
#   Windows  there is no system libcurl, so FetchContent builds it here, pinned
#            to a release, with Schannel (Windows' own TLS and certificate
#            store, so there's no OpenSSL to carry). No nghttp2 yet, so
#            Windows speaks HTTP/1.1 with keep-alive; the code asks for HTTP/2
#            and gets it wherever curl has it.
if(WIN32)
    include(FetchContent)
    # Plain variables, not cache entries: curl's option()s take them
    # (policy CMP0077), and they stay inside this directory.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    set(BUILD_CURL_EXE OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_LIBCURL_DOCS OFF)
    set(BUILD_MISC_DOCS OFF)
    set(ENABLE_CURL_MANUAL OFF)
    set(CURL_DISABLE_INSTALL ON) # `cmake --install` installs our programs, not curl
    # HTTP and HTTPS only: no FTP, SMTP, LDAP... means less code facing the network.
    foreach(protocol DICT FILE FTP GOPHER IMAP LDAP LDAPS MQTT POP3 IPFS RTSP SMB SMTP TELNET
                     TFTP WEBSOCKETS)
        set(CURL_DISABLE_${protocol} ON)
    endforeach()
    set(CURL_USE_SCHANNEL ON)
    set(CURL_USE_OPENSSL OFF)
    set(CURL_USE_LIBPSL OFF)
    set(CURL_USE_LIBSSH2 OFF)
    set(USE_NGHTTP2 OFF)
    set(USE_LIBIDN2 OFF)
    # These three are cache strings in curl (AUTO, ON or OFF), which a plain
    # variable doesn't override. AUTO would link whatever copy it finds on the
    # build machine (an MSYS2 zstd, say).
    set(CURL_ZLIB OFF CACHE STRING "" FORCE)
    set(CURL_BROTLI OFF CACHE STRING "" FORCE)
    set(CURL_ZSTD OFF CACHE STRING "" FORCE)
    FetchContent_Declare(
        curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        # curl-8_23_0, by commit.
        GIT_TAG 3163ec065643ba20709f190111f42c3e1fe95273
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(curl)
    set(AM_CURL_TARGET CURL::libcurl_static)
else()
    find_package(PkgConfig REQUIRED)
    # 7.85 for CURLOPT_PROTOCOLS_STR; Ubuntu 24.04 has 8.5 and Arch is current.
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl>=7.85)
    set(AM_CURL_TARGET PkgConfig::CURL)
endif()

add_library(am_net STATIC
    catalog_parser.cpp
    http_client.cpp
)
target_link_libraries(am_net PUBLIC am_pal am_common PRIVATE ${AM_CURL_TARGET})
am_enable_warnings(am_net)
