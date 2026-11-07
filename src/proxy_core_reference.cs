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
// 代理核心演示（C# 版，与 proxy_core.cpp 逻辑一一对应）
//
// 用法：
//   代理核心演示.exe                                  交互输入
//   代理核心演示.exe <目标> <目标端口> <代理IP> <代理端口> [用户] [密码]
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

internal static class ProxyCore
{
    private sealed class State
    {
        public volatile bool Running;
        public Socket Listen;
        public string ProxyIp = "";
        public string ProxyUser = "";
        public string ProxyPass = "";
        public string TargetHost = "";
        public int ProxyPort;
        public int TargetPort;
        public int ListenPort = 25565;
        public int ConnCount;
        public string LastError = "";
    }

    private static void Log(string s) { Console.WriteLine(s); }

    private static void Pause(string hint)
    {
        Console.WriteLine();
        if (hint != null && hint.Length > 0) Console.WriteLine(hint);
        Console.WriteLine("按任意键退出...");
        try { Console.ReadKey(true); } catch { }
    }

    // ---------------- SOCKS5 握手 + CONNECT ----------------
    private static Socket ConnectSocks5(State st)
    {
        st.LastError = "";

        Socket s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        s.ReceiveTimeout = 10000;
        s.SendTimeout = 10000;

        try
        {
            IPAddress ip;
            if (!IPAddress.TryParse(st.ProxyIp, out ip))
            {
                IPAddress[] list;
                try { list = Dns.GetHostAddresses(st.ProxyIp); }
                catch (Exception ex)
                {
                    st.LastError = "代理地址解析失败: " + st.ProxyIp + " (" + ex.Message + ")";
                    s.Close();
                    return null;
                }
                ip = null;
                foreach (IPAddress a in list)
                    if (a.AddressFamily == AddressFamily.InterNetwork) { ip = a; break; }
                if (ip == null)
                {
                    st.LastError = "代理地址没有 IPv4 记录: " + st.ProxyIp;
                    s.Close();
                    return null;
                }
            }

            try { s.Connect(new IPEndPoint(ip, st.ProxyPort)); }
            catch (Exception ex)
            {
                st.LastError = "连不上代理服务器 " + st.ProxyIp + ":" + st.ProxyPort + " (" + ex.Message + ")";
                s.Close();
                return null;
            }

            // 账号密码留空 -> 只申报无认证；有任意一个非空 -> 申报无认证 + 用户名密码
            bool useAuth = st.ProxyUser.Length > 0 || st.ProxyPass.Length > 0;
            byte[] hello = useAuth
                ? new byte[] { 0x05, 0x02, 0x00, 0x02 }
                : new byte[] { 0x05, 0x01, 0x00 };
            s.Send(hello);

            byte[] resp = new byte[2];
            int n = RecvExact(s, resp, 2);
            if (n < 2)
            {
                st.LastError = "代理服务器没有回应 SOCKS5 握手（收到 " + n + " 字节），"
                             + "这个地址端口可能不是 SOCKS5 代理";
                s.Close();
                return null;
            }
            if (resp[0] != 0x05)
            {
                st.LastError = "对端不是 SOCKS5 代理（VER=0x" + resp[0].ToString("X2")
                             + "），这个地址端口大概填错了";
                s.Close();
                return null;
            }

            if (resp[1] == 0xFF) { st.LastError = "代理服务器拒绝所有认证方法 (0xFF)"; s.Close(); return null; }
            if (resp[1] == 0x02 && !useAuth)
            {
                st.LastError = "代理服务器要求用户名密码，但没填账号密码";
                s.Close(); return null;
            }
            if (resp[1] != 0x00 && resp[1] != 0x02)
            {
                st.LastError = "代理服务器选中了不支持的认证方法: 0x" + resp[1].ToString("X2");
                s.Close(); return null;
            }

            if (resp[1] == 0x02)
            {
                byte[] u = Encoding.UTF8.GetBytes(st.ProxyUser);
                byte[] p = Encoding.UTF8.GetBytes(st.ProxyPass);
                byte[] auth = new byte[3 + u.Length + p.Length];
                auth[0] = 0x01;
                auth[1] = (byte)u.Length;
                Array.Copy(u, 0, auth, 2, u.Length);
                auth[2 + u.Length] = (byte)p.Length;
                Array.Copy(p, 0, auth, 3 + u.Length, p.Length);
                s.Send(auth);

                byte[] ack = new byte[2];
                n = RecvExact(s, ack, 2);
                if (n != 2 || ack[1] != 0x00)
                {
                    st.LastError = "代理用户名或密码错误";
                    s.Close();
                    return null;
                }
            }

            byte[] host = Encoding.UTF8.GetBytes(st.TargetHost);
            if (host.Length > 255)
            {
                st.LastError = "目标地址太长（超过 255 字节）";
                s.Close();
                return null;
            }

            byte[] req = new byte[7 + host.Length];
            req[0] = 0x05; req[1] = 0x01; req[2] = 0x00; req[3] = 0x03;
            req[4] = (byte)host.Length;
            Array.Copy(host, 0, req, 5, host.Length);
            req[5 + host.Length] = (byte)((st.TargetPort >> 8) & 0xFF);
            req[6 + host.Length] = (byte)(st.TargetPort & 0xFF);
            s.Send(req);

            byte[] head = new byte[4];
            n = RecvExact(s, head, 4);
            if (n != 4)
            {
                st.LastError = "读取 CONNECT 回复失败（收到 " + n + " / 4 字节）";
                s.Close();
                return null;
            }
            if (head[1] != 0x00)
            {
                string[] why = new string[]
                {
                    "成功", "一般性失败", "规则不允许", "网络不可达", "主机不可达",
                    "连接被拒绝", "TTL过期", "不支持的命令", "不支持的地址类型"
                };
                string desc = head[1] < why.Length ? why[head[1]] : "未知";
                st.LastError = "代理拒绝了到 " + st.TargetHost + ":" + st.TargetPort
                             + " 的 CONNECT（错误码 0x" + head[1].ToString("X2") + " " + desc + "）";
                s.Close();
                return null;
            }

            int rest;
            if (head[3] == 0x01) rest = 6;
            else if (head[3] == 0x04) rest = 18;
            else if (head[3] == 0x03)
            {
                byte[] l = new byte[1];
                if (RecvExact(s, l, 1) != 1) { st.LastError = "读取域名长度失败"; s.Close(); return null; }
                rest = l[0] + 2;
            }
            else
            {
                st.LastError = "CONNECT 回复里的地址类型未知: 0x" + head[3].ToString("X2");
                s.Close();
                return null;
            }

            byte[] tail = new byte[rest];
            if (RecvExact(s, tail, rest) != rest)
            {
                st.LastError = "读取 CONNECT 回复的地址部分失败";
                s.Close();
                return null;
            }

            return s;
        }
        catch (Exception ex)
        {
            st.LastError = "代理连接异常: " + ex.Message;
            try { s.Close(); } catch { }
            return null;
        }
    }

