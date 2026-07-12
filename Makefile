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

# AddressSanitizer + UBSan
asan:
	@mkdir -p asan && \
	(cd asan && cmake -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
		-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
		-DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../src) && \
	(cd asan && make -j8)

clean:
	@rm -rf release debug asan

.PHONY: release debug asan clean
