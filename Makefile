# macOS wired tethering — Android USB tethering for macOS
# SIP/kext/DriverKit 없이 사용자 공간 RNDIS + utun 으로 테더링한다.

VERSION      := 1.0.0
BUILD_NUMBER ?= 1
# Empty = ad-hoc sign. Set to a Developer ID for internal signed builds.
# 비우면 임시(ad-hoc) 서명. 사내 Developer ID 가 있으면 그 값을 넣는다.
CODESIGN_IDENTITY ?=

BUILD       := build
DIST        := dist
APP_DISPLAY := macOS wired tethering
APP_EXEC    := MacOSWiredTethering
APP_BUNDLE  := $(BUILD)/$(APP_DISPLAY).app
ENGINE_BIN  := macos-wired-tethering-engine
ENGINE      := $(BUILD)/$(ENGINE_BIN)
HELPER_BIN  := macos-wired-tethering-helper
HELPER      := $(BUILD)/$(HELPER_BIN)
TEST_BIN    := $(BUILD)/test_rndis
TEST_DHCP   := $(BUILD)/test_dhcp
TEST_MAC    := $(BUILD)/test_mac_sync
TEST_PARSE  := $(BUILD)/test_net_parse
TEST_UTUN   := $(BUILD)/test_utun
TEST_SWIFT  := $(BUILD)/test_parse
TSAN_DIR    := $(BUILD)/tsan

SDK        := $(shell xcrun --show-sdk-path)
MINVER     := 14.0
ARCH       := $(shell uname -m)
TARGET     := $(ARCH)-apple-macos$(MINVER)

CC         := cc
CFLAGS     := -std=c11 -O2 -g -Wall -Wextra -Wshadow \
              -mmacosx-version-min=$(MINVER) \
              -Iengine/include \
              -DAT_VERSION=\"$(VERSION)\"
TSAN_CFLAGS := $(filter-out -O2,$(CFLAGS)) -O1 -fsanitize=thread -fno-omit-frame-pointer
LDFLAGS    := -framework IOKit -framework CoreFoundation \
              -framework SystemConfiguration -lpthread

SWIFTC     := swiftc
SWIFTFLAGS := -sdk $(SDK) -target $(TARGET) -O \
              -parse-as-library \
	-framework SwiftUI -framework AppKit -framework Foundation \
	-framework CoreWLAN -framework Carbon -framework OpenDirectory \
	-framework ServiceManagement -framework Security

ENGINE_SRCS := \
	engine/src/common.c \
	engine/src/rndis.c \
	engine/src/usb_rndis.c \
	engine/src/utun_if.c \
	engine/src/net_parse.c \
	engine/src/net_config.c \
	engine/src/dhcp_client.c \
	engine/src/json_status.c \
	engine/src/tether_engine.c \
	engine/src/main.c

SWIFT_SRCS := \
	app/MacOSWiredTetheringApp.swift \
	app/Models.swift \
	app/ParseHelpers.swift \
	app/L10n.swift \
	app/MenuBarGlyph.swift \
	app/PrivilegeLauncher.swift \
	helper/HelperProtocol.swift \
	helper/HelperSecurity.swift \
	app/HelperClient.swift \
	app/TetherController.swift \
	app/ContentView.swift \
	app/AboutWindow.swift \
	app/LogWindow.swift \
	app/WiFiPower.swift \
	app/AdminAuthWindow.swift

HELPER_SRCS := \
	helper/HelperMain.swift \
	helper/HelperProtocol.swift \
	helper/HelperSecurity.swift \
	app/L10n.swift \
	app/Models.swift

.PHONY: all app engine helper test test-tsan clean install uninstall run dist dist-pkg smoke

all: test app

$(BUILD):
	mkdir -p $(BUILD)

engine: $(ENGINE)
helper: $(HELPER)

$(ENGINE): $(ENGINE_SRCS) engine/include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(ENGINE_SRCS) $(LDFLAGS)

$(HELPER): $(HELPER_SRCS) | $(BUILD)
	$(SWIFTC) -sdk $(SDK) -target $(TARGET) -O -parse-as-library \
		-framework Foundation -framework Security \
		-o $@ $(HELPER_SRCS)

$(TEST_BIN): engine/tests/test_rndis.c engine/src/rndis.c engine/src/common.c engine/include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ engine/tests/test_rndis.c engine/src/rndis.c engine/src/common.c