    private static int RecvExact(Socket s, byte[] buf, int len)
    {
        int got = 0;
        while (got < len)
        {
            int r;
            try { r = s.Receive(buf, got, len - got, SocketFlags.None); }
            catch (SocketException) { break; }
            if (r < 1) break;
            got += r;
        }
        return got;
    }

    // ---------------- 双向转发 ----------------
    private static void HandleClient(State st, Socket client)
    {
        Interlocked.Increment(ref st.ConnCount);

        Socket up = ConnectSocks5(st);
        if (up == null)
        {
            if (st.LastError.Length > 0) Log("[!] " + st.LastError);
            try { client.Close(); } catch { }
            Interlocked.Decrement(ref st.ConnCount);
            return;
        }

        Thread t = new Thread(delegate () { Relay(st, client, up); });
        t.IsBackground = true;
        t.Start();

        Relay(st, up, client);
        try { t.Join(2000); } catch { }

        try { up.Close(); } catch { }
        try { client.Close(); } catch { }
        Interlocked.Decrement(ref st.ConnCount);
    }

    private static void Relay(State st, Socket from, Socket to)
    {
        byte[] buf = new byte[4096];
        try
        {
            while (st.Running)
            {
                int n = from.Receive(buf);
                if (n < 1) break;
                to.Send(buf, 0, n, SocketFlags.None);
            }
        }
        catch { }
    }

