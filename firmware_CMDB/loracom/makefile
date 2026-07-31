OBJ_DIR := obj
BIN_DIR := bin

CXX := g++
# -I$(OBJ_DIR) so main.cpp can pick up the generated version.h.
CXXFLAGS := -std=c++20 -Wall -Wextra -I$(OBJ_DIR)
# -MMD -MP emit a .d file per object listing the headers it included, pulled in
# via `-include $(DEPS)` below. Without it, editing a header rebuilds nothing:
# objects compiled against the old declarations get linked with new ones, and a
# changed class layout turns into stack corruption at runtime, not a link error.
DEPFLAGS := -MMD -MP

TARGET := $(BIN_DIR)/loracom
SRCS := ArgParser.cpp BasicUart.cpp DeviceLock.cpp LoRaCom.cpp main.cpp
OBJS := $(SRCS:%.cpp=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

# The commit the binary was built from, reported by `loracom --help`.
# "unknown" covers building outside a checkout (e.g. from a source tarball).
VERSION_H := $(OBJ_DIR)/version.h
GIT_HASH := $(shell git -C $(CURDIR) rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
# Only loracom's own sources count as dirty - the marker answers "was this
# binary built from exactly the committed sources", and edits elsewhere in the
# repo (the install scripts, the HAT firmware) don't change what it was built
# from. Pathspecs are relative to this directory, so they cover loracom/ only.
GIT_DIRTY := $(shell git -C $(CURDIR) status --porcelain -- '*.cpp' '*.h' makefile 2>/dev/null | head -1)
GIT_VERSION := $(GIT_HASH)$(if $(GIT_DIRTY),-dirty,)

.PHONY: all clean FORCE

all: $(TARGET)

# Depends on FORCE so the hash is re-checked on every build, but the file is
# only rewritten when it actually changes - otherwise its mtime stays put and
# main.o isn't needlessly rebuilt. Without the FORCE, moving to a new commit
# without touching a source file would leave the old hash baked into the binary.
$(VERSION_H): FORCE | $(OBJ_DIR)
	@printf '#define LORACOM_VERSION "%s"\n' '$(GIT_VERSION)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

$(OBJ_DIR)/main.o: $(VERSION_H)

FORCE:

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

-include $(DEPS)
