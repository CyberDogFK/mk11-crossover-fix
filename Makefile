CC      = x86_64-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -Wno-unused-parameter
LDFLAGS = -shared -static -s

VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)
DIST    = dist/mk11-crossover-face-fix-$(VERSION)

build/xinput9_1_0.dll: src/rgb32fix.c src/xinput9_1_0.def
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/rgb32fix.c src/xinput9_1_0.def

# Release archive: the DLL plus install/uninstall scripts, ready to attach to a GitHub release.
dist: build/xinput9_1_0.dll
	rm -rf $(DIST) $(DIST).zip
	mkdir -p $(DIST)
	cp build/xinput9_1_0.dll scripts/install.sh scripts/uninstall.sh README.md LICENSE $(DIST)/
	cd dist && zip -qr mk11-crossover-face-fix-$(VERSION).zip mk11-crossover-face-fix-$(VERSION)
	cd dist && shasum -a 256 mk11-crossover-face-fix-$(VERSION).zip > mk11-crossover-face-fix-$(VERSION).zip.sha256
	@echo "release files in dist/"

clean:
	rm -rf build dist

.PHONY: dist clean
