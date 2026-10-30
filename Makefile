VCPKG_ROOT ?= D:/vcpkg
BUILD_DIR ?= build
TOOLCHAIN_FILE := $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake
PRODUCER_EXE := $(BUILD_DIR)/Debug/producer.exe

.PHONY: configure build run local clean

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE)

build: configure
	cmake --build $(BUILD_DIR) --config Debug --target producer

run: build
	set "PATH=$(VCPKG_ROOT)/installed/x64-windows/debug/bin;%PATH%" && "$(PRODUCER_EXE)"

local: run

clean:
	cmake --build $(BUILD_DIR) --config Debug --target clean
