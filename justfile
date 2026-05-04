build_dir := "build"
build_type := "Debug"
llvm_build_dir := "externals/llvm-project/build"
total_cpus := `python3 -c "import os; print(os.cpu_count() or 4)"`
jobs := `python3 -c "import os; n = os.cpu_count() or 4; print(max(4, n - 4))"`

# Fetch submodules (LLVM, StableHLO)
init:
    git submodule update --init --recursive

# Configure and build LLVM/MLIR from submodule
llvm:
    cmake -S externals/llvm-project/llvm -B {{llvm_build_dir}} -G Ninja \
        -DLLVM_ENABLE_ASSERTIONS=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_TARGETS_TO_BUILD="X86;AArch64;NVPTX" \
        -DLLVM_CCACHE_BUILD=true \
        -DLLVM_USE_LINKER=mold \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DLLVM_ENABLE_PROJECTS="mlir;llvm" \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++
    cmake --build {{llvm_build_dir}} -j{{jobs}}

# Configure the CMake build
configure *FLAGS:
    cmake -S . -B {{build_dir}} \
        -G Ninja \
        -DCMAKE_BUILD_TYPE={{build_type}} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DLLVM_DIR={{llvm_build_dir}}/lib/cmake/llvm \
        -DMLIR_DIR={{llvm_build_dir}}/lib/cmake/mlir \
        -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=mold" \
        -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=mold" \
        -DSAMMINE_TEST=ON \
        {{FLAGS}}

# Build the project
build:
    cmake --build {{build_dir}} -j{{jobs}}

alias b := build

# Run the test suite
test:
    cmake --build {{build_dir}} -j{{jobs}} --target unit-tests e2e-tests

# Clean the build directory
clean:
    rm -rf {{build_dir}}

# Run the compiler on a file
run FILE *ARGS:
    {{build_dir}}/bin/sammine -f {{FILE}} {{ARGS}}

# Symlink compile_commands.json to project root for LSP
compdb:
    ln -sf {{build_dir}}/compile_commands.json .
