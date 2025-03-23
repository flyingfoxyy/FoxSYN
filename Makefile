# Default build type
.DEFAULT_GOAL := release

# Release
release:
	@mkdir -p release && \
	(cd release && cmake -DCMAKE_BUILD_TYPE=Release ../src) && \
	(cd release && make -j)

# Debug
debug:
	@mkdir -p debug && \
	(cd debug && cmake -DCMAKE_BUILD_TYPE=Debug ../src) && \
	(cd debug && make -j12)

clean:
	@rm -rf release debug

.PHONY: release debug clean