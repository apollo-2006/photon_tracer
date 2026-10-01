CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -O3 -pthread -MMD -MP
INCLUDES = -I./include

SRC_DIR = src
OBJ_DIR = obj

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

TARGET = photon_tracer

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

-include $(OBJS:.o=.d)

# The GPU renderer (gpu/): mesh shaders and ray queries on Vulkan. Needs the
# Vulkan headers and loader, and glslc to compile the shaders, whose SPIR-V is
# built into the program.
GPU_SHADERS = $(wildcard gpu/shaders/*.task gpu/shaders/*.mesh gpu/shaders/*.frag gpu/shaders/*.comp)
GPU_SPIRV = $(patsubst gpu/shaders/%,$(OBJ_DIR)/shaders/%.inc,$(GPU_SHADERS))

$(OBJ_DIR)/shaders/%.inc: gpu/shaders/% gpu/shaders/common.glsl
	@mkdir -p $(OBJ_DIR)/shaders
	glslc --target-env=vulkan1.3 -O -mfmt=num -o $@ $<

photon_tracer_gpu: gpu/main.cpp gpu/vk.hpp $(GPU_SPIRV) $(wildcard include/*.hpp)
	$(CXX) $(CXXFLAGS) -Wno-missing-field-initializers $(INCLUDES) -Igpu -I$(OBJ_DIR)/shaders gpu/main.cpp -o $@ -lvulkan

gpu: photon_tracer_gpu

# The GPU renderer's images against the CPU renderer's references. Needs a GPU
# with mesh shaders and ray queries, so CI does not run it.
test-gpu: photon_tracer_gpu
	python3 tests/render_test.py check --gpu

# The demo's WebGPU tracer in a headless Chrome, against the same references.
# Needs web/dist (web/build.sh), node and google-chrome-stable, and a GPU.
test-webgpu:
	python3 tests/render_test.py check --webgpu

# Unit tests for the OBJ reader, the meshlet builder and the PNG decoder, then the render
# regression tests (tests/render_test.py).
tests/obj_test: tests/obj_test.cpp $(wildcard include/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

tests/meshlet_test: tests/meshlet_test.cpp $(wildcard include/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

tests/decode_image: tests/decode_image.cpp include/image_io.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

test: $(TARGET) tests/obj_test tests/meshlet_test tests/decode_image
	tests/obj_test
	tests/meshlet_test
	python3 tests/image_test.py
	python3 tests/render_test.py check

clean:
	rm -rf $(OBJ_DIR) $(TARGET) photon_tracer_gpu render.ppm tests/obj_test tests/meshlet_test tests/decode_image

.PHONY: all clean test gpu test-gpu test-webgpu