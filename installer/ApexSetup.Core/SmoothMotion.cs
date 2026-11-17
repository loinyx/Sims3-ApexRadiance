using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace ApexSetup;

// NVIDIA Smooth Motion (frame generation in the driver) for The Sims 3 under the RTX Remix bridge: the bridge's 64-bit
// NvRemixBridge.exe presents the frames (DXVK in Vulkan). NVIDIA ships a predefined profile "NvRemixBridge" holding that exe
// with Smooth Motion - Enabled APIs (0xB0CC0875) = 0, which blocks it; NVIDIA Profile Inspector turns it on by editing that
// profile. Setup does the same through NVAPI's driver settings (DRS): on the profile that holds the bridge's exe (Setup's own
// "Apex Radiance - The Sims 3" when none does) it sets 0xB0D384C0 = 1 and 0xB0CC0875 = 7 (DX12, DX11 and Vulkan); off puts
// NVIDIA's defaults back (or deletes Setup's profile), and only when Setup turned it on (Marker), so a player's own edit stays.
// The game's exes are left in NVIDIA's own profiles: they do not present frames under the bridge. Nothing here throws.
public static class SmoothMotion
{
    public const string ProfileName = "Apex Radiance - The Sims 3";
    public const string BridgeExe = "NvRemixBridge.exe";
    public const uint SettingId = 0xB0D384C0, ApisId = 0xB0CC0875, AllApis = 7, MinDriver = 57186; // 571.86

    // Card: the NVIDIA card's name (null: none). On: the bridge's profile has 0xB0D384C0 = 1. Owner: a profile of the player's
    // holding the bridge's exe (Setup leaves it alone). Error: NVAPI could not be read.
    public sealed record Status(string? Card, uint Driver, bool On, string? Owner, string? Error);

    // the record that Setup turned it on (a path, so tests point it elsewhere)
    public static string Marker = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Apex Radiance Setup", "smooth-motion-on.txt");
    public static bool SetupTurnedOn => File.Exists(Marker);

    // RTX 40 and 50 series (GeForce RTX 4060, RTX 5090 Laptop GPU), on a driver that has the setting
    public static bool Supported(string? card, uint driver) =>
        card is not null && Regex.IsMatch(card, @"RTX\s*(40|50)\d\d", RegexOptions.IgnoreCase) && driver >= MinDriver;

    public static string DriverText(uint driver) => $"{driver / 100}.{driver % 100:00}";

    // why the switch cannot be used (an English key for the UI's T), or null
    public static (string Key, string Arg)? Blocked(Status s, bool bridge, bool setupTurnedOn)
    {
        if (s.Error is not null) return ("Could not read the NVIDIA driver settings: {0}", s.Error);
        if (s.Owner is not null) return ("Already in your NVIDIA profile \"{0}\": Setup leaves it as it is.", s.Owner);
        // on, but not by Setup (Profile Inspector): Setup does not undo it
        if (s.On) return setupTurnedOn ? null : ("Turned on outside Setup, in the NVIDIA driver settings: Setup leaves it as it is.", "");
        if (!Regex.IsMatch(s.Card ?? "", @"RTX\s*(40|50)\d\d", RegexOptions.IgnoreCase)) return ("Needs an NVIDIA GeForce RTX 40 or 50 series card (found: {0}).", s.Card ?? "");
        if (s.Driver < MinDriver) return ("Needs NVIDIA driver 571.86 or newer (found: {0}).", DriverText(s.Driver));
        // shortcut: DXVK on TS3W.exe is Vulkan too but untested with Smooth Motion; allow it once seen working in game
        if (!bridge) return ("Needs the RTX Remix bridge as the graphics choice.", "");
        return null;
    }

    // no NVIDIA driver (no nvapi64.dll, or a leftover one without the export, or NVAPI_NVIDIA_DEVICE_NOT_FOUND): no card, no row;
    // anything else on an NVIDIA PC is an error shown on the row
    public static Status Failed(Exception e) => e is DllNotFoundException or EntryPointNotFoundException
        ? new(null, 0, false, null, null) : new(null, 0, false, null, e.Message);
    public static bool NoCard(int initResult) => initResult == -6;

