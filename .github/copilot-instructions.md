# Copilot / AI Agent Instructions for FoxSYN

This file gives concise, actionable guidance for AI coding agents to be productive in this C++ repository.

Summary
- Project: FoxSYN — C++ FPGA/logic-mapping toolset built on top of the ABC framework.
- Build system: CMake via top-level `Makefile` targets (`release`, `debug`, `asan`, `clangd`).

Quick start (reproducible builds)
- Build release: `make release` (invokes `cmake ../src` in `release` and `make -j8`).
- Build debug: `make debug`.
- ASan build: `make asan` (uses Address and Undefined Behavior sanitizers).
- Clang build (clang-20): `make clangd` (CMake option `-DUSE_CLANG20=ON`).
- Note: CMake produces build dirs `release`, `debug`, `asan`, `build_clang` with `compile_commands.json`.

High-level architecture (where to look)
- Integration with ABC: `src/main.cpp` registers ABC commands (`foxmap`, `smap`) and calls `Abc_RealMain`.
- Core mapper implementations:
  - `src/fox/` — `foxmap` implementation (cut enumeration, Param, Cut, Node, FoxMap classes).
  - `src/supper/` — alternate mapper and `smap` implementation (classes: `mapper`, `Config`, `CutCost`).
  - `src/abc/` — ABC glue and library code; the project links `libabc` (see `src/CMakeLists.txt`).
- Tests / utilities: `test_dot.cpp` and `test_verilog.cpp` (built as `test_dot` and `test_verilog` executables).

Important project-specific conventions
- Command registration: user-facing functionality is exposed by adding commands to ABC via `Cmd_CommandAdd` in `main.cpp`.
- Command-line flags: parse conventions follow `main.cpp` handlers (examples: `foxmap` supports `-k`, `-F`, `-E`, `-a`, `-v`; `smap` supports `-d <dot_file>`).
- LUT size and cut limits: constants such as `kMaxLutSize = 6` and `kMaxCutNum = 16` are central — many algorithms assume these bounds.
- Memory ownership: code uses manual allocation patterns (e.g., `new[]`/`delete[]`, custom `Cut::copy`/`Cut::dealloc`) — be conservative when refactoring ownership.
- Debug guards: conditional code paths rely on macros like `kDebugBuild` and heavy use of `Assert()` macros — keep assertions intact during changes.

Build-time and environment notes
- `src/CMakeLists.txt` adds `include_directories(/home/longfei/taskflow)` — this external path must exist in developer environments or be adjusted.
- The CMake option `USE_CLANG20` toggles clang-20 specific options and is used by `make clangd`.
- Link targets: executable `FoxSYN` links `libabc`, `fox`, and `supper` libraries (see `src/CMakeLists.txt`).

Where to make common changes
- Add new ABC commands: modify `src/main.cpp` to register and export the command handler.
- Add mapping algorithms or change heuristics: `src/fox/*` and `src/supper/*` (cut enumeration, cost functions, mapper internals).
- Tests or output routines (DOT / Verilog): `test_dot.cpp`, `test_verilog.cpp`, and `src/supper`'s `mapper->to_dot`/export functions.

Patterns & pitfalls for PRs
- Keep changes localized to mapper or ABC glue — avoid touching `abc/` unless necessary.
- Maintain ABI of command handlers: they are C-style functions called by ABC; signatures must remain `int Cmd(Abc_Frame_t*, int, char**)`.
- Respect static limits and array sizes (`kMaxLutSize`, `kMaxCutNum`, `kMaxId`). Changing these may require widespread updates.
- Watch for absolute paths (`/home/longfei/taskflow`) and duplicated Makefile targets (there are two `clangd` targets) — cleanups are welcome but validate builds across all Makefile targets.

Useful places to inspect when diagnosing bugs
- `src/main.cpp` — command parsing and ABC integration.
- `src/fox/foxmap.hpp` and corresponding .cpp files — cut management, Param, Cut, Node.
- `src/supper/map.hpp` and corresponding .cpp files — `mapper` implementation and Agdmap helpers.
- `src/CMakeLists.txt` and top-level `Makefile` — build flow and important CMake options.
- `regression/` — scripts and example circuits used for regression testing.

Example guidance for code generation tasks
- When adding a new CLI flag, mirror parsing style from `Foxmap_Command` and update the usage block printed via `Abc_Print`.
- When changing cost heuristics, reference `CutCost::GetRankFn`-style functions in `supper` and `RankFnSet` in `fox` for examples.
- For parallelism changes, follow existing use of atomics and `std::atomic` in `src/supper/map.hpp` (e.g., `_id_counter`).

If anything above is ambiguous or you need examples of specific functions to reference, tell me which area (e.g., `foxmap` cut enumeration or `supper` mapper API) and I'll expand with code snippets and exact file links.
