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
// 代理核心演示（C# 版，与 proxy_core.cpp 逻辑一一对应，用于验证协议实现）
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
    }

    private static void Log(string s) { Console.WriteLine(s); }

    // ---------------- SOCKS5 握手 + CONNECT ----------------
    private static Socket ConnectSocks5(State st)
    {
        Socket s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        s.ReceiveTimeout = 10000;
        s.SendTimeout = 10000;

        try
        {
            IPAddress ip;
            if (!IPAddress.TryParse(st.ProxyIp, out ip))
            {
                IPAddress[] list = Dns.GetHostAddresses(st.ProxyIp);
                ip = null;
                foreach (IPAddress a in list)
                    if (a.AddressFamily == AddressFamily.InterNetwork) { ip = a; break; }
                if (ip == null) throw new Exception("resolve failed");
            }

            try { s.Connect(new IPEndPoint(ip, st.ProxyPort)); }
            catch (Exception ex)
            {
                Log("[DEBUG] 连接代理服务器 " + st.ProxyIp + ":" + st.ProxyPort + " 失败, 错误码: " + ex.Message);
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
            if (n != 2 || resp[0] != 0x05)
            {
                Log("[DEBUG] 握手响应无效, 收到 " + n + " 字节");
                s.Close();
                return null;
            }

            if (resp[1] == 0xFF) { Log("[DEBUG] 代理服务器拒绝所有请求的认证方法 (0xFF)"); s.Close(); return null; }
            if (resp[1] == 0x02 && !useAuth) { Log("[DEBUG] 代理服务器要求用户名密码，但未填写账号密码"); s.Close(); return null; }
            if (resp[1] != 0x00 && resp[1] != 0x02)
            {
                Log("[DEBUG] 代理服务器选择了不支持的认证方法: 0x" + resp[1].ToString("X2"));
                s.Close();
                return null;
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
                    Log("[DEBUG] 认证失败, 响应: " + n + " 字节");
                    s.Close();
                    return null;
                }
            }

            byte[] host = Encoding.UTF8.GetBytes(st.TargetHost);
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
                Log("[DEBUG] 读取CONNECT响应头失败, 已读 " + n);
                s.Close();
                return null;
            }
            if (head[1] != 0x00)
            {
                Log("[DEBUG] CONNECT请求被拒绝, 错误码: 0x" + head[1].ToString("X2"));
                s.Close();
                return null;
            }

            int rest;
            if (head[3] == 0x01) rest = 6;
            else if (head[3] == 0x04) rest = 18;
            else if (head[3] == 0x03)
            {
                byte[] l = new byte[1];
                if (RecvExact(s, l, 1) != 1) { Log("[DEBUG] 读取域名长度失败"); s.Close(); return null; }
                rest = l[0] + 2;
            }
            else { Log("[DEBUG] CONNECT响应地址类型未知: 0x" + head[3].ToString("X2")); s.Close(); return null; }

            byte[] tail = new byte[rest];
            if (RecvExact(s, tail, rest) != rest) { Log("[DEBUG] 读取CONNECT响应地址失败"); s.Close(); return null; }

            return s;
        }
        catch (Exception ex)
        {
            Log("[DEBUG] 异常: " + ex.Message);
            try { s.Close(); } catch { }
            return null;
        }
    }

    private static int RecvExact(Socket s, byte[] buf, int len)
    {
        int got = 0;
        while (got < len)
        {
            int r = s.Receive(buf, got, len - got, SocketFlags.None);
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
            try { client.Close(); } catch { }
            Interlocked.Decrement(ref st.ConnCount);
            return;
        }

        Thread t = new Thread(delegate () { Relay(st, client, up); });
        t.IsBackground = true;
        t.Start();

        Relay(st, up, client);   // 下游：代理 -> 本地
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
        try { ls.Bind(new IPEndPoint(IPAddress.Any, st.ListenPort)); ls.Listen(10); }
        catch (Exception ex) { Log("[X] 监听失败: " + ex.Message); return; }

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

    private static void HostsAdd(string host)
    {
        try { File.AppendAllText(HostsPath, "\n127.0.0.1 " + host + " # MC_PROXY", Encoding.ASCII); }
        catch (Exception ex) { Log("[警告] 写 hosts 失败: " + ex.Message); }
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
        catch (Exception ex) { Log("[警告] 清理 hosts 失败: " + ex.Message); }
    }

    // ---------------- 启动 / 停止 ----------------
    private static bool Start(State st, string tHost, int tPort, string pIp, int pPort, string user, string pass)
    {
        if (st.Running) { Log("[X] 代理已经在运行中!"); return false; }

        st.TargetHost = tHost; st.TargetPort = tPort;
        st.ProxyIp = pIp; st.ProxyPort = pPort;
        st.ProxyUser = user; st.ProxyPass = pass;

        Log("正在测试自定义代理连接...");
        Socket t = ConnectSocks5(st);
        if (t == null) { Log("[X] 代理连接测试失败，请检查代理信息是否正确"); return false; }
        t.Close();
        Log("[√] 代理连接测试成功");

        HostsAdd(st.TargetHost);

        st.Running = true;
        Thread th = new Thread(delegate () { Listener(st); });
        th.IsBackground = true;
        th.Start();
        Thread.Sleep(500);

        Log("[√] 自定义代理启动成功!");
        Log("[√] MC中连接地址: " + st.TargetHost);
        Log("[√] 使用代理: " + st.ProxyIp + ":" + st.ProxyPort);
        Log("[√] 用户名: " + st.ProxyUser);
        return true;
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

    private static int Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        State st = new State { ListenPort = 25565 };

        Console.WriteLine("========================================");
        Console.WriteLine("           代理核心演示程序");
        Console.WriteLine("========================================");
        Console.WriteLine();

        string tHost = Ask("目标服务器地址: ");
        string s = Ask("目标端口 (默认25565): ");
        int tPort = s.Length > 0 ? int.Parse(s) : 25565;
        string pIp = Ask("自己的代理IP地址: ");
        s = Ask("自己的代理端口: ");
        int pPort = s.Length > 0 ? int.Parse(s) : 1080;
        string user = Ask("代理用户名 (留空=不用账密): ");
        string pass = Ask("代理密码   (留空=不用账密): ");

        if (tHost.Length == 0 || pIp.Length == 0 || pPort <= 0)
        {
            Console.WriteLine("[X] 代理信息不完整，请重新输入");
            return 1;
        }

        if (!Start(st, tHost, tPort, pIp, pPort, user, pass)) return 1;

        Console.WriteLine();
        Console.WriteLine("按回车键停止代理并退出...");
        Console.ReadLine();
        Stop(st);
        return 0;
    }
}
