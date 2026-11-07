# MinecraftWebProxy

一个轻量的 **SOCKS5 代理转发器**，把 Minecraft 客户端到服务器的流量经由你自备的 SOCKS5 代理转发出去。

单文件 C++17 实现，跨平台（Windows / Linux x86_64 / Linux ARM64 / Linux ARM32），
用 [zig](https://ziglang.org) 一条命令交叉编译，musl 静态链接，拷到目标机直接跑。

```
游戏客户端 ── 127.0.0.1:25565 ──> 本程序 ── SOCKS5 ──> 代理服务器 ──> 目标游戏服务器
```

## 特性

- **零依赖**：只有标准库 + socket，编译出来是单个静态可执行文件
- **无认证 / 账密认证自适应**：账号密码留空就只申报“无认证”，填了才走 RFC 1929
- **自动 hosts 重定向**：启动时把目标域名指向 `127.0.0.1` 并打上 `# MC_PROXY` 标记，退出时清理
- **双向转发**：每条连接两个线程，4096 字节缓冲，粘包/半包不做处理直接透传
- **控制台编码自适应**：Windows 控制台是 GBK 也能正常显示中文，重定向到文件时输出 UTF-8
- **跨平台构建**：Windows 上一条命令编译出全部四种目标

## 快速开始

### 1. 编译

需要 [zig](https://ziglang.org/download/) 0.13 或更新版本。

```bash
# Linux / macOS
ZIG=/path/to/zig ./scripts/build.sh
```

```bat
REM Windows
set ZIG=D:\zig\zig.exe
scripts\build.bat
```

也可以直接用 GCC / MSVC：

```bash
# Windows (MinGW)
g++ -O2 -std=c++17 src/proxy_core.cpp -o proxy_core.exe -lws2_32 -static

# Linux
g++ -O2 -std=c++17 src/proxy_core.cpp -o proxy_core -static -lpthread
```

产物在 `build/` 下：

| 目标 | 文件 |
|---|---|
| Windows x64 | `proxy_core_win_x64.exe` |
| Linux x86_64 | `proxy_core_linux_x64` |
| Linux ARM64 | `proxy_core_linux_arm64` |
| Linux ARM32 (armhf) | `proxy_core_linux_arm32` |

### 2. 运行

```bash
# Linux（改 hosts 需要 root）
chmod +x build/proxy_core_linux_arm64
sudo ./build/proxy_core_linux_arm64
```

```bat
REM Windows
build\proxy_core_win_x64.exe
```

按提示输入：

```
========================================
           代理核心演示程序
========================================

目标服务器地址: mc.example.com
目标端口 (默认25565):
代理IP地址: 1.2.3.4
代理端口 (默认1080): 1080
代理用户名 (留空=不用账密):
代理密码   (留空=不用账密):
```

成功后：

```
[√] 代理连接测试成功
[√] 自定义代理启动成功!
[√] 本地监听: 0.0.0.0:25565
[√] 目标服务器: mc.example.com:25565
[√] 使用代理: 1.2.3.4:1080
[√] 认证方式: 无（未填写账号密码）
```

然后在游戏里连接 `127.0.0.1:25565` 即可。

按回车停止代理并退出，退出时会自动清理 hosts。

## 协议细节

### 握手

| 场景 | 客户端发送 |
|---|---|
| 账号密码都留空 | `05 01 00`（VER=5, NMETHODS=1, 方法：无认证） |
| 用户名或密码非空 | `05 02 00 02`（VER=5, NMETHODS=2, 方法：无认证 / 用户名密码） |

服务器回 `VER METHOD`：

- `0x00` 无认证 → 直接 CONNECT
- `0x02` 用户名密码 → 走 RFC 1929
- `0xFF` 拒绝所有方法 → 报错退出

### 用户名密码认证（RFC 1929）

```
01 | len(user) | user | len(pass) | pass
```

服务端回 2 字节，第二字节必须为 `0x00`。

### CONNECT

固定使用域名方式（ATYP = `0x03`），即使目标是纯 IP 也当域名交给代理解析：

```
05 01 00 03 | len(host) | host | port_hi port_lo
```

### 回复

```
VER REP RSV ATYP
```

`REP != 0` 即失败，随后按 `ATYP` 读完 `BND.ADDR + BND.PORT`：

- `ATYP=0x01` → 4 + 2 字节
- `ATYP=0x03` → 1 + len + 2 字节
- `ATYP=0x04` → 16 + 2 字节

收发超时均为 10 秒。

## 项目结构

```
.
├── src/
│   ├── proxy_core.cpp            主实现（跨平台 C++17）
│   └── proxy_core_reference.cs   C# 参考实现（逻辑等价，便于对照）
├── scripts/
│   ├── build.sh                  zig 交叉编译脚本
│   └── build.bat
├── docs/
│   ├── architecture.md           架构与设计说明
│   └── protocol.md               协议细节
└── .github/workflows/build.yml   CI：自动构建四平台产物
```

## 说明

- 本项目为独立实现，行为与某款闭源 Minecraft 代理工具的代理部分保持一致
  （SOCKS5 握手字节、CONNECT 方式、hosts 标记等），便于互相替换。
- 仅供学习与个人网络调试使用，请自行确保对目标服务器的使用符合其服务条款。

## License

[Apache License 2.0](LICENSE)
