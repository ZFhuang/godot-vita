#!/bin/bash
# 通过 WSL2 调用 Windows 侧的 scons 进行编译
cd /mnt/d/Github/godot-vita
cmd.exe /c "scons p=windows target=release_debug tools=yes warnings=all werror=no"
