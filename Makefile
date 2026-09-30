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

# Render regression tests: see tests/render_test.py.
test: $(TARGET)
	python3 tests/render_test.py check

clean:
	rm -rf $(OBJ_DIR) $(TARGET) render.ppm

.PHONY: all clean test