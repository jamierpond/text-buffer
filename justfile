# text-buffer dev commands. Requires just, cmake, ninja and a C++20 toolchain.

set windows-shell := ["cmd.exe", "/c"]

build_dir := "build"
build_type := "Release"

default:
    @just --list

[doc('Configure + build the libraries and their tests')]
build *cmake_args:
    cmake -S . -B {{build_dir}} -G Ninja -DCMAKE_BUILD_TYPE={{build_type}} {{cmake_args}}
    cmake --build {{build_dir}}

[doc('Build, then run the tests')]
test: build
    ctest --test-dir {{build_dir}} --output-on-failure

[doc('Move branch-pinned deps to their branch heads and rewrite lockstep.lock')]
update-deps:
    cmake -S . -B {{build_dir}} -G Ninja -DCMAKE_BUILD_TYPE={{build_type}} -DLOCKSTEP_MODE=update
