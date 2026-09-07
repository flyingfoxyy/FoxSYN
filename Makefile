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

# Test
test:
	@mkdir -p release && \
	(cd release && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ../src) && \
	(cd release && make -j8 test_dot test_verilog test_hop test_cpr test_csr2 test_csr3 test_csr4 test_fmpart test_hive) && \
	echo "\n=== Running Tests ===" && \
	for test in release/test_build/test_*; do \
		if [ -x "$$test" ]; then \
			echo "\n--- Running $$test ---"; \
			$$test || echo "FAILED: $$test"; \
		fi \
	done

clean:
	@rm -rf release debug asan

.PHONY: release debug asan test clean
