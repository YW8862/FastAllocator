PROJECT := fastallocator

CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread -Iinclude

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin

SRC_FILES := $(wildcard src/*.cpp)
SRC_OBJS := $(patsubst src/%.cpp,$(OBJ_DIR)/src/%.o,$(SRC_FILES))

TEST_SIZE_CLASS_BIN := $(BIN_DIR)/test_size_class
TEST_ALLOCATOR_BIN := $(BIN_DIR)/test_allocator
BENCH_BIN := $(BIN_DIR)/bench_allocator

.PHONY: all test clean bench

all: $(TEST_SIZE_CLASS_BIN) $(TEST_ALLOCATOR_BIN) $(BENCH_BIN)

test: $(TEST_SIZE_CLASS_BIN) $(TEST_ALLOCATOR_BIN)
	$(TEST_SIZE_CLASS_BIN)
	$(TEST_ALLOCATOR_BIN)

bench: $(BENCH_BIN)

$(TEST_SIZE_CLASS_BIN): $(SRC_OBJS) test/test_size_class.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(TEST_ALLOCATOR_BIN): $(SRC_OBJS) test/test_allocator.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BENCH_BIN): $(SRC_OBJS) test/bench_allocator.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(OBJ_DIR)/src/%.o: src/%.cpp | $(OBJ_DIR)/src
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/src:
	mkdir -p $@

$(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
