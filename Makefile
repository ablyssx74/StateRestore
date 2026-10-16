# StateRestore: build on Haiku with `make`.
TARGET = StateRestore
CXX ?= g++
CXXFLAGS = -Wall -O2 -std=c++11
LIBS = -lbe

all: $(TARGET)

$(TARGET): StateRestore.cpp StateRestore.rsrc
	$(CXX) $(CXXFLAGS) -o $(TARGET) StateRestore.cpp $(LIBS)
	xres -o $(TARGET) StateRestore.rsrc
	mimeset -f $(TARGET)

StateRestore.rsrc: StateRestore.rdef
	rc -o $@ $<

clean:
	rm -f $(TARGET) StateRestore.rsrc

.PHONY: all clean
