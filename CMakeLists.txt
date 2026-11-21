cmake_minimum_required(VERSION 3.28)

project(capybaradb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CAPYBARADB_ENABLE_SANITIZERS "Enable ASan/UBSan on GCC/Clang" OFF)
option(CAPYBARADB_BUILD_TESTS "Build unit and render tests" ON)

include(CTest)
include(cmake/CompilerWarnings.cmake)
include(cmake/FindODBCNative.cmake)
include(cmake/DeployQt.cmake)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg Test)
find_package(qt6advanceddocking CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(Freetype REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(nanobench CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# capybaradb_core — Pure C++23 core library (MUST NOT include any Qt headers)
# ---------------------------------------------------------------------------
add_library(capybaradb_core STATIC
    src/core/arena/arena_allocator.hpp
    src/core/arena/arena_allocator.cpp
    src/core/result/cell_desc.hpp
    src/core/result/result_set_storage.hpp
    src/core/result/result_set_storage.cpp
    src/core/result/result_formatter.hpp
    src/core/result/result_formatter.cpp
    src/core/result/result_exporter.hpp
    src/core/result/result_exporter.cpp
    src/core/plan/execution_plan.hpp
    src/core/plan/execution_plan.cpp
    src/core/completion/accent_fold.hpp
    src/core/completion/accent_fold.cpp
    src/core/completion/sql_completion.hpp
    src/core/completion/sql_completion.cpp
    src/core/config/state_paths.hpp
    src/core/config/state_paths.cpp
    src/core/config/file_util.hpp
    src/core/config/file_util.cpp
    src/core/config/utf_convert.hpp
    src/core/config/query_options.hpp
    src/core/config/environment_options.hpp
    src/core/config/environment_options.cpp
    src/core/config/connection_profile.hpp
    src/core/config/connection_profile.cpp
    src/core/config/connection_uri_parser.hpp
    src/core/config/connection_uri_parser.cpp
    src/core/config/query_history.hpp
    src/core/config/query_history.cpp
    src/core/config/query_recovery.hpp
    src/core/config/query_recovery.cpp
    src/core/threading/cancel_token.hpp
    src/core/threading/catalog_worker_pool.hpp
    src/core/threading/catalog_worker_pool.cpp
    src/core/threading/stream_sink.hpp
    src/core/threading/db_session_worker.hpp
    src/core/threading/db_session_worker.cpp
    src/dialects/sql_dialect.hpp
    src/dialects/dialect_lex_utils.hpp
    src/dialects/postgres_dialect.hpp
    src/dialects/postgres_dialect.cpp
    src/dialects/tsql_dialect.hpp
    src/dialects/tsql_dialect.cpp
    src/dialects/sqlite_dialect.hpp
    src/dialects/sqlite_dialect.cpp
    src/dialects/sql_placeholder_scanner.hpp
    src/dialects/sql_placeholder_scanner.cpp
    src/drivers/common/db_types.hpp
    src/drivers/common/driver_ui_descriptor.hpp
    src/drivers/common/db_driver.hpp
    src/drivers/common/catalog_provider.hpp
    src/drivers/common/driver_plugin_abi.hpp
    src/drivers/common/driver_registry.hpp
    src/drivers/common/driver_registry.cpp
    src/drivers/postgres/pg_type_oids.hpp
    src/drivers/mssql/mssql_batch_splitter.hpp
    src/drivers/mssql/mssql_batch_splitter.cpp
    src/drivers/mssql/mssql_type_utils.hpp
)
set_target_properties(capybaradb_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
if(WIN32)
    target_sources(capybaradb_core PRIVATE src/core/config/secret_store_win.cpp)
else()
    target_sources(capybaradb_core PRIVATE src/core/config/secret_store_linux.cpp)
endif()
target_include_directories(capybaradb_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(capybaradb_core PUBLIC
    nlohmann_json::nlohmann_json
)
if(WIN32)
    target_link_libraries(capybaradb_core PRIVATE shell32 ole32 crypt32 advapi32)
else()
    target_link_libraries(capybaradb_core PRIVATE PkgConfig::LIBSECRET ${CMAKE_DL_LIBS})
endif()
capybaradb_set_compiler_warnings(capybaradb_core)
capybaradb_enable_sanitizers_if_requested(capybaradb_core)

# ---------------------------------------------------------------------------
# Dynamic Driver Plugins (lazy-loaded via DriverRegistry)
# ---------------------------------------------------------------------------
add_library(capybaradb_driver_postgres SHARED
    src/drivers/postgres/pg_driver.hpp
    src/drivers/postgres/pg_driver.cpp
    src/drivers/postgres/pg_catalog_provider.hpp
    src/drivers/postgres/pg_catalog_provider.cpp
    src/drivers/postgres/pg_plugin.cpp
)
target_link_libraries(capybaradb_driver_postgres PRIVATE
    capybaradb_core
    PostgreSQL::PostgreSQL
)
if(WIN32)
    target_link_libraries(capybaradb_driver_postgres PRIVATE ws2_32)
endif()
capybaradb_set_compiler_warnings(capybaradb_driver_postgres)
capybaradb_enable_sanitizers_if_requested(capybaradb_driver_postgres)
capybaradb_deploy_runtime_dlls(capybaradb_driver_postgres)

add_library(capybaradb_driver_mssql SHARED
    src/drivers/mssql/mssql_driver.hpp
    src/drivers/mssql/mssql_driver.cpp
    src/drivers/mssql/mssql_catalog_provider.hpp
    src/drivers/mssql/mssql_catalog_provider.cpp
    src/drivers/mssql/mssql_plugin.cpp
)
target_link_libraries(capybaradb_driver_mssql PRIVATE
    capybaradb_core
    ODBC::ODBC
)
capybaradb_set_compiler_warnings(capybaradb_driver_mssql)
capybaradb_enable_sanitizers_if_requested(capybaradb_driver_mssql)
capybaradb_deploy_runtime_dlls(capybaradb_driver_mssql)

add_library(capybaradb_driver_sqlite SHARED
    src/drivers/sqlite/sqlite_driver.hpp
    src/drivers/sqlite/sqlite_driver.cpp
    src/drivers/sqlite/sqlite_catalog_provider.hpp
    src/drivers/sqlite/sqlite_catalog_provider.cpp
    src/drivers/sqlite/sqlite_plugin.cpp
)
target_link_libraries(capybaradb_driver_sqlite PRIVATE
    capybaradb_core
    unofficial::sqlite3::sqlite3
)
capybaradb_set_compiler_warnings(capybaradb_driver_sqlite)
capybaradb_enable_sanitizers_if_requested(capybaradb_driver_sqlite)
capybaradb_deploy_runtime_dlls(capybaradb_driver_sqlite)

set(CAPYBARADB_DRIVER_PLUGINS
    capybaradb_driver_postgres
    capybaradb_driver_mssql
    capybaradb_driver_sqlite
)

# ---------------------------------------------------------------------------
# capybaradb_ui — Qt 6 Widgets + ADS desktop shell
# ---------------------------------------------------------------------------
add_library(capybaradb_ui STATIC
    src/ui/shell/theme_tokens.hpp
    src/ui/shell/theme_tokens.cpp
    src/ui/shell/icon_catalog.hpp
    src/ui/shell/icon_catalog.cpp
    src/ui/shell/shortcut_router.hpp
    src/ui/shell/shortcut_router.cpp
    src/ui/shell/main_window.hpp
    src/ui/shell/main_window.cpp
    src/ui/explorer/catalog_tree_model.hpp
    src/ui/explorer/catalog_tree_model.cpp
    src/ui/explorer/explorer_context_menu.hpp
    src/ui/explorer/explorer_context_menu.cpp
    src/ui/explorer/object_explorer_panel.hpp
    src/ui/explorer/object_explorer_panel.cpp
    src/ui/explorer/registered_servers_panel.hpp
    src/ui/explorer/registered_servers_panel.cpp
    src/ui/editor/sql_syntax_highlighter.hpp
    src/ui/editor/sql_syntax_highlighter.cpp
    src/ui/editor/sql_code_editor.hpp
    src/ui/editor/sql_code_editor.cpp
    src/ui/editor/completion_popup.hpp
    src/ui/editor/completion_popup.cpp
    src/ui/editor/editor_search_bars.hpp
    src/ui/editor/editor_search_bars.cpp
    src/ui/editor/query_tab_widget.hpp
    src/ui/editor/query_tab_widget.cpp
    src/ui/plan/execution_plan_widget.hpp
    src/ui/plan/execution_plan_widget.cpp
    src/ui/grid/column_metrics.hpp
    src/ui/grid/grid_selection_model.hpp
    src/ui/grid/grid_view_state.hpp
    src/ui/grid/glyph_atlas.hpp
    src/ui/grid/glyph_atlas.cpp
    src/ui/grid/grid_frame_builder.hpp
    src/ui/grid/grid_frame_builder.cpp
    src/ui/grid/software_grid_backend.hpp
    src/ui/grid/software_grid_backend.cpp
    src/ui/grid/result_grid_viewport.hpp
    src/ui/grid/result_grid_viewport.cpp
    src/ui/dialogs/connection_pages.hpp
    src/ui/dialogs/connection_pages.cpp
    src/ui/dialogs/profile_sidebar_widget.hpp
    src/ui/dialogs/profile_sidebar_widget.cpp
    src/ui/dialogs/connection_dialog.hpp
    src/ui/dialogs/connection_dialog.cpp
    src/ui/dialogs/export_flyout_widget.hpp
    src/ui/dialogs/export_flyout_widget.cpp
    src/ui/dialogs/filter_settings_dialog.hpp
    src/ui/dialogs/filter_settings_dialog.cpp
    src/ui/dialogs/options_dialog.hpp
    src/ui/dialogs/options_dialog.cpp
    src/ui/dialogs/query_history_dialog.hpp
    src/ui/dialogs/query_history_dialog.cpp
    src/ui/dialogs/query_options_dialog.hpp
    src/ui/dialogs/query_options_dialog.cpp
    assets/resources.qrc
)
set_target_properties(capybaradb_ui PROPERTIES
    AUTOMOC ON
    AUTORCC ON
)
target_include_directories(capybaradb_ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(capybaradb_ui PUBLIC
    capybaradb_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    ads::qt6advanceddocking
    Freetype::Freetype
    harfbuzz::harfbuzz
)
capybaradb_set_compiler_warnings(capybaradb_ui)
capybaradb_enable_sanitizers_if_requested(capybaradb_ui)

# ---------------------------------------------------------------------------
# capybaradb — Main executable
# ---------------------------------------------------------------------------
add_executable(capybaradb
    src/main.cpp
)
if(WIN32)
    set_target_properties(capybaradb PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_link_libraries(capybaradb PRIVATE
    capybaradb_ui
)
add_dependencies(capybaradb ${CAPYBARADB_DRIVER_PLUGINS})
capybaradb_set_compiler_warnings(capybaradb)
capybaradb_enable_sanitizers_if_requested(capybaradb)

capybaradb_deploy_windows_runtime(capybaradb)
capybaradb_configure_packaging(capybaradb)

# ---------------------------------------------------------------------------
# Automated Tests & Benchmarks
# ---------------------------------------------------------------------------
if(CAPYBARADB_BUILD_TESTS AND BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)

    add_executable(config_and_dialects_test
        tests/unit/config_and_dialects_test.cpp
    )
    target_link_libraries(config_and_dialects_test PRIVATE
        capybaradb_core
        Catch2::Catch2WithMain
    )
    capybaradb_set_compiler_warnings(config_and_dialects_test)
    capybaradb_enable_sanitizers_if_requested(config_and_dialects_test)
    capybaradb_deploy_runtime_dlls(config_and_dialects_test)

    add_test(NAME unit.config_and_dialects COMMAND config_and_dialects_test)

    add_executable(driver_and_catalog_test
        tests/integration/driver_and_catalog_test.cpp
    )
    target_link_libraries(driver_and_catalog_test PRIVATE
        capybaradb_core
        Catch2::Catch2WithMain
    )
    add_dependencies(driver_and_catalog_test ${CAPYBARADB_DRIVER_PLUGINS})
    capybaradb_set_compiler_warnings(driver_and_catalog_test)
    capybaradb_enable_sanitizers_if_requested(driver_and_catalog_test)
    capybaradb_deploy_runtime_dlls(driver_and_catalog_test)

    add_test(NAME integration.driver_and_catalog COMMAND driver_and_catalog_test)

    add_executable(shell_and_editor_test
        tests/render/shell_and_editor_test.cpp
    )
    set_target_properties(shell_and_editor_test PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(shell_and_editor_test PRIVATE
        capybaradb_ui
        Qt6::Test
    )
    capybaradb_set_compiler_warnings(shell_and_editor_test)
    capybaradb_enable_sanitizers_if_requested(shell_and_editor_test)
    capybaradb_deploy_windows_runtime(shell_and_editor_test)

    add_test(NAME render.shell_and_editor COMMAND shell_and_editor_test)
    set_tests_properties(render.shell_and_editor PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(arena_and_result_storage_test
        tests/unit/arena_and_result_storage_test.cpp
    )
    target_link_libraries(arena_and_result_storage_test PRIVATE
        capybaradb_core
        Catch2::Catch2WithMain
    )
    capybaradb_set_compiler_warnings(arena_and_result_storage_test)
    capybaradb_enable_sanitizers_if_requested(arena_and_result_storage_test)
    capybaradb_deploy_runtime_dlls(arena_and_result_storage_test)

    add_test(NAME unit.arena_and_result_storage COMMAND arena_and_result_storage_test)

    add_executable(result_grid_test
        tests/render/result_grid_test.cpp
    )
    set_target_properties(result_grid_test PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(result_grid_test PRIVATE
        capybaradb_ui
        Qt6::Test
    )
    capybaradb_set_compiler_warnings(result_grid_test)
    capybaradb_enable_sanitizers_if_requested(result_grid_test)
    capybaradb_deploy_windows_runtime(result_grid_test)

    add_test(NAME render.result_grid COMMAND result_grid_test)
    set_tests_properties(render.result_grid PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(query_workflow_and_plan_test
        tests/unit/query_workflow_and_plan_test.cpp
    )
    target_link_libraries(query_workflow_and_plan_test PRIVATE
        capybaradb_core
        Catch2::Catch2WithMain
    )
    capybaradb_set_compiler_warnings(query_workflow_and_plan_test)
    capybaradb_enable_sanitizers_if_requested(query_workflow_and_plan_test)
    capybaradb_deploy_runtime_dlls(query_workflow_and_plan_test)

    add_test(NAME unit.query_workflow_and_plan COMMAND query_workflow_and_plan_test)

    add_executable(export_plan_and_completion_ui_test
        tests/render/export_plan_and_completion_ui_test.cpp
    )
    set_target_properties(export_plan_and_completion_ui_test PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(export_plan_and_completion_ui_test PRIVATE
        capybaradb_ui
        Qt6::Test
    )
    capybaradb_set_compiler_warnings(export_plan_and_completion_ui_test)
    capybaradb_enable_sanitizers_if_requested(export_plan_and_completion_ui_test)
    capybaradb_deploy_windows_runtime(export_plan_and_completion_ui_test)

    add_test(NAME render.export_plan_and_completion_ui COMMAND export_plan_and_completion_ui_test)
    set_tests_properties(render.export_plan_and_completion_ui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(result_grid_render_benchmark
        tests/render/result_grid_render_benchmark.cpp
    )
    target_link_libraries(result_grid_render_benchmark PRIVATE
        capybaradb_ui
        nanobench::nanobench
    )
    capybaradb_set_compiler_warnings(result_grid_render_benchmark)
    capybaradb_enable_sanitizers_if_requested(result_grid_render_benchmark)
    capybaradb_deploy_windows_runtime(result_grid_render_benchmark)
endif()
