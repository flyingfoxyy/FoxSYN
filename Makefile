# Default build type
.DEFAULT_GOAL := release

# Release
release:
	@mkdir -p release && \
	(cd release && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../src) && \
	(cd release && make -j8)

# Debug
debug:
	@mkdir -p debug && \
	(cd debug && cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../src) && \
	(cd debug && make -j8)

# ASan
asan:
	@mkdir -p asan && \
	(cd asan && cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer" ../src) && \
	(cd asan && make -j8)

clangd:
	@mkdir -p build_clang && \
	(cd build_clang && cmake -DUSE_CLANG20=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../src) && \
	(cd build_clang && make -j8)

clangd:
	@mkdir -p build_clang && \
	(cd build_clang && cmake -DUSE_CLANG20=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../src) && \
	(cd build_clang && make -j8)

clean:
	@rm -rf release debug

.PHONY: release debug clean
