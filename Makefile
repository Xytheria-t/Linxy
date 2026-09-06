CC    := x86_64-w64-mingw32-gcc
CFLAGS := -O2 -s -Wall -Wextra -mwindows
TARGET := Linxy.exe
# 运行副本所在目录,make deploy 会把新构建的 exe 复制过去(按需修改)
DEPLOY_DIR ?= /mnt/d/Tool/Linxy

$(TARGET): src/main.c
	$(CC) $(CFLAGS) -o $@ $< -lole32 -luuid -lshell32

deploy: $(TARGET)
	cp -f $(TARGET) $(DEPLOY_DIR)/

clean:
	rm -f $(TARGET)

.PHONY: clean deploy
