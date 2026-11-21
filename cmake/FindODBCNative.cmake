include_guard(GLOBAL)

if(NOT TARGET ODBC::ODBC)
    add_library(ODBC::ODBC INTERFACE IMPORTED)
    if(WIN32)
        target_link_libraries(ODBC::ODBC INTERFACE
            odbc32
            odbccp32
        )
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(UNIXODBC REQUIRED IMPORTED_TARGET odbc)
        pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
        target_link_libraries(ODBC::ODBC INTERFACE
            PkgConfig::UNIXODBC
        )
    endif()
endif()
