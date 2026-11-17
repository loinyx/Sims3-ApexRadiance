using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace ApexSetup;

// NVIDIA Smooth Motion (frame generation in the driver) for The Sims 3 under the RTX Remix bridge: a driver profile holding the
// bridge's 64-bit NvRemixBridge.exe (it presents the frames, through DXVK in Vulkan) and the game's exe, with setting 0xB0D384C0
// = 1, written through NVAPI's driver settings (DRS) as NVIDIA Profile Inspector writes it. Nothing here throws: no nvapi64.dll,
// no NVIDIA card or any NVAPI error reads as "not available", with the reason in Error.
public static class SmoothMotion
{
    public const string ProfileName = "Apex Radiance - The Sims 3";
    public const string BridgeExe = "NvRemixBridge.exe";
    public const uint SettingId = 0xB0D384C0, MinDriver = 57186; // 571.86

    // Card: the NVIDIA card's name (null: none). On: Apex's profile has the setting at 1. Owner: a profile of the player's
    // holding one of the exes (Setup leaves it alone). Error: NVAPI could not be read.
    public sealed record Status(string? Card, uint Driver, bool On, string? Owner, string? Error);

    // RTX 40 and 50 series (GeForce RTX 4060, RTX 5090 Laptop GPU), on a driver that has the setting
    public static bool Supported(string? card, uint driver) =>
        card is not null && Regex.IsMatch(card, @"RTX\s*(40|50)\d\d", RegexOptions.IgnoreCase) && driver >= MinDriver;

    public static string DriverText(uint driver) => $"{driver / 100}.{driver % 100:00}";

    // why the switch cannot be turned on (an English key for the UI's T), or null; turning off Apex's own profile always works
    public static (string Key, string Arg)? Blocked(Status s, bool bridge)
    {
        if (s.Error is not null) return ("Could not read the NVIDIA driver settings: {0}", s.Error);
        if (s.On) return null;
        if (s.Owner is not null) return ("Already in your NVIDIA profile \"{0}\": Setup leaves it as it is.", s.Owner);
        if (!Regex.IsMatch(s.Card ?? "", @"RTX\s*(40|50)\d\d", RegexOptions.IgnoreCase)) return ("Needs an NVIDIA GeForce RTX 40 or 50 series card (found: {0}).", s.Card ?? "");
        if (s.Driver < MinDriver) return ("Needs NVIDIA driver 571.86 or newer (found: {0}).", DriverText(s.Driver));
        // shortcut: DXVK on TS3W.exe is Vulkan too but untested with Smooth Motion; allow it once seen working in game
        if (!bridge) return ("Needs the RTX Remix bridge as the graphics choice.", "");
        return null;
    }

    // the bridge, this game's exe, then the other edition's (Steam TS3W.exe, EA app TS3.exe): one profile serves every copy of
    // the game on the PC, and turning it on for one never drops the other's entry. Only the first two can block (Owner).
    // no NVIDIA driver (no nvapi64.dll, or a leftover one without the export, or NVAPI_NVIDIA_DEVICE_NOT_FOUND): no card, no row;
    // anything else on an NVIDIA PC is an error shown on the row
    public static Status Failed(Exception e) => e is DllNotFoundException or EntryPointNotFoundException
        ? new(null, 0, false, null, null) : new(null, 0, false, null, e.Message);
    public static bool NoCard(int initResult) => initResult == -6;

    public static string[] Apps(string gameExe) => new[] { BridgeExe, gameExe }.Concat(new[] { "TS3W.exe", "TS3.exe" }.Where(e => !e.Equals(gameExe, StringComparison.OrdinalIgnoreCase))).ToArray();

    public static Status Read(string gameExe)
    {
        // test runs: APEX_SETUP_FAKE_NV = "<card>|<driver, 57186>|<on, 0 or 1>|<owner profile>" stands in for the driver
        if (Environment.GetEnvironmentVariable("APEX_SETUP_FAKE_NV") is { Length: > 0 } fake)
        {
            var p = fake.Split('|');
            return new(p[0].Length > 0 ? p[0] : null, p.Length > 1 && uint.TryParse(p[1], out var d) ? d : 0,
                       p.Length > 2 && p[2] == "1", p.Length > 3 && p[3].Length > 0 ? p[3] : null, null);
        }
        try { return Nv.Read(Apps(gameExe)); }
        catch (Exception e) { return Failed(e); }
    }

