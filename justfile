build_dir := "build"
build_type := "Debug"
jobs := `nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4`

# List available recipes
default:
    @just --list

# Configure the CMake build
configure *FLAGS:
    cmake -S . -B {{build_dir}} \
        -G Ninja \
        -DCMAKE_BUILD_TYPE={{build_type}} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        {{FLAGS}}

# Build the project
build *TARGETS:
    cmake --build {{build_dir}} -j{{jobs}} {{TARGETS}}

# Configure + build in one step
all *FLAGS: (configure FLAGS) build

# Run the test suite
test *ARGS:
    just configure -DSAMMINE_TEST=ON
    just build
    cd {{build_dir}} && ctest --output-on-failure {{ARGS}}

# Clean the build directory
clean:
    rm -rf {{build_dir}}

# Run the compiler on a file
run FILE *ARGS:
    {{build_dir}}/bin/sammine -f {{FILE}} {{ARGS}}

# Configure a release build
release *FLAGS:
    just build_type=Release configure {{FLAGS}}

# Build with sanitizers
sanitize *FLAGS:
    just configure -DSAMMINE_SANITIZE=ON {{FLAGS}}
    just build

# Build with Tracy profiling
tracy *FLAGS:
    just configure -DSAMMINE_TRACY=ON {{FLAGS}}
    just build

# Symlink compile_commands.json to project root for LSP
compdb:
    ln -sf {{build_dir}}/compile_commands.json .
