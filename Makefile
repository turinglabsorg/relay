CXX ?= g++
PACKAGES = Qt6Core Qt6Gui Qt6Quick Qt6Qml Qt6QuickControls2 Qt6Concurrent libvncclient libsecret-1 openssl zlib
CXXFLAGS += -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -fPIC $(shell pkg-config --cflags $(PACKAGES))
LDLIBS += $(shell pkg-config --libs $(PACKAGES)) -pthread
QT_TOOLS := $(shell pkg-config --variable=libexecdir Qt6Core)

.PHONY: all clean test
all: build/relay

build:
	mkdir -p build

build/moc_session.cpp: src/session.h | build
	$(QT_TOOLS)/moc $< -o $@

build/moc_credentials.cpp: src/credentials.h | build
	$(QT_TOOLS)/moc $< -o $@

build/resources.cpp: resources.qrc $(wildcard ui/*.qml) assets/relay.svg | build
	$(QT_TOOLS)/rcc $< -o $@

build/relay: src/main.cpp src/session.cpp src/session.h src/apple_mvs.cpp src/apple_mvs.h src/credentials.cpp src/credentials.h build/moc_session.cpp build/moc_credentials.cpp build/resources.cpp
	$(CXX) $(CXXFLAGS) src/main.cpp src/session.cpp src/apple_mvs.cpp src/credentials.cpp build/moc_session.cpp build/moc_credentials.cpp build/resources.cpp -o $@ $(LDLIBS)

test: build/relay
	python tests/test_vnc.py
	python tests/test_ui.py
	python tests/test_credentials.py

clean:
	rm -rf build
