using System.Runtime.InteropServices;

namespace ApexSetup;

// AMD Fluid Motion Frames (frame generation in the AMD driver) through AMD's ADLX (amdadlx64.dll, its C interface). ADLX has
// AFMF as one setting for the whole system (IADLX3DSettingsServices1::GetAMDFluidMotionFrames takes no GPU and no application),
// so it applies to every game. Setup remembers that it turned it on (a file in its own folder): maintenance turns it off only
// then, never a setting the player had on already. Nothing here throws: a failure reads as a reason in Error.
public static class Afmf
{
    // Present: ADLX started (an AMD card and driver). Supported: what ADLX reports (AMD documents RX 6000 series or newer).
    public sealed record Status(bool Present, bool Supported, bool On, string? Error);

    public static (string Key, string Arg)? Blocked(Status s)
    {
        if (s.Error is not null) return ("Could not read the AMD driver settings: {0}", s.Error);
        if (s.On) return null;
        if (!s.Supported) return ("Not available on this AMD card or driver: AMD Fluid Motion Frames needs a Radeon RX 6000 series card or newer and a recent driver.", "");
        return null;
    }

    // the record that Setup turned it on (a path, so tests point it elsewhere)
    public static string Marker = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Apex Radiance Setup", "afmf-on.txt"); // (machine-wide, as AFMF is)
    public static bool SetupTurnedOn => File.Exists(Marker);

    public static Status Read()
    {
        // test runs: APEX_SETUP_FAKE_AMD = "<supported, 0 or 1>|<on, 0 or 1>" stands in for ADLX ("none": no AMD card)
        if (Environment.GetEnvironmentVariable("APEX_SETUP_FAKE_AMD") is { } fake)
        {
            if (fake == "none") return new(false, false, false, null);
            var p = fake.Split('|');
            return new(true, p[0] == "1", p.Length > 1 && p[1] == "1", null);
        }
        // (IsEnabled answers ADLX_NOT_SUPPORTED where AFMF is not supported: asked only where it is)
        try { return Tidy(Adlx.Run(f => f is not null && Adlx.Supported(f) ? new Status(true, true, Adlx.Enabled(f), null) : new Status(true, false, false, null)) ?? new(false, false, false, null)); }
        catch (Exception e) { return new(true, false, false, e.Message); }
    }

    // null when done, else why not; the dry run and the fake write nothing
    public static string? Set(bool on)
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1" || Environment.GetEnvironmentVariable("APEX_SETUP_FAKE_AMD") is not null)
            return "Dry run: Setup changes nothing.";
        try
        {
            // the record first: a crash after the driver call still leaves it, so maintenance can turn AFMF off again
            if (on) { Directory.CreateDirectory(Path.GetDirectoryName(Marker)!); File.WriteAllText(Marker, "on"); }
            if (Adlx.Run(f => f is null ? throw new InvalidOperationException("AMD Fluid Motion Frames not available") : Adlx.SetEnabled(f, on)) != 1)
                throw new InvalidOperationException("AMD driver not found");
            if (!on) File.Delete(Marker);
            return null;
        }
        catch (Exception e)
        {
            if (on) try { File.Delete(Marker); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
            return e.Message;
        }
    }

    // a record left while AFMF is off (turned off in AMD Software, a failed call): dropped, so maintenance never turns off a
    // setting the player turns on later
    public static Status Tidy(Status s)
    {
        if (s is { Present: true, Error: null, On: false } && File.Exists(Marker))
            try { File.Delete(Marker); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
        return s;
    }

    // ADLXInitialize's answer: ADLX_OK or ADLX_ALREADY_INITIALIZED start it; anything else (a leftover amdadlx64.dll without an
    // AMD GPU) is no AMD card
    public static bool Started(int initResult) => initResult is 0 or 2;

    // maintenance (removing the bridge or Apex): off only when Setup turned it on; null when nothing to do or done
    public static string? OffIfSetupTurnedOn() => SetupTurnedOn ? Set(false) : null;

    // ADLX's C interface: every object is a pointer to its vtable; slots from the SDK headers (ADLX 2.0, ISystem.h,
    // I3DSettings1.h): IADLXSystem 7 Get3DSettingsServices; IADLXInterface 1 Release, 2 QueryInterface;
    // IADLX3DSettingsServices1 17 GetAMDFluidMotionFrames; IADLX3DAMDFluidMotionFrames 3 IsSupported, 4 IsEnabled, 5 SetEnabled.
    static unsafe class Adlx
    {
        const int Ok = 0, AlreadyEnabled = 1, AlreadyInitialized = 2; // ADLX_RESULT; the last two count as success (ADLX_SUCCEEDED)

        [DllImport("amdadlx64.dll", EntryPoint = "ADLXQueryFullVersion", CallingConvention = CallingConvention.Cdecl)]
        static extern int QueryFullVersion(ulong* version);
        [DllImport("amdadlx64.dll", EntryPoint = "ADLXInitialize", CallingConvention = CallingConvention.Cdecl)]
        static extern int Initialize(ulong version, nint* system);
        [DllImport("amdadlx64.dll", EntryPoint = "ADLXTerminate", CallingConvention = CallingConvention.Cdecl)]
        static extern int Terminate();

        static nint Slot(nint obj, int i) => (*(nint**)obj)[i];
        static void Check(int r, string what) { if (r != Ok) throw new InvalidOperationException($"{what} {r}"); }
        static void Release(nint obj) { if (obj != 0) ((delegate* unmanaged<nint, int>)Slot(obj, 1))(obj); }

        // body gets the AFMF interface, or null when this driver's ADLX has no IADLX3DSettingsServices1 (too old); null when ADLX
        // cannot start (no amdadlx64.dll: no AMD driver)
        public static T? Run<T>(Func<nint?, T> body)
        {
            ulong version;
            try { if (QueryFullVersion(&version) != Ok) return default; }
            catch (DllNotFoundException) { return default; }
            catch (EntryPointNotFoundException) { return default; }
            nint system = 0, services = 0, services1 = 0, afmf = 0;
            // a leftover amdadlx64.dll without an AMD GPU fails here: no AMD card, no row
            if (!Started(Initialize(version, &system))) return default;
            try
            {
                Check(((delegate* unmanaged<nint, nint*, int>)Slot(system, 7))(system, &services), "Get3DSettingsServices");
                fixed (char* iid = "IADLX3DSettingsServices1")
                    if (((delegate* unmanaged<nint, char*, nint*, int>)Slot(services, 2))(services, iid, &services1) != Ok) return body(null);
                if (((delegate* unmanaged<nint, nint*, int>)Slot(services1, 17))(services1, &afmf) != Ok || afmf == 0) return body(null);
                return body(afmf);
            }
            finally { Release(afmf); Release(services1); Release(services); Terminate(); }
        }

        public static bool Supported(nint? f) { byte b; Check(((delegate* unmanaged<nint, byte*, int>)Slot(f!.Value, 3))(f.Value, &b), "IsSupported"); return b != 0; }
        public static bool Enabled(nint? f) { byte b; Check(((delegate* unmanaged<nint, byte*, int>)Slot(f!.Value, 4))(f.Value, &b), "IsEnabled"); return b != 0; }
        public static int SetEnabled(nint? f, bool on) { var r = ((delegate* unmanaged<nint, byte, int>)Slot(f!.Value, 5))(f.Value, on ? (byte)1 : (byte)0); if (r is not (Ok or AlreadyEnabled)) Check(r, "SetEnabled"); return 1; }
    }
}
