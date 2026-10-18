if (WIVRN_USE_SYSTEM_BOOST)
    add_library(Boost::pfr ALIAS Boost::headers)
else()
    set(BOOST_INCLUDE_LIBRARIES locale url pfr iostreams)
    set(BOOST_LOCALE_ENABLE_ICU OFF)
    FetchContent_MakeAvailable(boost)
endif()

configure_file(wivrn_config.h.in wivrn_config.h)

add_library(wivrn-common-base STATIC EXCLUDE_FROM_ALL
    utils/strings.cpp
    utils/xdg_base_directory.cpp
    ${CMAKE_BINARY_DIR}/version.cpp
)
add_dependencies(wivrn-common-base wivrn-version)

add_library(wivrn-common STATIC EXCLUDE_FROM_ALL
    crypto.cpp
    smp.cpp
    secrets.cpp
    wivrn_sockets.cpp
    utils/strings.cpp
    vk/allocation.cpp
    vk/error_category.cpp
    vk/vk_allocator.cpp
    vk/vk_mem_alloc.cpp
)

if (WIVRN_BUILD_DASHBOARD OR WIVRN_BUILD_SERVER)
    add_library(wivrn-common-server STATIC EXCLUDE_FROM_ALL
        application.cpp
        utils/flatpak.cpp
        utils/load_icon.cpp
        utils/ini.cpp
        utils/steam_info.cpp
        utils/vdf.cpp
        utils/xdg_icon_lookup.cpp
    )
    target_link_libraries(wivrn-common-server
        PRIVATE nlohmann_json::nlohmann_json PNG::PNG PkgConfig::librsvg-2.0 LibArchive::LibArchive Boost::iostreams
        PUBLIC wivrn-common-base)

    if (WIVRN_BUILD_TEST)
        add_executable(list-apps
            test_application.cpp
        )

        target_link_libraries(list-apps wivrn-common-server)

        add_executable(vdf utils/vdf.cpp)
        target_compile_definitions(vdf PRIVATE WIVRN_BUILD_TEST)
        target_link_libraries(vdf Boost::iostreams)
    endif()
endif()

if(ANDROID)
    # The Vulkan headers in Android do not include the C++ headers, download them
    # Minimum version is 1.3.256
    set(VULKAN_VERSION "1.3.268.0")

    if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz)
        if (EXISTS ${CMAKE_SOURCE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz)
            file(CREATE_LINK ${CMAKE_SOURCE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz SYMBOLIC)
        else()
            file(DOWNLOAD https://github.com/KhronosGroup/Vulkan-Headers/archive/vulkan-sdk-${VULKAN_VERSION}.tar.gz ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz
                EXPECTED_HASH SHA256=94993cbe2b1a604c0d5d9ea37a767e1aba4d771d2bfd4ddceefd66243095164f)
        endif()
    endif()

    file(ARCHIVE_EXTRACT INPUT ${FETCHCONTENT_BASE_DIR}/vulkan-sdk-${VULKAN_VERSION}.tar.gz DESTINATION ${CMAKE_BINARY_DIR})

    target_include_directories(wivrn-common SYSTEM PUBLIC ${CMAKE_BINARY_DIR}/Vulkan-Headers-vulkan-sdk-${VULKAN_VERSION}/include)
    target_compile_definitions(wivrn-common PUBLIC VMA_VULKAN_VERSION=1001000)
else()
    target_link_libraries(wivrn-common PUBLIC Vulkan::Headers)
endif()

# XR_EXT_frame_synthesis auxiliary data transport, compiled in the client and
# server targets as it needs the OpenXR headers they provide
if (WIVRN_BUILD_CLIENT OR WIVRN_BUILD_SERVER)
    if (WIVRN_USE_SYSTEM_ZSTD AND NOT ANDROID)
        pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET GLOBAL libzstd)
        set(WIVRN_ZSTD_TARGET PkgConfig::ZSTD)
    else()
        set(ZSTD_BUILD_PROGRAMS OFF)
        set(ZSTD_BUILD_TESTS OFF)
        set(ZSTD_BUILD_SHARED OFF)
        set(ZSTD_BUILD_STATIC ON)
        set(ZSTD_LEGACY_SUPPORT OFF)
        set(ZSTD_MULTITHREAD_SUPPORT OFF)
        FetchContent_MakeAvailable(zstd)
        set(WIVRN_ZSTD_TARGET libzstd_static)
    endif()

    add_library(wivrn-frame-synthesis INTERFACE)
    target_sources(wivrn-frame-synthesis INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/frame_synthesis/annexb_sei.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/frame_synthesis/aux_codec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/frame_synthesis/aux_frame.cpp
    )
    target_link_libraries(wivrn-frame-synthesis INTERFACE ${WIVRN_ZSTD_TARGET})
endif()

target_include_directories(wivrn-common-base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wivrn-common PUBLIC Boost::pfr OpenSSL::Crypto wivrn-external wivrn-common-base)
target_compile_definitions(wivrn-common PUBLIC VULKAN_HPP_NO_STRUCT_CONSTRUCTORS)
