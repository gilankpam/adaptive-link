# Contributing to Adaptive-Link

Thank you for your interest in contributing to Adaptive-Link! This document outlines how to contribute to the project.

## Project Overview

Adaptive-Link is an adaptive wireless link profile selector for OpenIPC FPV drone systems. It dynamically adjusts video bitrate, MCS (modulation/coding scheme), FEC (forward error correction), TX power, and other transmission parameters based on real-time signal quality from the ground station.

## Development Environment

### Prerequisites

- **C Compiler**: GCC or Clang with C99 support
- **Python**: Python 3.7+
- **Build Tools**: GNU Make
- **Git**: For version control

### Building the Project

```bash
# Build drone daemon
make

# Build and run tests
make test

# Clean build artifacts
make clean
```

### Project Structure

```
adaptive-link/
├── drone/              # C daemon source code
│   └── src/            # Source files
├── ground-station/     # Python ground station script
├── config/             # Configuration templates
├── profiles/           # TX profile presets
├── scripts/            # Installation and utility scripts
├── test/               # Test files
│   ├── c/              # C unit tests
│   └── python/         # Python unit tests
└── docs/               # Documentation
```

## Making Changes

### 1. Drone Daemon (C)

The drone daemon is written in C99 with pthreads. When making changes:

- Follow existing coding style
- Add comments for complex logic
- Update `docs/ARCHITECTURE.md` if adding new components
- Test on actual hardware when possible

### 2. Ground Station (Python)

The ground station is a Python 3 script. When making changes:

- Keep the script monolithic for deployment simplicity
- Use only standard library modules
- Add docstrings for functions
- Test with mock wfb-ng data

### 3. Configuration Files

Configuration templates are in `config/`. When adding new options:

- Add documentation in the config file itself
- Update `docs/ARCHITECTURE.md` configuration reference
- Provide sensible defaults

### 4. TX Profiles

Profile presets are in `profiles/`. When creating new profiles:

- Use descriptive filenames (e.g., `safe-9mbps.conf`)
- Follow the existing format
- Document the intended use case in comments

## Testing

### C Tests

```bash
make test-c
```

C tests use the Unity test framework. Add new tests in `test/c/`.

### Python Tests

```bash
make test-python
```

Python tests use unittest. Add new tests in `test/python/`.

## Documentation

- **README.md**: Project overview and quick start
- **docs/ARCHITECTURE.md**: Technical architecture details
- **docs/FLOW.md**: Data flow documentation
- **docs/TUNING_GUIDE.md**: Configuration tuning guide

## Pull Request Process

1. Create a feature branch from main
2. Make your changes
3. Test thoroughly
4. Update documentation as needed
5. Submit a pull request with a clear description

## Code Style

### C Code

- Use C99 standard
- Follow existing naming conventions
- Use meaningful variable names
- Add comments for non-obvious logic

### Python Code

- Follow PEP 8 style guide
- Use meaningful variable names
- Add docstrings for public functions

## Questions?

For questions or discussions, please open an issue on GitHub.