    // null when done, else why not; the dry run and the fake write nothing
    public static string? Set(bool on, string gameExe)
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1" || Environment.GetEnvironmentVariable("APEX_SETUP_FAKE_NV") is { Length: > 0 })
            return "Dry run: Setup changes nothing.";
        try { Nv.Set(on, Apps(gameExe)); return null; }
        catch (Exception e) { return e.Message; }
    }

    // NVAPI through nvapi_QueryInterface, with the IDs and struct versions of the public nvapi.h (R570). Strings are
    // NvAPI_UnicodeString, NvU16[2048]; versions are sizeof | (n << 16).
    static unsafe class Nv
    {
        const int Str = 4096;
        const int ProfileSize = 4 + Str + 16;                 // NVDRS_PROFILE_V1: version, profileName, gpuSupport, isPredefined, numOfApps, numOfSettings
        const int AppSize = 8 + 3 * Str;                      // NVDRS_APPLICATION_V1: version, isPredefined, appName, userFriendlyName, launcher
        const int SettingSize = 4 + Str + 20 + 2 * 4100;      // NVDRS_SETTING_V1: version, settingName, id, type, location, isCurrentPredefined, isPredefinedValid, 2 value unions
        const int SettingIdAt = 4 + Str, SettingCurrentAt = 4 + Str + 20 + 4100;
        const int ProfileNotFound = -163, ExeNotFound = -166, SettingNotFound = -160;

        [DllImport("nvapi64.dll", EntryPoint = "nvapi_QueryInterface", CallingConvention = CallingConvention.Cdecl)]
        static extern nint Query(uint id);

        static nint F(uint id) => Query(id) is var p && p != 0 ? p : throw new InvalidOperationException($"NVAPI 0x{id:X8} missing");
        static void Ok(int r, string what) { if (r != 0) throw new InvalidOperationException($"{what} {r}"); }
        static byte[] Versioned(int size) { var b = new byte[size]; BitConverter.TryWriteBytes(b, (uint)size | (1u << 16)); return b; }
        static byte[] Name(string s) { var b = new byte[Str]; Encoding.Unicode.GetBytes(s, 0, Math.Min(s.Length, 2047), b, 0); return b; }
        static string NameAt(byte[] b, int at) => Encoding.Unicode.GetString(b, at, Str).Split('\0')[0];

        static int InitCode() => ((delegate* unmanaged[Cdecl]<int>)F(0x0150E828))();
        static void Init() => Ok(InitCode(), "NvAPI_Initialize");

        static (string? Card, uint Driver) Card()
        {
            var gpus = new nint[64]; uint n = 0;
            fixed (nint* g = gpus) if (((delegate* unmanaged[Cdecl]<nint*, uint*, int>)F(0xE5AC921F))(g, &n) != 0 || n == 0) return (null, 0);
            string? best = null;
            var name = new byte[64];
            for (var i = 0; i < n; i++)
                fixed (byte* nm = name)
                    if (((delegate* unmanaged[Cdecl]<nint, byte*, int>)F(0xCEEE8E9F))(gpus[i], nm) == 0)
                    {
                        var s = Encoding.ASCII.GetString(name).Split('\0')[0];
                        if (best is null || Supported(s, MinDriver)) best = s;
                    }
            uint driver = 0; var branch = new byte[64];
            fixed (byte* br = branch) ((delegate* unmanaged[Cdecl]<uint*, byte*, int>)F(0x2926AAAD))(&driver, br);
            return (best, driver);
        }

        static T Session<T>(Func<nint, T> body)
        {
            nint h;
            Ok(((delegate* unmanaged[Cdecl]<nint*, int>)F(0x0694D52E))(&h), "DRS_CreateSession");
            try
            {
                Ok(((delegate* unmanaged[Cdecl]<nint, int>)F(0x375DBD6B))(h), "DRS_LoadSettings");
                return body(h);
            }
            finally { ((delegate* unmanaged[Cdecl]<nint, int>)F(0xDAD9CFF8))(h); }
        }

        static nint FindProfile(nint s, string name)
        {
            nint p; var n = Name(name);
            fixed (byte* np = n)
            {
                var r = ((delegate* unmanaged[Cdecl]<nint, byte*, nint*, int>)F(0x7E4A9A0B))(s, np, &p);
                if (r == ProfileNotFound) return 0;
                Ok(r, "DRS_FindProfileByName");
            }
            return p;
        }

        // the profile holding an exe: (name, predefined by NVIDIA), or null
        static (string Name, bool Predefined)? OwnerOf(nint s, string exe)
        {
            nint p; var n = Name(exe); var app = Versioned(AppSize);
            fixed (byte* np = n, ap = app)
            {
                var r = ((delegate* unmanaged[Cdecl]<nint, byte*, nint*, byte*, int>)F(0xEEE566B2))(s, np, &p, ap);
                if (r == ExeNotFound) return null;
                Ok(r, "DRS_FindApplicationByName");
            }
            var info = Versioned(ProfileSize);
            fixed (byte* ip = info) Ok(((delegate* unmanaged[Cdecl]<nint, nint, byte*, int>)F(0x61CD6FD6))(s, p, ip), "DRS_GetProfileInfo");
            return (NameAt(info, 4), BitConverter.ToUInt32(info, 4 + Str + 4) != 0);
        }

        static bool SettingOn(nint s, nint p)
        {
            var set = Versioned(SettingSize);
            fixed (byte* sp = set)
            {
                var r = ((delegate* unmanaged[Cdecl]<nint, nint, uint, byte*, int>)F(0x73BF8338))(s, p, SettingId, sp);
                if (r == SettingNotFound) return false;
                Ok(r, "DRS_GetSetting");
            }
            return BitConverter.ToUInt32(set, SettingCurrentAt) == 1;
        }

        // a profile of the player's (not Apex's, not NVIDIA's predefined one) holding an exe
        static string? PlayerOwner(nint s, string[] apps) =>
            apps.Take(2).Select(a => OwnerOf(s, a)).FirstOrDefault(o => o is { Predefined: false } x && x.Name != ProfileName)?.Name;

        public static Status Read(string[] apps)
        {
            if (InitCode() is var ir && NoCard(ir)) return new(null, 0, false, null, null);
            Ok(ir, "NvAPI_Initialize");
            var (card, driver) = Card();
            if (card is null) return new(null, driver, false, null, null);
            return Session(s => FindProfile(s, ProfileName) is var p && p != 0
                ? new Status(card, driver, SettingOn(s, p), PlayerOwner(s, apps), null)
                : new Status(card, driver, false, PlayerOwner(s, apps), null));
        }

        public static void Set(bool on, string[] apps)
        {
            Init();
            Session(s =>
            {
                var p = FindProfile(s, ProfileName);
                if (!on)
                {
                    if (p != 0) Ok(((delegate* unmanaged[Cdecl]<nint, nint, int>)F(0x17093206))(s, p), "DRS_DeleteProfile");
                }
                else
                {
                    if (PlayerOwner(s, apps) is { } owner) throw new InvalidOperationException($"Already in the profile \"{owner}\"");
                    if (p == 0)
                    {
                        var prof = Versioned(ProfileSize);
                        Encoding.Unicode.GetBytes(ProfileName, 0, ProfileName.Length, prof, 4);
                        fixed (byte* pp = prof) Ok(((delegate* unmanaged[Cdecl]<nint, byte*, nint*, int>)F(0xCC176068))(s, pp, &p), "DRS_CreateProfile");
                    }
                    foreach (var exe in apps)
                    {
                        // an exe in NVIDIA's own profile (the game's predefined one) stays there; the bridge's exe is the one that counts
                        if (OwnerOf(s, exe) is not null) continue;
                        var app = Versioned(AppSize);
                        Encoding.Unicode.GetBytes(exe, 0, exe.Length, app, 8);
                        fixed (byte* ap = app) Ok(((delegate* unmanaged[Cdecl]<nint, nint, byte*, int>)F(0x4347A9DE))(s, p, ap), "DRS_CreateApplication");
                    }
                    var set = Versioned(SettingSize);
                    BitConverter.TryWriteBytes(set.AsSpan(SettingIdAt), SettingId);          // settingType 0: NVDRS_DWORD_TYPE
                    BitConverter.TryWriteBytes(set.AsSpan(SettingCurrentAt), 1u);
                    fixed (byte* sp = set) Ok(((delegate* unmanaged[Cdecl]<nint, nint, byte*, int>)F(0x577DD202))(s, p, sp), "DRS_SetSetting");
                }
                Ok(((delegate* unmanaged[Cdecl]<nint, int>)F(0xFCBC7E14))(s), "DRS_SaveSettings");
                return 0;
            });
        }
    }
}
