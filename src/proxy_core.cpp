// Copyright 2026 MusangOpen
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ============================================================================
//  LeaveH 代理核心（从 Leave IPban Beta 2.7 Fix.exe 反编译还原）
//  ---------------------------------------------------------------------------
//  跨平台：Windows (winsock2) / Linux (POSIX socket)
//  可单独编译成 exe / elf，不依赖原程序。
//
//  编译示例：
//    Windows (zig):
//      zig c++ -target x86_64-windows-gnu -O2 -std=c++17 proxy_core.cpp -o proxy_core.exe -lws2_32
//    Windows (MinGW):
//      g++ -O2 -std=c++17 proxy_core.cpp -o proxy_core.exe -lws2_32 -static
//    Linux x86_64:
//      g++ -O2 -std=c++17 proxy_core.cpp -o proxy_core -static -lpthread
//    Linux ARM64（交叉）:
//      zig c++ -target aarch64-linux-musl -O2 -std=c++17 proxy_core.cpp -o proxy_core_arm64 -static -lpthread
//
//  原程序对应地址（ImageBase = 0x140000000）：
//    FUN_140035490  socks5_connect()        SOCKS5 握手 + CONNECT
//    FUN_140034580  listener_thread()       本地监听 / accept 循环
//    FUN_140035330  relay_down()            upstream -> client 转发
//    FUN_1400e4300  relay_up()              client -> upstream 转发
//    FUN_1400396b0  start_custom_proxy()    “使用自定义代理”入口
//    FUN_14003b070  hosts_add_entry()       写 hosts（# MC_PROXY）
//    FUN_140033d00  hosts_remove_entries()  清理 hosts
//
//  改动（相对原版）：
//    * 账号密码留空时，握手只申报“无认证”，不再走用户名密码认证；
//      用户名或密码填了任意一个，才申报“用户名密码”。
//    * 监听循环超时由 {0,0} 改为 10ms，避免忙等占满 CPU。
// ============================================================================

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
    #define _WINSOCK_DEPRECATED_NO_WARNINGS
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <csignal>
  #include <cerrno>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ===========================================================================
//  平台抽象层
// ===========================================================================
#ifdef _WIN32

typedef SOCKET net_sock;
static const net_sock kInvalidSock = INVALID_SOCKET;

static bool net_init()
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
static void net_done() { WSACleanup(); }
static void net_close(net_sock s) { ::closesocket(s); }
static int  net_err() { return WSAGetLastError(); }

static void net_set_timeout(net_sock s, int ms)
{
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, (int)sizeof(ms));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, (int)sizeof(ms));
}

static bool net_wait_readable(net_sock s, int ms)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r = ::select(0, &rd, nullptr, nullptr, &tv);
    return r > 0 && FD_ISSET(s, &rd) != 0;
}

#else

typedef int net_sock;
static const net_sock kInvalidSock = -1;

static bool net_init()
{
    ::signal(SIGPIPE, SIG_IGN);          // 对端断开时不要被 SIGPIPE 杀掉
    return true;
}
static void net_done() {}
static void net_close(net_sock s) { ::close(s); }
static int  net_err() { return errno; }

static void net_set_timeout(net_sock s, int ms)
{
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, (socklen_t)sizeof(tv));
}

static bool net_wait_readable(net_sock s, int ms)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r = ::select(s + 1, &rd, nullptr, nullptr, &tv);
    return r > 0 && FD_ISSET(s, &rd) != 0;
}

#endif

static int net_send(net_sock s, const void* buf, int len)
{
    return (int)::send(s, (const char*)buf, len, 0);
}
static int net_recv(net_sock s, void* buf, int len)
{
    return (int)::recv(s, (char*)buf, len, 0);
}

// ===========================================================================
//  控制台输出编码适配
//  Windows 控制台默认是 GBK(936)，直接输出 UTF-8 字节会变成乱码。
//  这里先把控制台代码页切成 UTF-8；若切换不生效，再把文本转成当前
//  代码页输出，保证任何控制台下都不乱码。
// ===========================================================================
#ifdef _WIN32
static bool g_console_utf8 = true;

