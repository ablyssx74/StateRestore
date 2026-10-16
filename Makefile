# StateRestore: build on Haiku with `make`, and make the package with `make package`.
# The one place a release changes is the VERSION file.
TARGET = StateRestore
CXXFLAGS = -Wall -O2 -std=c++11
LIBS = -lbe

# On 32-bit Haiku the program is built with the newer secondary-architecture compiler, as Cricket is, against the libraries
# in /boot/system/lib/x86 (the gcc 2 compiler cannot build C++11).
ifeq ($(shell uname -m), BePC)
	CXX = g++-x86
	LDFLAGS = -L/boot/system/lib/x86
else
	CXX = g++
	LDFLAGS =
endif

VERSION := $(shell sed -n 's/.*\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' VERSION | head -1)
V_MAJOR := $(word 1,$(subst ., ,$(VERSION)))
V_MIDDLE := $(word 2,$(subst ., ,$(VERSION)))
V_MINOR := $(word 3,$(subst ., ,$(VERSION)))
REVISION ?= 1
ARCH := $(shell uname -m | sed 's/BePC/x86_gcc2/')
PACKAGE_DIR = build/package
PACKAGE = staterestore-$(VERSION)-$(REVISION)-$(ARCH).hpkg

all: $(TARGET)

$(TARGET): StateRestore.cpp StateRestore.rsrc
	$(CXX) $(CXXFLAGS) -DSTATERESTORE_VERSION=\"$(VERSION)\" $(LDFLAGS) -o $(TARGET) StateRestore.cpp $(LIBS)
	xres -o $(TARGET) StateRestore.rsrc
	mimeset -f $(TARGET)

# the version fields are filled in from VERSION
StateRestore.rsrc: StateRestore.rdef VERSION
	sed -e 's/@V_MAJOR@/$(V_MAJOR)/' -e 's/@V_MIDDLE@/$(V_MIDDLE)/' -e 's/@V_MINOR@/$(V_MINOR)/' \
		-e 's/@VERSION@/$(VERSION)/' $< > StateRestore.version.rdef
	rc -o $@ StateRestore.version.rdef

package: all
	rm -rf $(PACKAGE_DIR)
	mkdir -p $(PACKAGE_DIR)/apps $(PACKAGE_DIR)/bin $(PACKAGE_DIR)/data/deskbar/menu/Applications \
		$(PACKAGE_DIR)/documentation/packages/staterestore
	cp $(TARGET) $(PACKAGE_DIR)/apps/$(TARGET)
	ln -s ../apps/$(TARGET) $(PACKAGE_DIR)/bin/$(TARGET)
	ln -s ../../../../apps/$(TARGET) $(PACKAGE_DIR)/data/deskbar/menu/Applications/$(TARGET)
	cp LICENSE README.md $(PACKAGE_DIR)/documentation/packages/staterestore/
	sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@REVISION@/$(REVISION)/g' -e 's/@YEAR@/$(shell date +%Y)/' \
		-e 's/^architecture.*/architecture	$(ARCH)/' packaging/staterestore.tpl > $(PACKAGE_DIR)/.PackageInfo
	rm -f build/$(PACKAGE)
	package create -C $(PACKAGE_DIR) build/$(PACKAGE)
	@echo "built build/$(PACKAGE)"

clean:
	rm -rf $(TARGET) StateRestore.rsrc StateRestore.version.rdef build

.PHONY: all package clean
