CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra
# -MMD -MP emit a .d file per object listing the headers it included, pulled in
# via `-include $(DEPS)` below. Without it, editing a header rebuilds nothing:
# objects compiled against the old declarations get linked with new ones, and a
# changed class layout turns into stack corruption at runtime, not a link error.
DEPFLAGS := -MMD -MP

OBJ_DIR := obj
BIN_DIR := bin

TARGET := $(BIN_DIR)/loracom
SRCS := ArgParser.cpp BasicUart.cpp LoRaCom.cpp main.cpp
OBJS := $(SRCS:%.cpp=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

-include $(DEPS)
