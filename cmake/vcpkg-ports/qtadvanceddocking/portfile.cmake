vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO githubuser0xFFFF/Qt-Advanced-Docking-System
    REF "${VERSION}"
    SHA512 ee78b1c7f6164b06ce9c193aa5dfa19281a1c894cd8a8cbcae6d137abc13417f32e0f2a05f9d91557e14ced91b3b541991065d0ee190ea5ad2623c3848674eaf
    HEAD_REF master
)

set(_ads_qt_options)
if(DEFINED ENV{Qt6_DIR} AND NOT "$ENV{Qt6_DIR}" STREQUAL "")
    list(APPEND _ads_qt_options "-DQt6_DIR=$ENV{Qt6_DIR}")
endif()
if(DEFINED ENV{CMAKE_PREFIX_PATH} AND NOT "$ENV{CMAKE_PREFIX_PATH}" STREQUAL "")
    list(APPEND _ads_qt_options "-DCMAKE_PREFIX_PATH=$ENV{CMAKE_PREFIX_PATH}")
elseif(DEFINED ENV{QT_ROOT_DIR} AND NOT "$ENV{QT_ROOT_DIR}" STREQUAL "")
    list(APPEND _ads_qt_options "-DCMAKE_PREFIX_PATH=$ENV{QT_ROOT_DIR}")
endif()

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" BUILD_STATIC)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${_ads_qt_options}
        -DBUILD_EXAMPLES=OFF
        -DADS_VERSION=${VERSION}
        -DQT_VERSION_MAJOR=6
        -DBUILD_STATIC=${BUILD_STATIC}
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME "qt6advanceddocking" CONFIG_PATH "lib/cmake/qt6advanceddocking")

vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/share/qt6advanceddocking/qt6advanceddockingConfig.cmake"
    "include(CMakeFindDependencyMacro)"
    [[include(CMakeFindDependencyMacro)
find_dependency(Qt6 COMPONENTS Core Gui Widgets)]]
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/license")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/license")

file(INSTALL "${SOURCE_PATH}/gnu-lgpl-v2.1.md" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