static void console_init()
{
    // stdout 是控制台时按控制台代码页输出（通常是 GBK/936），
    // 被重定向到文件或管道时按 UTF-8 原样输出。
    DWORD mode = 0;
    bool isConsole = (::GetConsoleMode(::GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0);
    g_console_utf8 = (!isConsole) || (::GetConsoleOutputCP() == 65001);
}

static void out_text(const char* utf8)
{
    if (g_console_utf8 || utf8 == nullptr)
    {
        std::fputs(utf8 ? utf8 : "", stdout);
        return;
    }

    UINT cp = ::GetConsoleOutputCP();
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wlen <= 1)
    {
        std::fputs(utf8, stdout);
        return;
    }
    std::vector<wchar_t> w((size_t)wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), wlen);

    int alen = ::WideCharToMultiByte(cp, 0, w.data(), -1, nullptr, 0, nullptr, nullptr);
    if (alen <= 1)
    {
        std::fputs(utf8, stdout);
        return;
    }
    std::vector<char> a((size_t)alen);
    ::WideCharToMultiByte(cp, 0, w.data(), -1, a.data(), alen, nullptr, nullptr);
    std::fputs(a.data(), stdout);
}

#else

static void console_init() {}
static void out_text(const char* utf8)
{
    std::fputs(utf8 ? utf8 : "", stdout);
}

#endif

// ===========================================================================
//  代理状态（对应原程序 ProxyState 对象，字段偏移见注释）
// ===========================================================================
struct ProxyState
{
    std::atomic<bool> running{ false };           // +0x00 运行标志
    net_sock          listenSock{ kInvalidSock }; // +0x18 监听 socket
    std::string       proxyIp;                    // +0x20 代理服务器 IP / 域名
    std::string       proxyUser;                  // +0x40 代理用户名
    std::string       proxyPass;                  // +0x60 代理密码
    std::string       targetHost;                 // +0x80 目标服务器
    int               proxyPort{ 0 };             // +0xa0 代理端口
    int               targetPort{ 0 };            // +0xa4 目标端口
    int               listenPort{ 25565 };        // +0xa8 本地监听端口
    std::atomic<int>  connCount{ 0 };             // +0xac 当前连接数
};

static void log_line(const std::string& s)
{
    out_text(s.c_str());
    out_text("\n");
    std::fflush(stdout);
}

static const char* socks5_reply_text(unsigned char rep)
{
    switch (rep)
    {
    case 0x01: return "一般性失败";
    case 0x02: return "规则不允许";
    case 0x03: return "网络不可达";
    case 0x04: return "主机不可达";
    case 0x05: return "连接被拒绝";
    case 0x06: return "TTL过期";
    case 0x07: return "不支持的命令";
    case 0x08: return "不支持的地址类型";
    default:   return nullptr;
    }
}

static std::string hex2(unsigned int v)
{
    char b[8];
    std::snprintf(b, sizeof(b), "0x%02X", v & 0xFF);
    return std::string(b);
}

