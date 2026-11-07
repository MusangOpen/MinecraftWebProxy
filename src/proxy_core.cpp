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
//  MinecraftWebProxy —— SOCKS5 代理转发核心
// ---------------------------------------------------------------------------
//  跨平台：Windows (winsock2) / Linux (POSIX socket)
//
//  用法：
//    proxy_core                                  交互输入
//    proxy_core <目标> <目标端口> <代理IP> <代理端口> [用户] [密码]
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
//  Windows 控制台默认 GBK(936)，直接输出 UTF-8 会乱码。
//  控制台 -> 转当前代码页；重定向 -> 原样 UTF-8。
// ===========================================================================
#ifdef _WIN32
static bool g_console_utf8 = true;

static void console_init()
{
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
    if (wlen <= 1) { std::fputs(utf8, stdout); return; }
    std::vector<wchar_t> w((size_t)wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), wlen);

    int alen = ::WideCharToMultiByte(cp, 0, w.data(), -1, nullptr, 0, nullptr, nullptr);
    if (alen <= 1) { std::fputs(utf8, stdout); return; }
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

static void log_line(const std::string& s)
{
    out_text(s.c_str());
    out_text("\n");
    std::fflush(stdout);
}

static void pause_exit(const std::string& hint)
{
    out_text("\n");
    if (!hint.empty())
    {
        out_text(hint.c_str());
        out_text("\n");
    }
    out_text("按任意键退出...\n");
    std::fflush(stdout);
    // 清掉行缓冲里剩余内容，再等一个字符
    std::cin.clear();
    std::cin.sync();
    std::cin.get();
}

// ===========================================================================
//  代理状态
// ===========================================================================
struct ProxyState
{
    std::atomic<bool> running{ false };
    net_sock          listenSock{ kInvalidSock };
    std::string       proxyIp;
    std::string       proxyUser;
    std::string       proxyPass;
    std::string       targetHost;
    int               proxyPort{ 0 };
    int               targetPort{ 0 };
    int               listenPort{ 25565 };
    std::atomic<int>  connCount{ 0 };
};

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
//  socks5_connect
//    err 非空时写入失败原因
//    账号密码都空 -> 只申报无认证；有任意一个非空 -> 申报无认证 + 用户名密码
// ---------------------------------------------------------------------------
static net_sock socks5_connect(const ProxyState& st, std::string* err)
{
    const bool useAuth = !st.proxyUser.empty() || !st.proxyPass.empty();

    net_sock s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSock)
    {
        if (err) *err = "创建 socket 失败: " + std::to_string(net_err());
        return kInvalidSock;
    }

    net_set_timeout(s, 10000);

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    std::string portStr = std::to_string(st.proxyPort);
    int rc = ::getaddrinfo(st.proxyIp.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr)
    {
        if (err) *err = "代理地址解析失败: " + st.proxyIp + " (code " + std::to_string(rc) + ")";
        net_close(s);
        return kInvalidSock;
    }

    rc = ::connect(s, res->ai_addr, (int)res->ai_addrlen);
    if (rc != 0)
    {
        if (err) *err = "连不上代理服务器 " + st.proxyIp + ":" + portStr +
                        " (code " + std::to_string(net_err()) + ")";
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
        if (err) *err = "发送握手数据失败";
        net_close(s);
        return kInvalidSock;
    }

    unsigned char resp[2] = { 0, 0 };
    int n = net_recv(s, resp, 2);
    if (n < 2)
    {
        if (err) *err = "代理服务器没有回应 SOCKS5 握手（收到 " + std::to_string(n) +
                        " 字节），这个地址端口可能不是 SOCKS5 代理";
        net_close(s);
        return kInvalidSock;
    }
    if (resp[0] != 0x05)
    {
        if (err) *err = "对端不是 SOCKS5 代理（VER=" + hex2(resp[0]) +
                        "），这个地址端口大概填错了";
        net_close(s);
        return kInvalidSock;
    }

    if (resp[1] == 0xFF)
    {
        if (err) *err = "代理服务器拒绝所有认证方法 (0xFF)";
        net_close(s);
        return kInvalidSock;
    }
    if (resp[1] == 0x02 && !useAuth)
    {
        if (err) *err = "代理服务器要求用户名密码，但没填账号密码";
        net_close(s);
        return kInvalidSock;
    }
    if (resp[1] != 0x00 && resp[1] != 0x02)
    {
        if (err) *err = "代理服务器选中了不支持的认证方法: " + hex2(resp[1]);
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
            if (err) *err = "发送认证数据失败";
            net_close(s);
            return kInvalidSock;
        }

        unsigned char ack[2] = { 0, 0 };
        n = net_recv(s, ack, 2);
        if (n != 2 || ack[1] != 0x00)
        {
            if (err) *err = "代理用户名或密码错误";
            net_close(s);
            return kInvalidSock;
        }
    }

    // ---- 3. CONNECT（固定 ATYP=0x03 域名方式）------------------------------
    if (st.targetHost.empty() || st.targetHost.size() > 255)
    {
        if (err) *err = "目标地址为空或太长（上限 255 字节）";
        net_close(s);
        return kInvalidSock;
    }

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
        if (err) *err = "发送 CONNECT 请求失败";
        net_close(s);
        return kInvalidSock;
    }

    // ---- 4. 读回复头 -------------------------------------------------------
    unsigned char head[4];
    n = 0;
    while (n < 4)
    {
        int r = net_recv(s, head + n, 4 - n);
        if (r < 1) break;
        n += r;
    }
    if (n != 4)
    {
        if (err) *err = "读取 CONNECT 回复失败（" + std::to_string(n) + "/4 字节）";
        net_close(s);
        return kInvalidSock;
    }

    if (head[1] != 0x00)
    {
        std::string msg = "代理拒绝了到 " + st.targetHost + ":" + std::to_string(st.targetPort) +
                          " 的 CONNECT（错误码 " + hex2(head[1]);
        const char* why = socks5_reply_text(head[1]);
        if (why) msg += std::string(" ") + why;
        msg += "）";
        if (err) *err = msg;
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
            if (err) *err = "读取域名长度失败";
            net_close(s);
            return kInvalidSock;
        }
        rest = (int)len + 2;
    }
    else
    {
        if (err) *err = "CONNECT 回复里的地址类型未知: " + hex2(atyp);
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
        if (err) *err = "读取 CONNECT 回复的地址部分失败";
        net_close(s);
        return kInvalidSock;
    }

    return s;
}

