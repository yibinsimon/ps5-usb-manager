# usbmanage.elf — PS5 USB 管理服务 payload
#
# 依赖 ps5-payload-sdk：https://github.com/ps5-payload-dev/sdk
#
# ------------------------------------------------------------
#  Linux / macOS ：
#    export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#    make
#
#  Windows（Git Bash / MSYS2，无需 WSL、无需 Docker）：
#    bash build.sh
#    —— 直接跑本 Makefile 亦可（make 会转调 build.sh）。
#    原因见 build.sh 头部注释：SDK 自带的 win/prospero-clang.cmd
#    漏了 clang>=20 的 crt1.o 去重逻辑，直接用它链接会失败。
# ------------------------------------------------------------
#
# 产物：usbmanage.elf

ifndef PS5_PAYLOAD_SDK
$(error 请先设置 PS5_PAYLOAD_SDK 环境变量，指向 ps5-payload-sdk 根目录)
endif

HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
TARGET := usbmanage.elf
SRC    := usbmanage.c

# 内置网页选择界面：web/index.html 经 xxd 转成 C 字节数组后编进 elf。
# xxd 随 vim-common 提供（Debian: apt install xxd；macOS: brew install vim）。
WEB    := web/index.html
WEBHDR := web/index_html.h

ifeq ($(OS),Windows_NT)

# Windows：交给 build.sh（内含 PATH 与 crt1.o 处理）
all:
	bash "$(HERE)build.sh"

clean:
	rm -f $(TARGET) $(SRC:%.c=%.o) $(WEBHDR)

else

# Linux / macOS：用 SDK 官方 wrapper
CC     := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CFLAGS := -Os -Wall -Wextra
LDADD  := -lkernel_sys -lSceLibcInternal -lSceNet

all: $(TARGET)

$(WEBHDR): $(WEB)
	xxd -i -n usbmanage_index_html $< > $@

$(TARGET): $(SRC) $(WEBHDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDADD)

clean:
	rm -f $(TARGET) $(SRC:%.c=%.o) $(WEBHDR)

endif

.PHONY: all clean
