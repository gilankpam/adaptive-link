# Adaptive-Link Root Makefile
# Delegates to sub-project Makefiles

.PHONY: all clean test test-c test-python drone ground-station ssc338q

all: drone

# Cross-compile for SSC338Q (ARM Cortex-A7)
ssc338q:
	$(MAKE) -C drone CC=armv7l-unknown-linux-musleabihf-gcc \
		OPT="-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -Os" \
		LDFLAGS="-lm -lpthread -static"

# Build drone daemon
drone:
	$(MAKE) -C drone

# Ground station is Python - no build needed
ground-station:
	@echo "Ground station is Python - no build required"
	@python3 -m py_compile ground-station/alink_gs

# Run all tests
test: test-c test-python

# Run C tests
test-c:
	$(MAKE) -C drone test

# Run Python tests
test-python:
	python3 -m unittest discover -s ground-station/test -v

# Clean all
clean:
	$(MAKE) -C drone clean
	rm -f drone/test/test_util