// ---------------------------------------------------------------------------
//  socks5_connect —— 对应 FUN_140035490
//
//  认证方式申报规则（本次改动）：
//    账号、密码都为空   -> 只报 { 05, 01, 00 }         （只支持无认证）
//    有任意一个非空     -> 报 { 05, 02, 00, 02 }       （无认证 / 用户名密码）
// ---------------------------------------------------------------------------
static net_sock socks5_connect(const ProxyState& st)
{
    const bool useAuth = !st.proxyUser.empty() || !st.proxyPass.empty();

    net_sock s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSock)
    {
        log_line(std::string("[DEBUG] 创建socket失败: ") + std::to_string(net_err()));
        return kInvalidSock;
    }

    net_set_timeout(s, 10000);                 // 原程序：收/发超时 10 秒

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    std::string portStr = std::to_string(st.proxyPort);
    int rc = ::getaddrinfo(st.proxyIp.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr)
    {
        log_line(std::string("[DEBUG] 解析代理地址 ") + st.proxyIp +
                 " 失败, 错误码: " + std::to_string(rc));
        net_close(s);
        return kInvalidSock;
    }

    rc = ::connect(s, res->ai_addr, (int)res->ai_addrlen);
    if (rc != 0)
    {
        log_line("[DEBUG] 连接代理服务器 " + st.proxyIp + ":" + portStr +
                 " 失败, 错误码: " + std::to_string(net_err()));
        ::freeaddrinfo(res);
        net_close(s);
        return kInvalidSock;
    }
    ::freeaddrinfo(res);

    // ---- 1. 协商认证方式 ---------------------------------------------------
    unsigned char hello[4];
    int helloLen;
    if (useAuth)
    {
        hello[0] = 0x05; hello[1] = 0x02; hello[2] = 0x00; hello[3] = 0x02;
        helloLen = 4;
    }
    else
    {
        hello[0] = 0x05; hello[1] = 0x01; hello[2] = 0x00;
        helloLen = 3;
    }

    if (net_send(s, hello, helloLen) != helloLen)
    {
        log_line("[DEBUG] 发送握手数据失败");
        net_close(s);
        return kInvalidSock;
    }

    unsigned char resp[2] = { 0, 0 };
    int n = net_recv(s, resp, 2);
    if (n != 2 || resp[0] != 0x05)
    {
        log_line("[DEBUG] 握手响应无效, 收到 " + std::to_string(n) + " 字节");
        net_close(s);
        return kInvalidSock;
    }

    if (resp[1] == 0xFF)
    {
        log_line("[DEBUG] 代理服务器拒绝所有请求的认证方法 (0xFF)");
        net_close(s);
        return kInvalidSock;
    }
    if (resp[1] == 0x02 && !useAuth)
    {
        log_line("[DEBUG] 代理服务器要求用户名密码，但未填写账号密码");
        net_close(s);
        return kInvalidSock;
    }
    if (resp[1] != 0x00 && resp[1] != 0x02)
    {
        log_line("[DEBUG] 代理服务器选择了不支持的认证方法: " + hex2(resp[1]));
        net_close(s);
        return kInvalidSock;
    }

    // ---- 2. 用户名密码认证（RFC 1929）--------------------------------------
    if (resp[1] == 0x02)
    {
        std::vector<unsigned char> auth;
        auth.push_back(0x01);
        auth.push_back((unsigned char)st.proxyUser.size());
        auth.insert(auth.end(), st.proxyUser.begin(), st.proxyUser.end());
        auth.push_back((unsigned char)st.proxyPass.size());
        auth.insert(auth.end(), st.proxyPass.begin(), st.proxyPass.end());

        if (net_send(s, auth.data(), (int)auth.size()) != (int)auth.size())
        {
            log_line("[DEBUG] 发送认证数据失败");
            net_close(s);
            return kInvalidSock;
        }

        unsigned char ack[2] = { 0, 0 };
        n = net_recv(s, ack, 2);
        if (n != 2 || ack[1] != 0x00)
        {
            log_line("[DEBUG] 认证失败, 响应: " + std::to_string(n) + " 字节");
            net_close(s);
            return kInvalidSock;
        }
    }

    // ---- 3. CONNECT（原程序固定用 ATYP=0x03 域名方式）----------------------
    std::vector<unsigned char> req;
    req.push_back(0x05);
    req.push_back(0x01);
    req.push_back(0x00);
    req.push_back(0x03);
    req.push_back((unsigned char)st.targetHost.size());
    req.insert(req.end(), st.targetHost.begin(), st.targetHost.end());
    req.push_back((unsigned char)((st.targetPort >> 8) & 0xFF));
    req.push_back((unsigned char)(st.targetPort & 0xFF));

    if (net_send(s, req.data(), (int)req.size()) != (int)req.size())
    {
        log_line("[DEBUG] 发送CONNECT请求失败");
        net_close(s);
        return kInvalidSock;
    }

    // ---- 4. 读回复头 -------------------------------------------------------
    unsigned char head[4];
    n = 0;
    while (n < 4)
    {
        int r = net_recv(s, head + n, 4 - n);
        if (r < 1)
        {
            log_line("[DEBUG] 读取CONNECT响应头失败, 已读 " + std::to_string(n));
            net_close(s);
            return kInvalidSock;
        }
        n += r;
    }

    if (head[1] != 0x00)
    {
        std::string msg = "[DEBUG] CONNECT请求被拒绝, 错误码: " + hex2(head[1]);
        const char* why = socks5_reply_text(head[1]);
        if (why) msg += std::string(" (") + why + ")";
        log_line(msg);
        net_close(s);
        return kInvalidSock;
    }

    // ---- 5. 读完 BND.ADDR + BND.PORT ---------------------------------------
    int rest = 0;
    unsigned char atyp = head[3];
    if (atyp == 0x01)      rest = 4 + 2;
    else if (atyp == 0x04) rest = 16 + 2;
    else if (atyp == 0x03)
    {
        unsigned char len = 0;
        if (net_recv(s, &len, 1) != 1)
        {
            log_line("[DEBUG] 读取域名长度失败");
            net_close(s);
            return kInvalidSock;
        }
        rest = (int)len + 2;
    }
    else
    {
        log_line("[DEBUG] CONNECT响应地址类型未知: " + hex2(atyp));
        net_close(s);
        return kInvalidSock;
    }

    int got = 0;
    unsigned char tmp[260];
    while (got < rest)
    {
        int r = net_recv(s, tmp, rest - got);
        if (r < 1) break;
        got += r;
    }
    if (got != rest)
    {
        log_line("[DEBUG] 读取CONNECT响应地址失败, 已读 " + std::to_string(got));
        net_close(s);
        return kInvalidSock;
    }

    return s;
}