    // ---------------- 监听 ----------------
    private static void Listener(State st)
    {
        Socket ls = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        st.Listen = ls;
        ls.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        try
        {
            ls.Bind(new IPEndPoint(IPAddress.Any, st.ListenPort));
            ls.Listen(10);
        }
        catch (Exception ex)
        {
            Log("[X] 监听 0.0.0.0:" + st.ListenPort + " 失败: " + ex.Message);
            Log("    端口可能被占用（比如另一个代理已经在跑），换个端口或先关掉占用的程序");
            return;
        }

        while (st.Running)
        {
            try
            {
                if (!ls.Poll(10000, SelectMode.SelectRead)) continue;
                Socket c = ls.Accept();
                if (!st.Running) { c.Close(); break; }
                Thread th = new Thread(delegate () { HandleClient(st, c); });
                th.IsBackground = true;
                th.Start();
            }
            catch { if (!st.Running) break; }
        }
        try { ls.Close(); } catch { }
        st.Listen = null;
    }

    // ---------------- hosts ----------------
    private static readonly string HostsPath = @"C:\Windows\System32\drivers\etc\hosts";

    private static void HostsAdd(string host, int listenPort)
    {
        try
        {
            File.AppendAllText(HostsPath, "\n127.0.0.1 " + host + " # MC_PROXY", Encoding.ASCII);
            Log("[√] 已写入 hosts: 127.0.0.1 " + host);
        }
        catch (Exception ex)
        {
            Log("[警告] 写 hosts 失败（需要管理员权限）: " + ex.Message);
            Log("        代理本身不受影响，游戏里直接连 127.0.0.1:" + listenPort + " 即可");
        }
    }

    private static void HostsRemove()
    {
        try
        {
            string[] lines = File.ReadAllLines(HostsPath);
            StringBuilder sb = new StringBuilder();
            foreach (string l in lines)
            {
                if (l.Contains("# MC_PROXY")) continue;
                sb.AppendLine(l);
            }
            File.WriteAllText(HostsPath, sb.ToString(), Encoding.ASCII);
        }
        catch { }
    }

    // ---------------- 启动 / 停止 ----------------
    private static bool Start(State st, string tHost, int tPort, string pIp, int pPort, string user, string pass)
    {
        if (st.Running) { Log("[X] 代理已经在运行中!"); return false; }

        st.TargetHost = tHost; st.TargetPort = tPort;
        st.ProxyIp = pIp; st.ProxyPort = pPort;
        st.ProxyUser = user; st.ProxyPass = pass;

        Log("");
        Log("目标服务器 : " + tHost + ":" + tPort);
        Log("代理服务器 : " + pIp + ":" + pPort
            + ((user.Length > 0 || pass.Length > 0) ? " (用户名密码)" : " (无认证)"));
        Log("");
        Log("正在测试连接...");

        Socket t = ConnectSocks5(st);
        if (t == null)
        {
            Log("[X] 连接测试失败");
            if (st.LastError.Length > 0) Log("    原因: " + st.LastError);
            return false;
        }
        t.Close();
        Log("[√] 代理连接测试成功");

        HostsAdd(st.TargetHost, st.ListenPort);

        st.Running = true;
        Thread th = new Thread(delegate () { Listener(st); });
        th.IsBackground = true;
        th.Start();
        Thread.Sleep(600);

        Log("");
        Log("[√] 代理已启动");
        Log("[√] 游戏里连接: 127.0.0.1:" + st.ListenPort);

        ProbeLocal(st.ListenPort);
        return true;
    }