    // a record left while Smooth Motion reads off (a driver reinstall, the player's own change): dropped
    public static Status Tidy(Status s)
    {
        if (s is { Card: not null, Error: null, On: false } && File.Exists(Marker))
            try { File.Delete(Marker); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
        return s;
    }

    public static Status Read()
    {
        // test runs: APEX_SETUP_FAKE_NV = "<card>|<driver, 57186>|<on, 0 or 1>|<owner profile>" stands in for the driver
        if (Environment.GetEnvironmentVariable("APEX_SETUP_FAKE_NV") is { Length: > 0 } fake)
        {
            var p = fake.Split('|');
            return new(p[0].Length > 0 ? p[0] : null, p.Length > 1 && uint.TryParse(p[1], out var d) ? d : 0,
                       p.Length > 2 && p[2] == "1", p.Length > 3 && p[3].Length > 0 ? p[3] : null, null);
        }
        try { return Tidy(Nv.Read()); }
        catch (Exception e) { return Failed(e); }
    }

    // null when done, else why not; the dry run and the fake write nothing. Off touches only what Setup turned on.
    public static string? Set(bool on)
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1" || Environment.GetEnvironmentVariable("APEX_SETUP_FAKE_NV") is { Length: > 0 })
            return "Dry run: Setup changes nothing.";
        if (!on && !SetupTurnedOn) return null;
        try
        {
            // the record first: a crash after the driver call still lets maintenance turn it off
            if (on) { Directory.CreateDirectory(Path.GetDirectoryName(Marker)!); File.WriteAllText(Marker, "on"); }
            Nv.Set(on);
            if (!on) File.Delete(Marker);
            return null;
        }
        catch (Exception e)
        {
            if (on) try { File.Delete(Marker); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
            return e.Message;
        }
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
        const int ExeNotFound = -166, SettingNotFound = -160;

        [DllImport("nvapi64.dll", EntryPoint = "nvapi_QueryInterface", CallingConvention = CallingConvention.Cdecl)]
        static extern nint Query(uint id);

        static nint F(uint id) => Query(id) is var p && p != 0 ? p : throw new InvalidOperationException($"NVAPI 0x{id:X8} missing");
        // as NVIDIA Profile Inspector resolves them (NvapiDrsWrapper.cs): the newer interface first, the public one when missing.
        // The newer DRS_SetSetting (0x8A2CF5F5) takes two more arguments, passed as 0, and writes settings the driver's list of
        // named settings does not hold (Smooth Motion on 621.24); the public one answers NVAPI_SETTING_NOT_FOUND for them.
        static nint F(uint id, uint fallback) => Query(id) is var p && p != 0 ? p : F(fallback);
        const uint SetSettingId = 0x8A2CF5F5, SetSettingPublic = 0x577DD202, GetSettingId = 0xEA99498D, GetSettingPublic = 0x73BF8338,
                   RestoreDefaultId = 0x7DD5B261, RestoreDefaultPublic = 0x53F0381E;
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

        // the profile holding the bridge's exe: (handle, name, predefined by NVIDIA), or null
        static (nint Handle, string Name, bool Predefined)? BridgeProfile(nint s)
        {
            nint p; var n = Name(BridgeExe); var app = Versioned(AppSize);
            fixed (byte* np = n, ap = app)
            {
                var r = ((delegate* unmanaged[Cdecl]<nint, byte*, nint*, byte*, int>)F(0xEEE566B2))(s, np, &p, ap);
                if (r == ExeNotFound) return null;
                Ok(r, "DRS_FindApplicationByName");
            }
            var info = Versioned(ProfileSize);
            fixed (byte* ip = info) Ok(((delegate* unmanaged[Cdecl]<nint, nint, byte*, int>)F(0x61CD6FD6))(s, p, ip), "DRS_GetProfileInfo");
            return (p, NameAt(info, 4), BitConverter.ToUInt32(info, 4 + Str + 4) != 0);
        }

        // a profile the player made: neither NVIDIA's predefined one nor Setup's
        static bool Players((nint Handle, string Name, bool Predefined) b) => !b.Predefined && b.Name != ProfileName;

        static bool SettingOn(nint s, nint p)
        {
            var set = Versioned(SettingSize);
            fixed (byte* sp = set)
            {
                uint x = 0;
                var r = ((delegate* unmanaged[Cdecl]<nint, nint, uint, byte*, uint*, int>)F(GetSettingId, GetSettingPublic))(s, p, SettingId, sp, &x);
                if (r == SettingNotFound) return false;
                Ok(r, "DRS_GetSetting");
            }
            return BitConverter.ToUInt32(set, SettingCurrentAt) == 1;
        }

        static void SetDword(nint s, (nint Handle, string Name, bool Predefined) b, uint id, uint value)
        {
            var set = Versioned(SettingSize);
            BitConverter.TryWriteBytes(set.AsSpan(SettingIdAt), id);              // settingType 0: NVDRS_DWORD_TYPE; location, predefined: 0
            BitConverter.TryWriteBytes(set.AsSpan(SettingCurrentAt), value);
            fixed (byte* sp = set)
                Ok(((delegate* unmanaged[Cdecl]<nint, nint, byte*, uint, uint, int>)F(SetSettingId, SetSettingPublic))(s, b.Handle, sp, 0, 0),
                   $"DRS_SetSetting 0x{id:X8} = {value} on \"{b.Name}\"{(b.Predefined ? " (NVIDIA's)" : "")}");
        }

        public static Status Read()
        {
            if (InitCode() is var ir && NoCard(ir)) return new(null, 0, false, null, null);
            Ok(ir, "NvAPI_Initialize");
            var (card, driver) = Card();
            if (card is null) return new(null, driver, false, null, null);
            return Session(s => BridgeProfile(s) is { } b
                ? new Status(card, driver, SettingOn(s, b.Handle), Players(b) ? b.Name : null, null)
                : new Status(card, driver, false, null, null));
        }

        public static void Set(bool on)
        {
            Init();
            Session(s =>
            {
                var b = BridgeProfile(s);
                if (b is { } own && Players(own)) throw new InvalidOperationException($"Already in the profile \"{own.Name}\"");
                if (!on)
                {
                    if (b is not { } t) return 0;
                    if (!t.Predefined) Ok(((delegate* unmanaged[Cdecl]<nint, nint, int>)F(0x17093206))(s, t.Handle), "DRS_DeleteProfile");
                    else
                        foreach (var id in new[] { SettingId, ApisId })
                        {
                            // NVAPI_DRS_RestoreProfileDefaultSetting: NVIDIA's value back (a setting it never had is removed)
                            var r = ((delegate* unmanaged[Cdecl]<nint, nint, uint, int>)F(RestoreDefaultId, RestoreDefaultPublic))(s, t.Handle, id);
                            if (r != SettingNotFound) Ok(r, $"DRS_RestoreProfileDefaultSetting 0x{id:X8} on \"{t.Name}\"");
                        }
                }
                else
                {
                    if (b is null)
                    {
                        nint p;
                        var prof = Versioned(ProfileSize);
                        Encoding.Unicode.GetBytes(ProfileName, 0, ProfileName.Length, prof, 4);
                        fixed (byte* pp = prof) Ok(((delegate* unmanaged[Cdecl]<nint, byte*, nint*, int>)F(0xCC176068))(s, pp, &p), "DRS_CreateProfile");
                        var app = Versioned(AppSize);
                        Encoding.Unicode.GetBytes(BridgeExe, 0, BridgeExe.Length, app, 8);
                        fixed (byte* ap = app) Ok(((delegate* unmanaged[Cdecl]<nint, nint, byte*, int>)F(0x4347A9DE))(s, p, ap), "DRS_CreateApplication");
                        b = (p, ProfileName, false);
                    }
                    SetDword(s, b.Value, SettingId, 1);
                    SetDword(s, b.Value, ApisId, AllApis);
                }
                Ok(((delegate* unmanaged[Cdecl]<nint, int>)F(0xFCBC7E14))(s), "DRS_SaveSettings");
                return 0;
            });
        }
    }
}