// ---------------------------------------------------------------------------
//  转发
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

static void handle_client(ProxyState* st, net_sock client)
{
    st->connCount.fetch_add(1);

    std::string err;
    net_sock upstream = socks5_connect(*st, &err);
    if (upstream == kInvalidSock)
    {
        if (!err.empty()) log_line("[!] " + err);
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
//  监听
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
        log_line("[X] 监听 0.0.0.0:" + std::to_string(st->listenPort) + " 失败");
        log_line("    端口可能被占用（比如另一个代理已经在跑），先关掉占用的程序");
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
//  hosts
// ---------------------------------------------------------------------------
#ifdef _WIN32
static const char* kHostsPath = "C:\\Windows\\System32\\drivers\\etc\\hosts";
#else
static const char* kHostsPath = "/etc/hosts";
#endif
static const char* kHostsMark = "# MC_PROXY";

static void hosts_add_entry(const std::string& host, int listenPort)
{
    FILE* f = std::fopen(kHostsPath, "a+");
    if (!f)
    {
        log_line("[警告] 写 hosts 失败（需要管理员/root 权限）");
        log_line("        代理本身不受影响，游戏里直接连 127.0.0.1:" +
                 std::to_string(listenPort) + " 即可");
        return;
    }
    std::fputs("\n127.0.0.1 ", f);
    std::fputs(host.c_str(), f);
    std::fputs(" # MC_PROXY", f);
    std::fflush(f);
    std::fclose(f);
    log_line("[√] 已写入 hosts: 127.0.0.1 " + host);
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
//  启动 / 停止
// ---------------------------------------------------------------------------
static bool start_proxy(ProxyState* st,
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

    log_line("");
    log_line("目标服务器 : " + targetHost + ":" + std::to_string(targetPort));
    log_line("代理服务器 : " + proxyIp + ":" + std::to_string(proxyPort) +
             ((!user.empty() || !pass.empty()) ? " (用户名密码)" : " (无认证)"));
    log_line("");
    log_line("正在测试连接...");

    std::string err;
    net_sock t = socks5_connect(*st, &err);
    if (t == kInvalidSock)
    {
        log_line("[X] 连接测试失败");
        if (!err.empty()) log_line("    原因: " + err);
        return false;
    }
    net_close(t);
    log_line("[√] 代理连接测试成功");

    hosts_add_entry(st->targetHost, st->listenPort);

    st->running.store(true);
    std::thread(listener_thread, st).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    log_line("");
    log_line("[√] 代理已启动");
    log_line("[√] 游戏里连接: 127.0.0.1:" + std::to_string(st->listenPort));
    return true;
}

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

static int parse_or(const std::string& s, int fallback)
{
    if (s.empty()) return fallback;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return fallback;
    return (int)v;
}

int main(int argc, char** argv)
{
    console_init();

    if (!net_init())
    {
        out_text("[X] 网络初始化失败\n");
        return 1;
    }

    ProxyState st;
    st.listenPort = 25565;

    out_text("========================================\n");
    out_text("           代理核心演示程序\n");
    out_text("========================================\n\n");

    std::string tHost, pIp, user, pass;
    int tPort, pPort;

    if (argc >= 5)
    {
        tHost = argv[1];
        tPort = parse_or(argv[2], 0);
        pIp   = argv[3];
        pPort = parse_or(argv[4], 0);
        user  = (argc >= 6) ? argv[5] : "";
        pass  = (argc >= 7) ? argv[6] : "";
    }
    else
    {
        out_text("目标服务器 = 你要连的 Minecraft 服务器\n");
        out_text("代理服务器 = 你自己的 SOCKS5 代理（不是 MC 服务器）\n\n");

        tHost = ask("目标服务器地址: ");
        tPort = parse_or(ask("目标端口 (默认25565): "), 25565);
        pIp   = ask("SOCKS5代理IP/域名: ");
        pPort = parse_or(ask("SOCKS5代理端口 (默认1080): "), 1080);
        user  = ask("代理用户名 (留空=不用账密): ");
        pass  = ask("代理密码   (留空=不用账密): ");
    }

    if (tHost.empty() || pIp.empty() || tPort <= 0 || tPort > 65535 || pPort <= 0 || pPort > 65535)
    {
        log_line("[X] 参数不完整或端口不合法（端口范围 1-65535）");
        pause_exit("");
        net_done();
        return 1;
    }

    // 目标服务器和代理服务器填成同一个，几乎肯定把 MC 服务器当代理了
    if (tHost == pIp && tPort == pPort)
    {
        log_line("");
        log_line("[X] 「目标服务器」和「SOCKS5代理」填成了同一个地址: " + tHost + ":" + std::to_string(tPort));
        log_line("");
        log_line("    目标服务器 = 你要玩的 Minecraft 服务器，比如 colorgarden.top:55555");
        log_line("    SOCKS5代理 = 你自己的代理服务器，通常是另一台机器，比如 1.2.3.4:1080");
        log_line("");
        log_line("    如果你想直连这个 MC 服务器，根本不需要本程序，直接用游戏连就行。");
        log_line("    如果你想通过代理连，需要先有一个 SOCKS5 代理服务。");
        pause_exit("");
        net_done();
        return 1;
    }

    if (!start_proxy(&st, tHost, tPort, pIp, pPort, user, pass))
    {
        pause_exit("常见原因：\n"
                   "  1. 「SOCKS5代理IP/端口」填成了 Minecraft 服务器地址，应该填你自己的 SOCKS5 代理\n"
                   "  2. 代理服务器没开、端口不对、被防火墙拦了\n"
                   "  3. 代理要求账号密码但没填");
        net_done();
        return 1;
    }

    out_text("\n按回车键停止代理并退出...\n");
    std::fflush(stdout);
    std::string dummy;
    std::getline(std::cin, dummy);
    stop_proxy(&st);

    net_done();
    return 0;
}
