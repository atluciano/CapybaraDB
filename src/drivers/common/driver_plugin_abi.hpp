#pragma once

#include "drivers/common/catalog_provider.hpp"
#include "drivers/common/db_driver.hpp"

#include <cstdint>

#ifdef _WIN32
#define CAPYBARADB_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define CAPYBARADB_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace capybaradb::drivers {

inline constexpr uint32_t kDriverPluginAbiVersion = 1u;

using FnDriverAbiVersion       = uint32_t (*)();
using FnCreateDriver           = DbDriver* (*)();
using FnDestroyDriver          = void (*)(DbDriver*);
using FnCreateCatalogProvider  = CatalogProvider* (*)();
using FnDestroyCatalogProvider = void (*)(CatalogProvider*);
using FnShutdownPlugin         = void (*)();

} // namespace capybaradb::drivers