    // 探测本地监听是否真的起来了
    private static void ProbeLocal(int port)
    {
        try
        {
            using (Socket probe = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp))
            {
                probe.ReceiveTimeout = 3000;
                probe.Connect(new IPEndPoint(IPAddress.Loopback, port));
                Log("[√] 本地端口 " + port + " 已可连接");
            }
        }
        catch (Exception ex)
        {
            Log("[!] 本地 " + port + " 连不上: " + ex.Message);
        }
    }

    private static void Stop(State st)
    {
        if (!st.Running) { Log("代理未运行!"); return; }
        Log("正在停止代理...");
        st.Running = false;
        Socket ls = st.Listen;
        if (ls != null) { try { ls.Close(); } catch { } }
        st.Listen = null;
        HostsRemove();
        Thread.Sleep(300);
        Log("[√] 代理已停止");
    }

    private static string Ask(string prompt)
    {
        Console.Write(prompt);
        string s = Console.ReadLine();
        return s == null ? "" : s.Trim();
    }

    private static int ParseOr(string s, int fallback)
    {
        int v;
        return int.TryParse(s, out v) ? v : fallback;
    }

    private static int Main(string[] args)
    {
        try
        {
            try { Console.OutputEncoding = Encoding.UTF8; } catch { }
            return Run(args);
        }
        catch (Exception ex)
        {
            Console.WriteLine();
            Console.WriteLine("[X] 程序异常退出: " + ex.GetType().Name + ": " + ex.Message);
            Console.WriteLine(ex.StackTrace);
            Pause("");
            return 99;
        }
    }

    private static int Run(string[] args)
    {
        State st = new State { ListenPort = 25565 };

        Console.WriteLine("========================================");
        Console.WriteLine("           代理核心演示程序");
        Console.WriteLine("========================================");
        Console.WriteLine();

        string tHost, pIp, user, pass;
        int tPort, pPort;

        if (args.Length >= 4)
        {
            tHost = args[0];
            tPort = ParseOr(args[1], 0);
            pIp   = args[2];
            pPort = ParseOr(args[3], 0);
            user  = args.Length >= 5 ? args[4] : "";
            pass  = args.Length >= 6 ? args[5] : "";
        }
        else
        {
            Console.WriteLine("目标服务器 = 你要连的 Minecraft 服务器");
            Console.WriteLine("代理服务器 = 你自己的 SOCKS5 代理（不是 MC 服务器）");
            Console.WriteLine();

            tHost = Ask("目标服务器地址: ");
            tPort = ParseOr(Ask("目标端口 (默认25565): "), 25565);
            pIp   = Ask("SOCKS5代理IP/域名: ");
            pPort = ParseOr(Ask("SOCKS5代理端口 (默认1080): "), 1080);
            user  = Ask("代理用户名 (留空=不用账密): ");
            pass  = Ask("代理密码   (留空=不用账密): ");
        }

        if (tHost.Length == 0 || pIp.Length == 0 || tPort <= 0 || tPort > 65535 || pPort <= 0 || pPort > 65535)
        {
            Log("[X] 参数不完整或端口不合法（端口范围 1-65535）");
            Pause("");
            return 1;
        }

        // 目标服务器和代理服务器填成同一个，几乎肯定把 MC 服务器当代理了
        if (string.Equals(tHost, pIp, StringComparison.OrdinalIgnoreCase) && tPort == pPort)
        {
            Log("");
            Log("[X] 「目标服务器」和「SOCKS5代理」填成了同一个地址: " + tHost + ":" + tPort);
            Log("");
            Log("    目标服务器 = 你要玩的 Minecraft 服务器，比如 colorgarden.top:55555");
            Log("    SOCKS5代理 = 你自己的代理服务器，通常是另一台机器，比如 1.2.3.4:1080");
            Log("");
            Log("    如果你想直连这个 MC 服务器，根本不需要本程序，直接用游戏连就行。");
            Log("    如果你想通过代理连，需要先有一个 SOCKS5 代理服务。");
            Pause("");
            return 1;
        }

        if (!Start(st, tHost, tPort, pIp, pPort, user, pass))
        {
            Pause("常见原因：\n"
                + "  1. 「SOCKS5代理IP/端口」填成了 Minecraft 服务器地址，应该填你自己的 SOCKS5 代理\n"
                + "  2. 代理服务器没开、端口不对、被防火墙拦了\n"
                + "  3. 代理要求账号密码但没填");
            return 1;
        }

        Console.WriteLine();
        Console.WriteLine("按回车键停止代理并退出...");
        Console.ReadLine();
        Stop(st);
        return 0;
    }
}