$(TEST_DHCP): engine/tests/test_dhcp.c engine/src/dhcp_client.c engine/src/common.c engine/include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -DAT_DHCP_TEST -o $@ engine/tests/test_dhcp.c engine/src/dhcp_client.c engine/src/common.c

$(TEST_MAC): engine/tests/test_mac_sync.c engine/src/common.c engine/include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ engine/tests/test_mac_sync.c engine/src/common.c

$(TEST_PARSE): engine/tests/test_net_parse.c engine/src/net_parse.c engine/src/common.c engine/include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ engine/tests/test_net_parse.c engine/src/net_parse.c engine/src/common.c

$(TEST_UTUN): engine/tests/test_utun.c engine/src/utun_if.c engine/src/common.c engine/include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ engine/tests/test_utun.c engine/src/utun_if.c engine/src/common.c

$(TEST_SWIFT): app/tests/test_parse.swift app/ParseHelpers.swift app/Models.swift app/L10n.swift app/PrivilegeLauncher.swift helper/HelperSecurity.swift | $(BUILD)
	$(SWIFTC) -sdk $(SDK) -target $(TARGET) -O -parse-as-library \
		-framework Security \
		-o $@ app/tests/test_parse.swift app/ParseHelpers.swift app/Models.swift \
		app/L10n.swift app/PrivilegeLauncher.swift helper/HelperSecurity.swift

test: $(TEST_BIN) $(TEST_DHCP) $(TEST_MAC) $(TEST_PARSE) $(TEST_UTUN) $(TEST_SWIFT)
	$(TEST_BIN)
	$(TEST_DHCP)
	$(TEST_MAC)
	$(TEST_PARSE)
	$(TEST_UTUN)
	$(TEST_SWIFT)

# ThreadSanitizer on the C unit tests, including concurrent MAC snapshot vs store.
# C 단위 테스트에 TSan 을 건다. MAC 스냅샷/저장 경합 경로를 포함한다.
test-tsan: | $(BUILD)
	mkdir -p $(TSAN_DIR)
	$(CC) $(TSAN_CFLAGS) -o $(TSAN_DIR)/test_rndis \
		engine/tests/test_rndis.c engine/src/rndis.c engine/src/common.c
	$(CC) $(TSAN_CFLAGS) -DAT_DHCP_TEST -o $(TSAN_DIR)/test_dhcp \
		engine/tests/test_dhcp.c engine/src/dhcp_client.c engine/src/common.c
	$(CC) $(TSAN_CFLAGS) -o $(TSAN_DIR)/test_mac_sync \
		engine/tests/test_mac_sync.c engine/src/common.c
	$(CC) $(TSAN_CFLAGS) -o $(TSAN_DIR)/test_net_parse \
		engine/tests/test_net_parse.c engine/src/net_parse.c engine/src/common.c
	$(CC) $(TSAN_CFLAGS) -o $(TSAN_DIR)/test_utun \
		engine/tests/test_utun.c engine/src/utun_if.c engine/src/common.c
	$(TSAN_DIR)/test_rndis
	$(TSAN_DIR)/test_dhcp
	$(TSAN_DIR)/test_mac_sync
	$(TSAN_DIR)/test_net_parse
	$(TSAN_DIR)/test_utun

APP_STAMP := $(BUILD)/.app-stamp

app: $(APP_STAMP)

