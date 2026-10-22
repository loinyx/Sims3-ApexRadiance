// Runner side of the in-game test agent's mailbox (agent/Agent.cs, class Mailbox): finds the 1 MB byte[] in TS3W.exe by
// its magic header and exchanges one JSON request and reply through ReadProcessMemory / WriteProcessMemory. Loaded by
// run.ps1 with Add-Type; C# 5, so it compiles in Windows PowerShell 5.1 and PowerShell 7. Mailbox layout after ts3agent
// (sims3-modernization, MIT), forked from Sims3-MCP (MIT).
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace ApexIngame
{
    public sealed class MailboxClient : IDisposable
    {
        const int Header = 64, ReqCap = 64 * 1024, Capacity = 1024 * 1024, Version = 1;
        const uint Salt = 0x41505854;
        const int OffVersion = 16, OffNonce = 20, OffReqSeq = 24, OffRespSeq = 28, OffReqLen = 32, OffRespLen = 36,
                  OffCapacity = 40, OffCheck = 44, OffHeartbeat = 48, OffState = 56;
        static readonly byte[] Magic = Encoding.ASCII.GetBytes("APEXTEST-MBOX-01");

        [StructLayout(LayoutKind.Sequential)]
        struct MemoryBasicInformation
        {
            public IntPtr BaseAddress, AllocationBase;
            public uint AllocationProtect;
            public IntPtr RegionSize;
            public uint State, Protect, Type;
        }

        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr written);
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr addr, out MemoryBasicInformation mbi, IntPtr size);

        const uint ProcessVmRead = 0x10, ProcessVmWrite = 0x20, ProcessVmOperation = 0x8, ProcessQueryInformation = 0x400;
        const uint MemCommit = 0x1000, PageWritable = 0x04 | 0x08 | 0x40 | 0x80, PageGuard = 0x100;

        readonly IntPtr mProcess;
        long mAddress; // 0 until found
        uint mNextId;

        public MailboxClient(int pid)
        {
            mProcess = OpenProcess(ProcessVmRead | ProcessVmWrite | ProcessVmOperation | ProcessQueryInformation, false, pid);
            if (mProcess == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess " + pid);
        }

        public void Dispose() { CloseHandle(mProcess); }

        public bool Found { get { return mAddress != 0 && Valid(mAddress); } }

        byte[] Read(long addr, int size)
        {
            byte[] buf = new byte[size];
            IntPtr got;
            if (!ReadProcessMemory(mProcess, new IntPtr(addr), buf, new IntPtr(size), out got) || got.ToInt64() != size) return null;
            return buf;
        }

        void Write(long addr, byte[] data)
        {
            IntPtr put;
            if (!WriteProcessMemory(mProcess, new IntPtr(addr), data, new IntPtr(data.Length), out put) || put.ToInt64() != data.Length)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "WriteProcessMemory");
        }

        uint U32(long addr)
        {
            byte[] b = Read(addr, 4);
            if (b == null) throw new InvalidOperationException("mailbox no longer readable");
            return BitConverter.ToUInt32(b, 0);
        }

        // The header check of Mailbox.Open: magic, version, capacity and the check word over the nonce.
        public static bool HeaderValid(byte[] h)
        {
            if (h == null || h.Length < Header) return false;
            for (int i = 0; i < Magic.Length; i++) if (h[i] != Magic[i]) return false;
            uint nonce = BitConverter.ToUInt32(h, OffNonce), cap = BitConverter.ToUInt32(h, OffCapacity);
            return BitConverter.ToUInt32(h, OffVersion) == Version && cap == Capacity &&
                   (((uint)Version ^ nonce ^ cap ^ Salt) == BitConverter.ToUInt32(h, OffCheck));
        }

        bool Valid(long addr) { return HeaderValid(Read(addr, Header)); }

        // One pass over the committed writable regions of at least 1 MB, of any type and with or without execute (the game's
        // GC heap is PAGE_EXECUTE_READWRITE), up to the end of the target's address space; the same ranges as Frida's
        // enumerateRanges('rw-') that the sims3-modernization probe scans. Returns true once found.
        public bool Scan()
        {
            if (Found) return true;
            mAddress = 0;
            long addr = 0x10000;
            MemoryBasicInformation mbi;
            IntPtr mbiSize = new IntPtr(Marshal.SizeOf(typeof(MemoryBasicInformation)));
            byte[] chunk = new byte[4 * 1024 * 1024];
            while (VirtualQueryEx(mProcess, new IntPtr(addr), out mbi, mbiSize) != IntPtr.Zero)
            {
                long size = mbi.RegionSize.ToInt64();
                if (size <= 0) break;
                if (mbi.State == MemCommit && (mbi.Protect & PageWritable) != 0 && (mbi.Protect & PageGuard) == 0 && size >= Capacity)
                {
                    for (long off = 0; off < size; off += chunk.Length - Magic.Length)
                    {
                        int n = (int)Math.Min(chunk.Length, size - off);
                        IntPtr got;
                        if (!ReadProcessMemory(mProcess, new IntPtr(addr + off), chunk, new IntPtr(n), out got)) break;
                        int len = (int)got.ToInt64();
                        for (int i = IndexOf(chunk, len, 0); i >= 0; i = IndexOf(chunk, len, i + 1))
                            if (Valid(addr + off + i)) { mAddress = addr + off + i; return true; }
                        if (n < chunk.Length) break;
                    }
                }
                addr += size;
            }
            return false;
        }

        static int IndexOf(byte[] buf, int len, int start)
        {
            for (int i = start; i <= len - Magic.Length; i++)
            {
                if (buf[i] != Magic[0]) continue;
                int j = 1;
                while (j < Magic.Length && buf[i + j] == Magic[j]) j++;
                if (j == Magic.Length) return i;
            }
            return -1;
        }

        // Ticks of the agent's poll task; it moves while the game's simulator runs.
        public uint Heartbeat { get { return U32(mAddress + OffHeartbeat); } }
        public bool WorldLoaded { get { return U32(mAddress + OffState) == 1; } }

        // Sends one request and waits for the reply (the game answers on its next tick). Returns the reply JSON.
        public string Call(string cmd, string argsJson, int timeoutMs)
        {
            if (!Found && !Scan()) throw new InvalidOperationException("agent mailbox not found");
            if (U32(mAddress + OffReqSeq) != U32(mAddress + OffRespSeq)) throw new InvalidOperationException("previous request still pending");
            string text = "{\"id\":" + (++mNextId) + ",\"cmd\":\"" + cmd + "\"" + (string.IsNullOrEmpty(argsJson) ? "" : ",\"args\":" + argsJson) + "}";
            byte[] data = Encoding.UTF8.GetBytes(text);
            if (data.Length > ReqCap) throw new ArgumentException("request too large");
            Write(mAddress + Header, data);
            Write(mAddress + OffReqLen, BitConverter.GetBytes((uint)data.Length));
            uint seq = U32(mAddress + OffReqSeq) + 1;
            Write(mAddress + OffReqSeq, BitConverter.GetBytes(seq)); // publish last
            DateTime end = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < end)
            {
                if (U32(mAddress + OffRespSeq) == seq)
                {
                    int len = (int)U32(mAddress + OffRespLen);
                    byte[] reply = Read(mAddress + Header + ReqCap, len);
                    if (reply == null) throw new InvalidOperationException("reply not readable");
                    return Encoding.UTF8.GetString(reply);
                }
                System.Threading.Thread.Sleep(50);
            }
            throw new TimeoutException("no reply to '" + cmd + "' in " + timeoutMs + " ms (the simulator is not ticking)");
        }
    }
}
