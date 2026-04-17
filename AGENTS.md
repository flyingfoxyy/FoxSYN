# Repository Guidelines

## Project Structure & Module Organization
`src/` contains the active codebase. `src/main.cpp` builds the `FoxSYN` executable and registers ABC commands. Core modules live in `src/fox/`, `src/supper/`, and `src/partsyn/`, each with its own `CMakeLists.txt`. `src/test_dot.cpp` and `src/test_verilog.cpp` build small verification binaries for graph export paths. `src/abc/` is maintained as a separate subrepository so ABC can be updated from upstream while still carrying project-specific changes. Supporting notes live in `docs/`, while build trees are generated into `release/`, `debug/`, `asan/`, or `build_clang/`.

## Build, Test, and Development Commands
Use the top-level `Makefile` as the entry point. From `/home/longfei/FoxSYN`, run:

- `make` or `make release`: build the release configuration in `release/`.
- `make debug`: build a debug configuration in `debug/`.
- `make asan`: build with AddressSanitizer and UBSan enabled.
- `make clangd`: generate a Clang-based build with `compile_commands.json` for editor tooling.
- `make clean`: remove generated `release/` and `debug/` directories.

The project expects Taskflow headers at `/home/longfei/taskflow`. Clang builds also require `clang-20` and `clang++-20`.

## Coding Style & Naming Conventions
Match the surrounding C++ style instead of reformatting unrelated code. In practice this means 4-space indentation, braces on their own line in implementation files, and standard library includes before project headers when practical. Use `PascalCase` for types (`Param`, `Config`), `snake_case` for namespaces and most variables (`foxmap`, `opt_rounds`), and keep file names lowercase. No active linter is checked in, and the repository’s `.clang-format` is empty, so keep formatting changes narrow and consistent.

## Testing Guidelines
There is no dedicated unit test framework yet. Validate changes by building with `make`, then run the generated helper binaries from the build tree when relevant, for example `./release/test_dot` or `./debug/test_verilog`. Name new lightweight test drivers `src/test_<feature>.cpp` and wire them into `src/CMakeLists.txt`.

## Commit & Pull Request Guidelines
Recent history favors short, imperative commit subjects such as `Add partsyn`, `Fix bug`, and `Support multiple pass run for agdmap`. Keep commit messages neither too short nor too long, and use the subject to explain the reason for the change. Pull requests should summarize the affected module, list the build/test commands you ran, and include sample command output or screenshots only when user-facing behavior changed.
