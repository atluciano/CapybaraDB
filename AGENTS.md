# AGENTS.md — CapybaraDB Engineering & Architecture Guide

This file defines the architectural rules, performance invariants, and coding conventions for **CapybaraDB**. Every contributor and coding agent must follow these principles so new features never degrade startup memory, runtime footprint, or UI responsiveness.

---

## 1. Core Engineering Philosophy

CapybaraDB is a native **C++23 + Qt 6 Widgets** database workbench built around **data-oriented, procedural, measurement-first engineering**:

1. **Measure real bytes and real microseconds — never guess.**
   Before optimizing or introducing new subsystems, inspect real OS memory metrics (`Working Set`, `Private Working Set`, `Private Commit`, loaded modules, thread count) and real latency distributions (`p50`/`p95`/`p99`).
2. **Design around data layout and hardware cost, not class hierarchies.**
   Optimize for contiguous memory, cache locality, minimal allocations, and predictable lifetimes.
3. **Pay only for what is actively used.**
   Startup must remain minimal. Heavy resources (database drivers, worker threads, glyph caches, large result arenas) must be initialized lazily and reclaimed when idle or closed.
4. **Never stall the UI thread.**
   The UI must stay responsive (0-lag scrolling, editing, and cancellation) even with **300+ columns** and **100,000+ to millions of rows**.

---

## 2. Hard Architectural Boundaries

### Layering (`CMakeLists.txt`)
- **`capybaradb_core` (static library):**
  - Pure C++23. **MUST NOT include any Qt headers.**
  - **MUST NOT link directly against database client libraries** (`libpq`, `OpenSSL`, `ODBC`, `sqlite3`).
- **Dynamic Driver Plugins (`capybaradb_driver_postgres`, `capybaradb_driver_mssql`, `capybaradb_driver_sqlite`):**
  - Built as separate shared libraries (`.dll` on Windows, `.so` on Linux) exporting the C ABI in `src/drivers/common/driver_plugin_abi.hpp`.
  - Loaded on demand via `src/drivers/common/driver_registry.cpp` (`LoadLibraryW`/`dlopen`) with atomic reference counting across active `DbDriver` and `CatalogProvider` instances, and unloaded when idle via `unload_unused_driver_modules()`.
  - Any OS-level driver discovery (e.g., `installed_odbc_drivers()`) **must read OS metadata directly** (Windows Registry / `odbcinst.ini`) without loading driver binaries.
- **`capybaradb_ui` (static library):**
  - Qt 6 Widgets + Qt Advanced Docking (`qt6advanceddocking`) + FreeType + HarfBuzz + CPU software grid renderer.
  - Keep 2D UI and grid rendering strictly on the CPU software path; do not pull in GPU rendering stacks or driver dependencies for 2D widgets.

---

## 3. Memory Management Rules

1. **Use `ArenaAllocator` for bulk and scratch data (`src/core/arena/arena_allocator.hpp`).**
   - Result sets, text slabs, and large transient buffers must use virtual-memory bump allocation (`VirtualAlloc` on Windows, `mmap` on Linux) rather than per-row or per-cell `new`/`malloc`.
   - Use `TempArena` / `ScopedTempArena` (`begin_temp()` / `end_temp()`) for `O(1)` scratch memory rewinds without unmapping hot pages.
   - Large arena teardowns (`>= 32 MiB`) must stay off the UI thread (handled via the async OS page reaper in `arena_allocator.cpp`).
2. **Avoid `std::shared_ptr` unless shared ownership is truly unavoidable.**
   - Prefer single-owner `std::unique_ptr`, stack/value ownership, non-owning pointers/references with clear lifetime hierarchies, or explicit atomic refcounts where needed (e.g., driver plugin module slots).
3. **Prefer flat, contiguous storage over node-based containers.**
   - Avoid `std::map`, `std::set`, `std::list`, `std::deque`, or `std::unordered_map` holding heap-allocated nested vectors on hot or large structures.
   - Follow the pattern in `SqlCompletionCatalog` (`src/core/completion/sql_completion.hpp`): store child elements contiguously in one flat array and index them with `(offset, count)` slices.
4. **Keep hot structs compact and trivially copyable (POD).**
   - Pack fields by alignment, use fixed-width integer types (`uint8_t`, `uint16_t`, `uint32_t`), and add `static_assert(std::is_trivially_copyable_v<T>)` (and `sizeof(T)` assertions where cache-line sizing matters, e.g., `LexicalState` at 64 bytes).
5. **Avoid heap-allocating type erasure on hot paths.**
   - Avoid `std::function` in high-frequency or synchronization primitives. Prefer raw function pointer + `void* ctx` or bounded inline storage (see `CancelToken`, `ScopedCancelCallback`, and `StreamPublishSink`).
6. **Share and grow caches on demand.**
   - Never duplicate large caches per tab or widget. `GlyphAtlasCache` shares atlases per DPR, starts small (`512×512`, 256 KB), and grows dynamically (`512 → 1024 → 2048`) only on overflow.
   - `IconCatalog` rasterizes for the active screen DPR (plus `1.0` fallback when HiDPI), not all possible DPRs eagerly.

