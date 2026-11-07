# 架构说明

## 数据流

```
┌──────────────┐        ┌──────────────────────┐        ┌──────────────┐        ┌──────────────┐
│ Minecraft    │ TCP    │  proxy_core          │ SOCKS5 │  你的代理     │  TCP   │  目标游戏     │
│ 客户端        │───────>│  0.0.0.0:25565 监听   │───────>│  服务器       │───────>│  服务器       │
└──────────────┘        └──────────────────────┘        └──────────────┘        └──────────────┘
```

一条游戏连接在代理里对应：

```
accept() ──> 新建线程 handle_client()
                 ├── socks5_connect()  建立到代理服务器的 CONNECT 隧道
                 ├── 线程 A: relay_down()  upstream -> client
                 └── 线程 B: relay_up()    client   -> upstream
```

## 线程模型

| 线程 | 数量 | 职责 |
|---|---|---|
| `listener_thread` | 1 | bind / listen / select / accept，每条连接 `detach` 一个 `handle_client` |
| `handle_client` | 每连接 1 | 建隧道，跑下游转发，join 上游转发线程 |
| `relay_up` | 每连接 1 | 客户端 → 代理侧 |

没有线程池，连接数多了线程数会线性增长。对 Minecraft 这种「一个客户端一条连接」的
场景足够了；如果要支撑高并发，把 `handle_client` 换成线程池 + IOCP/epoll 即可。

## 关键函数

| 函数 | 作用 |
|---|---|
| `socks5_connect()` | SOCKS5 握手、可选用户密码认证、发 CONNECT、读回复 |
| `listener_thread()` | 本地监听与 accept 循环 |
| `relay_down()` | 代理侧收到的数据写给客户端 |
| `relay_up()` | 客户端发出的数据写给代理侧 |
| `start_custom_proxy()` | 先做一次连通性测试（连上即关），再写 hosts、起监听线程 |
| `stop_proxy()` | 清运行标志、关监听 socket、清理 hosts |
| `hosts_add_entry()` | 追加 `127.0.0.1 <域名> # MC_PROXY` |
| `hosts_remove_entries()` | 删除所有带 `# MC_PROXY` 的行 |

## 平台抽象

`socket` 的差异都收在两个宏分支里：

| 项 | Windows | Linux |
|---|---|---|
| socket 句柄 | `SOCKET` / `INVALID_SOCKET` | `int` / `-1` |
| 关闭 | `closesocket` | `close` |
| 错误码 | `WSAGetLastError()` | `errno` |
| 超时设置 | `DWORD` 毫秒 | `timeval` |
| `select` 首参 | 忽略，传 0 | 必须传 `maxfd + 1` |
| hosts 路径 | `C:\Windows\System32\drivers\etc\hosts` | `/etc/hosts` |

## 控制台编码

Windows 控制台默认代码页是 GBK(936)，而源码里的中文是 UTF-8 字节。
直接 `printf` 会显示成 `浠ｇ悊鏍稿績婕旂ず绋嬪簭`。

处理方式：

```cpp
bool isConsole = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
g_console_utf8 = (!isConsole) || (GetConsoleOutputCP() == 65001);
```

- stdout 是控制台且代码页 ≠ 65001 → `MultiByteToWideChar` → `WideCharToMultiByte(控制台代码页)` 再输出
- stdout 被重定向 → 原样输出 UTF-8，日志文件保持 UTF-8
- 代码页本来就是 65001 → 直接输出

## 与原工具的差异

| 项 | 原工具 | 本项目 |
|---|---|---|
| 账号密码留空 | 仍申报「用户名密码」方法 | 只申报「无认证」 |
| 监听循环 | `select` 超时 `{0,0}`，忙等占满一个核 | 10ms 超时 |
| 业务逻辑 | 含登录、余额、区域选择等（服务端接口） | 只保留代理核心 |

## 已知限制

- 不做 SOCKS5 的 `BIND` / `UDP ASSOCIATE`，只支持 `CONNECT`
- 目标地址固定按域名（ATYP=3）发送，若代理服务器不支持域名解析则无法连接 IP 形式的目标
- hosts 写入需要管理员/root 权限；无权限时仅提示，代理本身照常工作
- 没有做进程守护、日志落盘、配置持久化
