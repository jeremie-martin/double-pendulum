CC := clang++
CFLAGS := -std=c++20 -Wall -Wextra -Ofast
LDFLAGS := -pthread -lm -fuse-ld=lld

SRCDIR := src
OBJDIR := obj

SOURCES := $(wildcard $(SRCDIR)/*.cpp)
OBJECTS := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SOURCES))
EXECUTABLE := pendulum

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	@echo "Linking $@"
	$(CC) $(OBJECTS) $(CFLAGS) $(LDFLAGS) -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR)/* $(EXECUTABLE)

.PHONY: all clean
