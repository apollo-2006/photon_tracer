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
	rm -rf $(OBJ_DIR) $(TARGET) render.ppm tests/obj_test tests/meshlet_test tests/decode_image

.PHONY: all clean test