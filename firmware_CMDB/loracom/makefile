CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra

OBJ_DIR := obj
BIN_DIR := bin

TARGET := $(BIN_DIR)/loracom
SRCS := ArgParser.cpp BasicUart.cpp LoRaCom.cpp main.cpp
OBJS := $(SRCS:%.cpp=$(OBJ_DIR)/%.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
