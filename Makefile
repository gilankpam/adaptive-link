# Adaptive-Link Root Makefile
# Delegates to sub-project Makefiles

.PHONY: all clean test test-c test-python drone ground-station

all: drone

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