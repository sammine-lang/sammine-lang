build_dir := "build"
release_dir := "build-release"
llvm_build_dir := "externals/llvm-project/build"
total_cpus := `python3 -c "import os; print(os.cpu_count() or 4)"`
jobs := `python3 -c "import os; n = os.cpu_count() or 4; print(max(4, n - 4))"`

default:
      @just --list
# Fetch submodules (LLVM)
init:
    git submodule update --init --recursive

# Configure and build LLVM/MLIR from submodule
llvm:
    cmake -S externals/llvm-project/llvm -B {{llvm_build_dir}} -G Ninja \
        -DLLVM_ENABLE_ASSERTIONS=ON \
        -DLLVM_INCLUDE_BENCHMARKS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_TARGETS_TO_BUILD="X86;AArch64;NVPTX" \
        -DMLIR_ENABLE_CUDA_RUNNER=ON \
        -DLLVM_CCACHE_BUILD=true \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DLLVM_ENABLE_PROJECTS="mlir;llvm" \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++
    cmake --build {{llvm_build_dir}} -j{{jobs}}

# Shared CMake configuration for both build channels
_configure dir type *FLAGS:
    cmake -S . -B {{dir}} \
        -G Ninja \
        -DCMAKE_BUILD_TYPE={{type}} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DLLVM_DIR={{llvm_build_dir}}/lib/cmake/llvm \
        -DMLIR_DIR={{llvm_build_dir}}/lib/cmake/mlir \
        -DSAMMINE_TEST=ON \
        {{FLAGS}}

# Configure the debug build
configure *FLAGS: (_configure build_dir "Debug" FLAGS)

# Configure the optimized release build
configure-release *FLAGS: (_configure release_dir "Release" FLAGS)

# Build the debug channel in build/
build: configure
    cmake --build {{build_dir}} -j{{jobs}}

# Build the release channel (optimized) in build-release/
release: configure-release
    cmake --build {{release_dir}} -j{{jobs}}

alias b := build

# Run the test suite
test:
    cmake --build {{build_dir}} -j{{jobs}} --target unit-tests e2e-tests

# Clean both build directories
clean:
    rm -rf {{build_dir}} {{release_dir}}

# Run the compiler on a file
run FILE *ARGS:
    {{build_dir}}/bin/sammine -f {{FILE}} {{ARGS}}

# Symlink compile_commands.json to project root for LSP
compdb:
    ln -sf {{build_dir}}/compile_commands.json .