// ---------------------------------------------------------------------------
//  relay_down —— 对应 FUN_140035330（代理侧 -> 本地客户端）
// ---------------------------------------------------------------------------
static void relay_down(const ProxyState* st, net_sock client, net_sock upstream)
{
    std::vector<char> buf(4096);
    for (;;)
    {
        if (!st->running.load(std::memory_order_relaxed)) break;
        int n = net_recv(upstream, buf.data(), (int)buf.size());
        if (n < 1) break;
        int m = net_send(client, buf.data(), n);
        if (m <= 0) break;
    }
}

// ---------------------------------------------------------------------------
//  relay_up —— 对应 FUN_1400e4300（本地客户端 -> 代理侧）
// ---------------------------------------------------------------------------
static void relay_up(const ProxyState* st, net_sock client, net_sock upstream)
{
    std::vector<char> buf(4096);
    for (;;)
    {
        if (!st->running.load(std::memory_order_relaxed)) return;
        int n = net_recv(client, buf.data(), (int)buf.size());
        if (n < 1) return;
        int m = net_send(upstream, buf.data(), n);
        if (m < 1) return;
    }
}

// ---------------------------------------------------------------------------
//  handle_client —— 每条连接一个上游 socket + 两个转发线程
// ---------------------------------------------------------------------------
static void handle_client(ProxyState* st, net_sock client)
{
    st->connCount.fetch_add(1);

    net_sock upstream = socks5_connect(*st);
    if (upstream == kInvalidSock)
    {
        net_close(client);
        st->connCount.fetch_sub(1);
        return;
    }

    std::thread up_thread(relay_up, st, client, upstream);
    relay_down(st, client, upstream);
    up_thread.join();

    net_close(upstream);
    net_close(client);
    st->connCount.fetch_sub(1);
}

// ---------------------------------------------------------------------------
//  listener_thread —— 对应 FUN_140034580
// ---------------------------------------------------------------------------
static void listener_thread(ProxyState* st)
{
    net_sock ls = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ls == kInvalidSock)
    {
        log_line("[X] 创建监听 socket 失败");
        return;
    }
    st->listenSock = ls;

    int yes = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, (int)sizeof(yes));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons((unsigned short)st->listenPort);
    addr.sin_addr.s_addr = ::htonl(INADDR_ANY);

    if (::bind(ls, (sockaddr*)&addr, (int)sizeof(addr)) != 0 || ::listen(ls, 10) != 0)
    {
        log_line("[X] 监听端口 " + std::to_string(st->listenPort) + " 失败");
        net_close(ls);
        st->listenSock = kInvalidSock;
        return;
    }

    while (st->running.load(std::memory_order_relaxed))
    {
        if (!net_wait_readable(ls, 10)) continue;

        net_sock c = ::accept(ls, nullptr, nullptr);
        if (c == kInvalidSock) continue;
        if (!st->running.load(std::memory_order_relaxed)) { net_close(c); break; }

        std::thread(handle_client, st, c).detach();
    }

    net_close(ls);
    st->listenSock = kInvalidSock;
}

// ---------------------------------------------------------------------------
//  hosts —— 对应 FUN_14003b070 / FUN_140033d00
// ---------------------------------------------------------------------------
#ifdef _WIN32
static const char* kHostsPath = "C:\\Windows\\System32\\drivers\\etc\\hosts";
#else
static const char* kHostsPath = "/etc/hosts";
#endif
static const char* kHostsMark = "# MC_PROXY";

static void hosts_add_entry(const std::string& host)
{
    FILE* f = std::fopen(kHostsPath, "a+");
    if (!f)
    {
        log_line("[警告] 无权限写入 hosts，请手动把目标域名指向 127.0.0.1");
        return;
    }
    std::fputs("\n127.0.0.1 ", f);
    std::fputs(host.c_str(), f);
    std::fputs(" # MC_PROXY", f);
    std::fflush(f);
    std::fclose(f);
}

