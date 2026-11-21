# =============================================================================
# cmake/DeployQt.cmake — Cross-platform Qt 6 runtime deployment & packaging
# =============================================================================

function(capybaradb_deploy_runtime_dlls target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E "$<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>,copy_if_different,true>"
                $<TARGET_RUNTIME_DLLS:${target}>
                "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>:$<TARGET_FILE_DIR:${target}>>"
            COMMAND_EXPAND_LISTS
        )
    endif()
endfunction()

function(capybaradb_deploy_windows_runtime target)
    if(WIN32)
        get_target_property(_qt_qmake Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)
        get_filename_component(_qt_root_dir "${_qt_bin_dir}" DIRECTORY)

        capybaradb_deploy_runtime_dlls(${target})

        set(_d "$<$<CONFIG:Debug>:d>")
        foreach(_pair
            "platforms:qwindows"
            "platforms:qoffscreen"
            "styles:qmodernwindowsstyle"
            "iconengines:qsvgicon"
            "imageformats:qsvg"
            "imageformats:qico"
        )
            string(REPLACE ":" ";" _parts "${_pair}")
            list(GET _parts 0 _subdir)
            list(GET _parts 1 _plugin)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/${_subdir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_qt_root_dir}/plugins/${_subdir}/${_plugin}${_d}.dll"
                    "$<TARGET_FILE_DIR:${target}>/${_subdir}/"
            )
        endforeach()
    endif()
endfunction()

function(capybaradb_configure_packaging target)
    include(GNUInstallDirs)

    set(_license_files
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/NOTICE"
        "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
    )

    if(WIN32)
        get_target_property(_qt_qmake Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)
        get_filename_component(_qt_root_dir "${_qt_bin_dir}" DIRECTORY)
        find_program(CAPYBARADB_WINDEPLOYQT_EXECUTABLE
            NAMES windeployqt windeployqt6
            HINTS "${_qt_bin_dir}"
        )

        set(_stage_dir "${CMAKE_BINARY_DIR}/stage/capybaradb-win-x64")
        set(_stage_exe "${_stage_dir}/$<TARGET_FILE_NAME:${target}>")
        set(_d "$<$<CONFIG:Debug>:d>")

        set(_stage_cmds
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${_stage_dir}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target}>" "${_stage_exe}"
            COMMAND ${CMAKE_COMMAND} -E "$<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>,copy_if_different,true>"
                $<TARGET_RUNTIME_DLLS:${target}>
                "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>:${_stage_dir}>"
        )

        foreach(_pair
            "platforms:qwindows"
            "platforms:qoffscreen"
            "styles:qmodernwindowsstyle"
            "iconengines:qsvgicon"
            "imageformats:qsvg"
            "imageformats:qico"
        )
            string(REPLACE ":" ";" _parts "${_pair}")
            list(GET _parts 0 _subdir)
            list(GET _parts 1 _plugin)
            list(APPEND _stage_cmds
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_stage_dir}/${_subdir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_qt_root_dir}/plugins/${_subdir}/${_plugin}${_d}.dll"
                    "${_stage_dir}/${_subdir}/"
            )
        endforeach()

        set(_extra_dlls
            "capybaradb_driver_postgres.dll"
            "capybaradb_driver_mssql.dll"
            "capybaradb_driver_sqlite.dll"
            "libpq.dll"
            "sqlite3.dll"
            "harfbuzz.dll"
            "libcrypto-3-x64.dll"
            "libssl-3-x64.dll"
            "brotlidec.dll"
            "brotlicommon.dll"
            "libpng16${_d}.dll"
            "bz2${_d}.dll"
            "libz${_d}.dll"
        )
        foreach(_dll ${_extra_dlls})
            list(APPEND _stage_cmds
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_BINARY_DIR}/${_dll}"
                    "${_stage_dir}/"
            )
        endforeach()

        foreach(_lic ${_license_files})
            if(EXISTS "${_lic}")
                list(APPEND _stage_cmds
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_lic}" "${_stage_dir}/"
                )
            endif()
        endforeach()

        add_custom_target(capybaradb_stage
            ${_stage_cmds}
            DEPENDS ${target} capybaradb_driver_postgres capybaradb_driver_mssql capybaradb_driver_sqlite
            COMMAND_EXPAND_LISTS
            COMMENT "Staging self-contained Windows native package into ${_stage_dir}"
        )

        install(TARGETS ${target} capybaradb_driver_postgres capybaradb_driver_mssql capybaradb_driver_sqlite
            RUNTIME DESTINATION "."
            LIBRARY DESTINATION "."
        )
        install(FILES ${_license_files} DESTINATION ".")

        set(CPACK_GENERATOR "ZIP")
        set(CPACK_PACKAGE_FILE_NAME "capybaradb-${PROJECT_VERSION}-win-x64")
    else()
        set(_stage_dir "${CMAKE_BINARY_DIR}/stage/capybaradb-linux-x64/AppDir")

        install(TARGETS ${target} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
        install(TARGETS capybaradb_driver_postgres capybaradb_driver_mssql capybaradb_driver_sqlite
            LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        )
        if(EXISTS "${CMAKE_SOURCE_DIR}/assets/capybaradb.desktop")
            install(FILES "${CMAKE_SOURCE_DIR}/assets/capybaradb.desktop"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
        endif()
        if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icons/server.svg")
            install(FILES "${CMAKE_SOURCE_DIR}/assets/icons/server.svg"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
                RENAME "capybaradb.svg")
        endif()
        install(FILES ${_license_files}
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/capybaradb")

        add_custom_target(capybaradb_stage
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${_stage_dir}"
            COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${_stage_dir}/usr"
            DEPENDS ${target}
            COMMENT "Staging Linux FHS/AppDir package into ${_stage_dir}"
        )

        set(CPACK_GENERATOR "TGZ")
        set(CPACK_PACKAGE_FILE_NAME "capybaradb-${PROJECT_VERSION}-linux-x64")
    endif()

    set(CPACK_PACKAGE_NAME "CapybaraDB")
    set(CPACK_PACKAGE_VENDOR "CapybaraDB Contributors")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "CapybaraDB")
    include(CPack)
endfunction()
