#include "drivers/common/driver_plugin_abi.hpp"
#include "drivers/sqlite/sqlite_catalog_provider.hpp"
#include "drivers/sqlite/sqlite_driver.hpp"

#include <sqlite3.h>

CAPYBARADB_PLUGIN_EXPORT uint32_t capybaradb_driver_abi_version() {
    return capybaradb::drivers::kDriverPluginAbiVersion;
}

CAPYBARADB_PLUGIN_EXPORT capybaradb::drivers::DbDriver* capybaradb_create_driver() {
    return new capybaradb::drivers::SqliteDriver();
}

CAPYBARADB_PLUGIN_EXPORT void capybaradb_destroy_driver(capybaradb::drivers::DbDriver* drv) {
    delete drv;
}

CAPYBARADB_PLUGIN_EXPORT capybaradb::drivers::CatalogProvider* capybaradb_create_catalog_provider() {
    return new capybaradb::drivers::SqliteCatalogProvider();
}

CAPYBARADB_PLUGIN_EXPORT void capybaradb_destroy_catalog_provider(capybaradb::drivers::CatalogProvider* cat) {
    delete cat;
}

CAPYBARADB_PLUGIN_EXPORT void capybaradb_shutdown_plugin() {
    (void)sqlite3_shutdown();
}