static void hosts_remove_entries()
{
    FILE* f = std::fopen(kHostsPath, "r");
    if (!f) return;

    std::vector<std::string> keep;
    char line[1024];
    while (std::fgets(line, sizeof(line), f))
    {
        std::string s(line);
        if (s.find(kHostsMark) != std::string::npos) continue;
        keep.push_back(s);
    }
    std::fclose(f);

    FILE* w = std::fopen(kHostsPath, "w");
    if (!w) return;
    for (size_t i = 0; i < keep.size(); ++i) std::fputs(keep[i].c_str(), w);
    std::fclose(w);
}

// ---------------------------------------------------------------------------
//  start_custom_proxy —— 对应 FUN_1400396b0
// ---------------------------------------------------------------------------
static bool start_custom_proxy(ProxyState* st,
                               const std::string& targetHost, int targetPort,
                               const std::string& proxyIp, int proxyPort,
                               const std::string& user, const std::string& pass)
{
    if (st->running.load())
    {
        log_line("[X] 代理已经在运行中!");
        return false;
    }

    st->targetHost = targetHost;
    st->targetPort = targetPort;
    st->proxyIp = proxyIp;
    st->proxyPort = proxyPort;
    st->proxyUser = user;
    st->proxyPass = pass;

    log_line("正在测试自定义代理连接...");

    net_sock t = socks5_connect(*st);
    if (t == kInvalidSock)
    {
        log_line("[X] 代理连接测试失败，请检查代理信息是否正确");
        return false;
    }
    net_close(t);
    log_line("[√] 代理连接测试成功");

    hosts_add_entry(st->targetHost);

    st->running.store(true);
    std::thread(listener_thread, st).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    log_line("[√] 自定义代理启动成功!");
    log_line("[√] 本地监听: 0.0.0.0:" + std::to_string(st->listenPort));
    log_line("[√] 目标服务器: " + st->targetHost + ":" + std::to_string(st->targetPort));
    log_line("[√] 使用代理: " + st->proxyIp + ":" + std::to_string(st->proxyPort));
    if (st->proxyUser.empty() && st->proxyPass.empty())
        log_line("[√] 认证方式: 无（未填写账号密码）");
    else
        log_line("[√] 用户名: " + st->proxyUser);
    return true;
}

// ---------------------------------------------------------------------------
//  stop_proxy
// ---------------------------------------------------------------------------
static void stop_proxy(ProxyState* st)
{
    if (!st->running.load())
    {
        log_line("代理未运行!");
        return;
    }
    log_line("正在停止代理...");
    st->running.store(false);

    net_sock ls = st->listenSock;
    if (ls != kInvalidSock) net_close(ls);
    st->listenSock = kInvalidSock;

    hosts_remove_entries();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    log_line("[√] 代理已停止");
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------
static std::string ask(const char* prompt)
{
    std::string s;
    out_text(prompt);
    std::fflush(stdout);
    if (!std::getline(std::cin, s)) s.clear();
    while (!s.empty() && (s[s.size() - 1] == '\r' || s[s.size() - 1] == '\n'))
        s.erase(s.size() - 1);
    return s;
}

int main()
{
    console_init();

    if (!net_init())
    {
        out_text("[X] 网络初始化失败\n");
        return 1;
    }

    ProxyState st;
    st.listenPort = 25565;              // 本地监听端口，固定 25565

    out_text("========================================\n");
    out_text("           代理核心演示程序\n");
    out_text("========================================\n\n");

    std::string tHost = ask("目标服务器地址: ");
    std::string s = ask("目标端口 (默认25565): ");
    int tPort = s.empty() ? 25565 : std::atoi(s.c_str());
    std::string pIp = ask("代理IP地址: ");
    s = ask("代理端口 (默认1080): ");
    int pPort = s.empty() ? 1080 : std::atoi(s.c_str());
    std::string user = ask("代理用户名 (留空=不用账密): ");
    std::string pass = ask("代理密码   (留空=不用账密): ");

    if (tHost.empty() || pIp.empty() || pPort <= 0)
    {
        out_text("[X] 代理信息不完整，请重新输入\n");
        net_done();
        return 1;
    }

    if (!start_custom_proxy(&st, tHost, tPort, pIp, pPort, user, pass))
    {
        net_done();
        return 1;
    }

    out_text("\n按回车键停止代理并退出...\n");
    std::string dummy;
    std::getline(std::cin, dummy);
    stop_proxy(&st);

    net_done();
    return 0;
}