---

## 4. Coding Style & Design Rules

1. **Prefer procedural free functions over stateless wrapper classes.**
   - Do not create `*Manager`, `*Service`, or `*Store` classes that only group functions or wrap a single path/config. Use free functions in the appropriate namespace operating on plain data structs (e.g., `scan_sql_line`, `is_sql_keyword`, `create_db_driver`, `complete_sql`).
2. **Prefer direct `switch` dispatch over virtual polymorphism on hot paths.**
   - Keep `virtual` interfaces strictly at real architectural boundaries (`DbDriver`, `CatalogProvider`, Qt widget overrides). For closed sets like SQL dialects or cell formatting, use `switch (engine)` / direct dispatch.
3. **No exceptions for control flow.**
   - Use `std::expected<T, E>`, `std::optional<T>`, or boolean/status returns, and mark non-throwing paths `noexcept`.
4. **Strict C++23 & Cross-Platform Discipline.**
   - Keep code warning-clean under MSVC, GCC, and Clang (`cmake/CompilerWarnings.cmake`).
   - Isolate OS-specific code cleanly behind `#ifdef _WIN32` / `#else` (Windows + Linux X11/Wayland). Never rely on Windows-only or Linux-only behavior in shared logic.

---

## 5. Concurrency, Threading & UI Rendering Rules

1. **Lazy-spawn background threads.**
   - Pools like `CatalogWorkerPool` must not spawn OS threads during application startup; start threads lazily on the first submitted task.
2. **Fixed-capacity queues and non-blocking cancellation.**
   - `DbSessionWorker` maintains 1 dedicated session thread per active query tab (for driver handle affinity), a pre-allocated ring buffer (`kRingCapacity = 64`), and a dedicated cancel dispatch thread so `cancel_active()` (`PQcancel` / `SQLCancelHandle`) never blocks the UI.
3. **Coalesce worker-to-UI notifications.**
   - Streaming rows from worker threads must go through `StreamPublishSink` (`std::atomic_flag` coalescing + milestone/heartbeat throttling) so at most **1** UI update is queued at any time.
4. **Grid rendering must stay `O(visible_rows * visible_cols)`.**
   - `GridFrameBuilder::build()` must always cull strictly to the visible viewport range in both axes. Never scan all rows or all columns during a frame paint.
   - Keep `SoftwareGridBackend::render()` tight: hoist raw pixel pointers and row strides outside loops, pre-clip source/destination bounds before inner pixel loops, and avoid integer divisions (`/ 255`) inside per-pixel blending.

---

## 6. Performance & Memory Budgets (Non-Negotiable Gates)

Changes must preserve or improve these verified targets (Windows x64 Release):

| Metric | Budget / Target |
| :--- | :--- |
| **Startup Private Working Set** (`--no-startup-dialog`) | **≤ 28 MB** *(current baseline ~19.7 MB)* |
| **Startup Private Commit** (`--no-startup-dialog`) | **≤ 60 MB** *(current baseline ~23.0 MB)* |
| **Startup Threads** (`--no-startup-dialog`) | **≤ 12** *(current baseline 9)* |
| **Database driver DLLs loaded at startup** | **0** (`libpq`, `libcrypto`, `libssl`, `sqlite3`, `odbc32`, `capybaradb_driver_*`) |
| **Grid frame time (300 cols × 100k rows, 1080p)** | **< 2.0 ms p99** |
| **Grid frame time (300 cols × 100k rows, 4K)** | **< 5.0 ms p99** |

---

## 7. Build, Test & Verification Workflow

Use the presets defined in `CMakePresets.json`:

```powershell
# Configure & build (Windows Release)
cmake --preset win-msvc-release
cmake --build --preset win-msvc-release

# Run full test suite (all tests must pass)
ctest --preset win-msvc-release --output-on-failure
```

On Linux:
```bash
cmake --preset linux-gcc-release   # or linux-gcc-debug (ASan + UBSan)
cmake --build --preset linux-gcc-release
ctest --preset linux-gcc-release --output-on-failure
```

### Required Checks Before Finishing Any Performance or Architecture Change
1. **Run `ctest`** — 100% of unit, integration, and render tests must pass.
2. **Check startup memory** when modifying `main.cpp`, `MainWindow`, `CMakeLists.txt`, drivers, icons, fonts, or thread pools — verify no forbidden DLLs were pulled into startup and `Private Working Set` / `Private Commit` stay within budget.
3. **Check driver lifecycle** when modifying `src/drivers/` — verify lazy-loading, zero startup load, and safe `Load → Use → Destroy → Unload → Reload` behavior (note: third-party libraries like OpenSSL/ODBC must be empirically tested against live TLS/ODBC connections before assuming reload safety).
4. **Check grid frame time** when modifying `src/ui/grid/` — ensure culling remains strictly viewport-bound and inner blitter loops introduce zero allocations or per-pixel branches/divisions.