$(APP_STAMP): $(ENGINE) $(HELPER) $(SWIFT_SRCS) helper/com.macoswiredtethering.helper.plist helper/helper.entitlements engine/engine.entitlements app/MacOSWiredTethering.entitlements app/Info.plist app/PrivacyInfo.xcprivacy app/en.lproj/InfoPlist.strings app/ko.lproj/InfoPlist.strings app/en.lproj/Localizable.strings app/ko.lproj/Localizable.strings app/Assets/AppIcon.icns app/Assets/MenuBarIcon.png app/Assets/MenuBarIcon@2x.png | $(BUILD)
	rm -rf "$(APP_BUNDLE)"
	mkdir -p "$(APP_BUNDLE)/Contents/MacOS" "$(APP_BUNDLE)/Contents/Resources" \
		"$(APP_BUNDLE)/Contents/Library/LaunchDaemons"
	cp app/Info.plist "$(APP_BUNDLE)/Contents/Info.plist"
	/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $(VERSION)" \
		"$(APP_BUNDLE)/Contents/Info.plist"
	/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $(BUILD_NUMBER)" \
		"$(APP_BUNDLE)/Contents/Info.plist"
	/usr/libexec/PlistBuddy -c "Set :CFBundleGetInfoString macOS wired tethering $(VERSION), Android USB tethering for macOS." \
		"$(APP_BUNDLE)/Contents/Info.plist"
	cp app/Assets/AppIcon.icns "$(APP_BUNDLE)/Contents/Resources/AppIcon.icns"
	cp app/Assets/MenuBarIcon.png "$(APP_BUNDLE)/Contents/Resources/MenuBarIcon.png"
	cp app/Assets/MenuBarIcon@2x.png "$(APP_BUNDLE)/Contents/Resources/MenuBarIcon@2x.png"
	cp app/PrivacyInfo.xcprivacy "$(APP_BUNDLE)/Contents/Resources/PrivacyInfo.xcprivacy"
	mkdir -p "$(APP_BUNDLE)/Contents/Resources/en.lproj" "$(APP_BUNDLE)/Contents/Resources/ko.lproj"
	cp app/en.lproj/InfoPlist.strings "$(APP_BUNDLE)/Contents/Resources/en.lproj/InfoPlist.strings"
	cp app/ko.lproj/InfoPlist.strings "$(APP_BUNDLE)/Contents/Resources/ko.lproj/InfoPlist.strings"
	cp app/en.lproj/Localizable.strings "$(APP_BUNDLE)/Contents/Resources/en.lproj/Localizable.strings"
	cp app/ko.lproj/Localizable.strings "$(APP_BUNDLE)/Contents/Resources/ko.lproj/Localizable.strings"
	$(SWIFTC) $(SWIFTFLAGS) -o "$(APP_BUNDLE)/Contents/MacOS/$(APP_EXEC)" $(SWIFT_SRCS)
	cp $(ENGINE) "$(APP_BUNDLE)/Contents/MacOS/$(ENGINE_BIN)"
	cp $(HELPER) "$(APP_BUNDLE)/Contents/MacOS/$(HELPER_BIN)"
	cp helper/com.macoswiredtethering.helper.plist \
		"$(APP_BUNDLE)/Contents/Library/LaunchDaemons/com.macoswiredtethering.helper.plist"
	chmod +x "$(APP_BUNDLE)/Contents/MacOS/$(APP_EXEC)" \
	         "$(APP_BUNDLE)/Contents/MacOS/$(ENGINE_BIN)" \
	         "$(APP_BUNDLE)/Contents/MacOS/$(HELPER_BIN)"
	touch $(APP_STAMP)

run: app
	open "$(APP_BUNDLE)"

install: app
	APP_SRC="$(APP_BUNDLE)" $(SHELL) scripts/install.sh --src "$(APP_BUNDLE)"

uninstall:
	$(SHELL) scripts/uninstall.sh

# Internal distribution zip (+ SHA-256). Optional: CODESIGN_IDENTITY="Developer ID Application: …"
# 사내 배포용 zip 과 SHA-256. 서명 인증서가 있으면 CODESIGN_IDENTITY 를 넘긴다.
dist: app
	chmod +x scripts/package.sh scripts/install.sh scripts/uninstall.sh scripts/smoke-check.sh
	VERSION="$(VERSION)" BUILD_NUMBER="$(BUILD_NUMBER)" \
		APP_BUNDLE="$(APP_BUNDLE)" ENGINE="$(ENGINE)" \
		CODESIGN_IDENTITY="$(CODESIGN_IDENTITY)" \
		$(SHELL) scripts/package.sh

dist-pkg: dist
	VERSION="$(VERSION)" BUILD_NUMBER="$(BUILD_NUMBER)" \
		APP_BUNDLE="$(APP_BUNDLE)" \
		$(SHELL) scripts/package.sh --pkg

smoke: app
	chmod +x scripts/smoke-check.sh
	APP_BUNDLE="$(APP_BUNDLE)" ENGINE="$(ENGINE)" \
		$(SHELL) scripts/smoke-check.sh

clean:
	rm -rf $(BUILD) $(DIST)
