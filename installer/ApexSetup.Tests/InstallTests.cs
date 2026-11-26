// Offline checks of the launcher's installed and portable modes (ApexSetup.Core/LauncherInstall.cs) in a temporary
// %LOCALAPPDATA%, Start menu, desktop and HKCU key: nothing reaches the player's real profile.
using ApexSetup;
using Microsoft.Win32;

static class InstallTests
{
    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "ApexInstallTest-" + Guid.NewGuid().ToString("N")[..8]);
        var (oldLocal, oldBase, oldStart, oldDesk) = (Paths.LocalAppData, LauncherInstall.RegistryBase, LauncherInstall.StartMenuDir, LauncherInstall.DesktopDir);
        var oldEnv = Environment.GetEnvironmentVariable(LauncherInstall.PortableVar);
        Environment.SetEnvironmentVariable(LauncherInstall.PortableVar, null);
        Paths.LocalAppData = Path.Combine(root, "Local");
        LauncherInstall.RegistryBase = @"Software\ApexLauncherTests-" + Guid.NewGuid().ToString("N");
        LauncherInstall.StartMenuDir = Path.Combine(root, "Start");
        LauncherInstall.DesktopDir = Path.Combine(root, "Desktop");
        // a fake game's saves and mods: no launcher step may touch them
        var docs = Path.Combine(root, "Docs");
        Directory.CreateDirectory(Path.Combine(docs, "Saves"));
        Directory.CreateDirectory(Path.Combine(docs, "Mods"));
        File.WriteAllText(Path.Combine(docs, "Saves", "Household.sims3"), "save");
        File.WriteAllText(Path.Combine(docs, "Mods", "Resource.cfg"), "cfg");
        bool DocsIntact() => File.ReadAllText(Path.Combine(docs, "Saves", "Household.sims3")) == "save" && File.ReadAllText(Path.Combine(docs, "Mods", "Resource.cfg")) == "cfg";
        bool NoTraces() => !LauncherInstall.Registered() && !LauncherInstall.RunsAtStartup() && !File.Exists(LauncherInstall.StartMenuLink) && !File.Exists(LauncherInstall.DesktopLink);
        var started = new List<(string Exe, string[] Args)>();
        void Start(string exe, IEnumerable<string> args) => started.Add((exe, args.ToArray()));
        string NewExe(string dir, string name = "ApexLauncher.exe")
        {
            Directory.CreateDirectory(dir);
            var p = Path.Combine(dir, name);
            File.Copy(Environment.ProcessPath!, p, true);
            return p;
        }
        void Data()
        {
            new LauncherSettings { FixWithoutAsking = true }.Save();
            File.WriteAllText(Path.Combine(Paths.Data, "mod-notes.json"), "{\"notes\":1}");
        }
        bool DataKept() => LauncherSettings.Load().FixWithoutAsking && File.Exists(Path.Combine(Paths.Data, "mod-notes.json"));
        bool Gone(params string[] paths)
        {
            for (var i = 0; i < 100 && paths.Any(p => File.Exists(p) || Directory.Exists(p)); i++) Thread.Sleep(200); // (DeleteAfterExit waits about 3 s)
            return !paths.Any(p => File.Exists(p) || Directory.Exists(p));
        }

        try
        {
            // 1. the file an old Setup 1.2.x puts in place: it downloads ApexRadianceSetup.exe (the same bytes as
            // ApexLauncher.exe), renames itself to .old, copies the new one under its own name and starts it with its own
            // arguments, as administrator (its manifest requires it)
            Directory.CreateDirectory(Path.Combine(Paths.LocalAppData, "Apex Radiance Setup"));
            File.WriteAllText(Path.Combine(Paths.LocalAppData, "Apex Radiance Setup", "games.txt"), @"C:\Games\The Sims 3\Game\Bin");
            File.WriteAllText(Path.Combine(Paths.LocalAppData, "Apex Radiance Setup", "language.txt"), "pt");
            var downloads = Path.Combine(root, "Downloads");
            var oldSetup = Path.Combine(downloads, "ApexRadianceSetup.exe");
            Directory.CreateDirectory(downloads);
            File.WriteAllText(oldSetup, "old setup 1.2.7");
            var update = NewExe(Path.Combine(root, "Temp", "ApexRadianceSetup-update", "x"), "ApexRadianceSetup.exe");
            foreach (var oldArgs in new[] { Array.Empty<string>(), [@"C:\Games\The Sims 3"] })
            {
                started.Clear();
                SelfUpdate.ReplaceAndStart(update, oldSetup, oldArgs, Start);
                check(started.Single().Exe == oldSetup && started[0].Args.SequenceEqual(oldArgs) && File.Exists(oldSetup + ".old")
                    && File.ReadAllBytes(oldSetup).SequenceEqual(File.ReadAllBytes(update)),
                    $"old Setup update: the new exe in its place, started with its {oldArgs.Length} argument(s)");
                SelfUpdate.CleanUp(oldSetup);
            }
            check(!File.Exists(oldSetup + ".old"), "old Setup update: the replaced Setup removed at the new start");
            check(LauncherInstall.ModeOf(oldSetup) == LauncherMode.NotChosen, "old Setup update: the new exe starts NotChosen");
            check(LauncherInstall.FirstStep(oldSetup, elevated: true) == StartStep.RestartAsPlayer,
                "old Setup update: started as administrator, it opens again as the player");
            check(LauncherInstall.FirstStep(oldSetup, elevated: false) == StartStep.RunHere,
                "old Setup update: as the player it runs where it is, so the window asks");
            Paths.MigrateOldData();
            check(File.ReadAllText(Path.Combine(Paths.Data, "games.txt")).Contains("The Sims 3") && File.ReadAllText(Path.Combine(Paths.Data, "language.txt")) == "pt"
                && !Directory.Exists(Path.Combine(Paths.LocalAppData, "Apex Radiance Setup")), "old Setup update: its remembered games and language move over");
            check(!File.Exists(Paths.InstalledExe) && !Directory.Exists(Paths.InstallDir) && NoTraces() && !File.Exists(LauncherInstall.MarkerOf(oldSetup)),
                "old Setup update: nothing installed, registered or marked before the player chooses");

            // 2. mode detection
            var free = NewExe(Path.Combine(root, "Free"));
            check(LauncherInstall.ModeOf(free) == LauncherMode.NotChosen, "a copy without a marker is NotChosen");
            Environment.SetEnvironmentVariable(LauncherInstall.PortableVar, "1");
            check(LauncherInstall.ModeOf(free) == LauncherMode.Portable && LauncherInstall.FirstStep(free, false) == StartStep.RunHere,
                "APEX_LAUNCHER_PORTABLE=1 makes it portable");
            Environment.SetEnvironmentVariable(LauncherInstall.PortableVar, null);
            File.WriteAllText(LauncherInstall.MarkerOf(free), "");
            check(LauncherInstall.ModeOf(free) == LauncherMode.Portable && LauncherInstall.FirstStep(free, true) == StartStep.RestartAsPlayer,
                "the marker next to the exe makes it portable (as administrator: again as the player)");
            File.Delete(LauncherInstall.MarkerOf(free));

            // 3. install from the old Setup's file: copied, registered, started, the caller exits
            Data();
            started.Clear();
            LauncherInstall.Install(oldSetup, [@"C:\Games\The Sims 3"], Start);
            check(File.Exists(Paths.InstalledExe) && LauncherInstall.ModeOf(Paths.InstalledExe) == LauncherMode.Installed
                && LauncherInstall.FirstStep(Paths.InstalledExe, true) == StartStep.Register, "install: the installed copy is Installed");
            check(LauncherInstall.Registered() && File.Exists(LauncherInstall.StartMenuLink) && File.Exists(LauncherInstall.DesktopLink),
                "install: Installed apps entry, Start menu and desktop shortcuts");
            check(started.Single().Exe == Paths.InstalledExe && started[0].Args.SequenceEqual([@"C:\Games\The Sims 3", LauncherInstall.HandoverArg]),
                "install: the installed copy started with the arguments and the handover");
            check(LauncherInstall.FirstStep(oldSetup, false) == StartStep.HandToInstalled, "install: the downloaded copy started again hands over to the installed one");
            check(DataKept(), "install: settings and mod notes kept");

            // 4. installed -> portable in a folder of the player's: copied, marked, traces gone, install folder removed
            LauncherInstall.SetRunAtStartup(Paths.InstalledExe, true);
            started.Clear();
            var portDir = Path.Combine(root, "USB", "Apex");
            var port = LauncherInstall.MakePortable(Paths.InstalledExe, portDir, null, Start);
            check(port == Path.Combine(portDir, "ApexLauncher.exe") && LauncherInstall.ModeOf(port) == LauncherMode.Portable && started.Single().Exe == port,
                "installed -> portable: the copy in the chosen folder is portable and started");
            check(NoTraces() && Gone(Paths.InstallDir), "installed -> portable: shortcuts, Run, Installed apps entry and install folder gone");
            check(DataKept() && DocsIntact(), "installed -> portable: settings, mod notes, saves and mods kept");

            // 5. portable -> installed -> portable where it is
            started.Clear();
            LauncherInstall.Install(port, null, Start);
            check(LauncherInstall.Registered() && !File.Exists(LauncherInstall.MarkerOf(port)) && LauncherInstall.ModeOf(port) == LauncherMode.NotChosen
                && LauncherInstall.FirstStep(port, false) == StartStep.HandToInstalled && started.Single().Exe == Paths.InstalledExe,
                "portable -> installed: registered, marker gone, the old portable exe hands over");
            var threw = false;
            try { LauncherInstall.MakePortable(Paths.InstalledExe, null, null, Start); } catch (ArgumentException) { threw = true; }
            check(threw && LauncherInstall.Registered(), "the installed copy cannot be made portable inside the install folder");
            started.Clear();
            check(LauncherInstall.MakePortable(port, null, null, Start) == port && LauncherInstall.ModeOf(port) == LauncherMode.Portable && started.Count == 0,
                "a copy made portable where it is: marked, nothing started");
            check(DataKept(), "round trips: settings and mod notes kept");
            Directory.Delete(Paths.InstallDir, true);
            LauncherInstall.RemoveTraces();

            // 6. moving a portable copy: the whole folder keeps it portable, the exe alone is asked again
            var moved = Path.Combine(root, "Moved");
            Directory.Move(portDir, moved);
            var movedExe = Path.Combine(moved, "ApexLauncher.exe");
            check(LauncherInstall.ModeOf(movedExe) == LauncherMode.Portable, "a portable folder moved as a whole stays portable");
            var alone = Path.Combine(root, "Alone");
            Directory.CreateDirectory(alone);
            File.Move(movedExe, Path.Combine(alone, "ApexLauncher.exe"));
            check(LauncherInstall.ModeOf(Path.Combine(alone, "ApexLauncher.exe")) == LauncherMode.NotChosen, "a portable exe moved without its marker is asked again");
            File.Move(Path.Combine(alone, "ApexLauncher.exe"), movedExe);
            started.Clear();
            var moved2 = LauncherInstall.MakePortable(movedExe, Path.Combine(root, "Elsewhere"), null, Start);
            check(LauncherInstall.ModeOf(moved2) == LauncherMode.Portable && started.Single().Exe == moved2 && Gone(movedExe, LauncherInstall.MarkerOf(movedExe)),
                "a portable copy moved by the launcher: the new one marked and started, the old exe and marker gone");

            // 7. updates replace the running copy in place and keep its mode
            var upd = NewExe(Path.Combine(root, "Temp", "ApexLauncher-update", "y"));
            File.AppendAllText(upd, "x"); // (other bytes, as a newer build)
            SelfUpdate.ReplaceAndStart(upd, moved2, LauncherInstall.HandoverArgs([]), Start);
            SelfUpdate.CleanUp(moved2);
            check(LauncherInstall.ModeOf(moved2) == LauncherMode.Portable && File.ReadAllBytes(moved2).SequenceEqual(File.ReadAllBytes(upd)),
                "update of a portable copy: replaced in place, the marker survives");
            LauncherInstall.Install(moved2, null, Start);
            SelfUpdate.ReplaceAndStart(upd, Paths.InstalledExe, LauncherInstall.HandoverArgs([LauncherInstall.HandoverArg]), Start);
            SelfUpdate.CleanUp(Paths.InstalledExe);
            check(LauncherInstall.ModeOf(Paths.InstalledExe) == LauncherMode.Installed && File.ReadAllBytes(Paths.InstalledExe).SequenceEqual(File.ReadAllBytes(upd))
                && started[^1].Args.SequenceEqual([LauncherInstall.HandoverArg]), "update of the installed copy: replaced in place, still Installed, one handover");

            // 7b. a folder of the player's: moved there and back, and only the launcher's own files go from it
            {
                var games = Path.Combine(root, "D", "Games");
                Directory.CreateDirectory(games);
                var chosen = LauncherInstall.FolderFor(games);
                check(chosen == Path.Combine(games, "Apex Launcher") && LauncherInstall.FolderFor(chosen) == chosen, "install folder: one of the launcher's own inside the one picked");
                check(LauncherInstall.CanInstallTo(chosen) && !Directory.Exists(chosen), "install folder: the check leaves nothing");
                check(!LauncherInstall.CanInstallTo(Paths.Data) && !LauncherInstall.CanInstallTo(Path.Combine(Paths.Data, "x")) && !LauncherInstall.CanInstallTo(Path.Combine(Paths.DefaultInstallDir, "x"))
                    && !LauncherInstall.CanInstallTo("Apex") && !LauncherInstall.CanInstallTo(@"C:\%TEMP%\x"), "install folder: refused among the launcher's data, inside the default one, or not a full path");
                LauncherInstall.SetRunAtStartup(Paths.InstalledExe, true);
                started.Clear();
                LauncherInstall.Install(Paths.InstalledExe, null, Start, chosen);
                check(Paths.InstallDir == chosen && File.Exists(Path.Combine(chosen, "ApexLauncher.exe")) && Gone(Paths.DefaultInstallDir) && started.Single().Exe == Paths.InstalledExe
                    && LauncherInstall.ModeOf(Paths.InstalledExe) == LauncherMode.Installed && LauncherInstall.Registered() && DataKept(), "install folder: moved, started from there, the old copy gone");
                using (var run = Registry.CurrentUser.OpenSubKey(LauncherInstall.RegistryBase + @"\Run"))
                    check(run?.GetValue("Apex Launcher") is string cmd && cmd.Contains(chosen), "install folder: Start with Windows follows");
                check(LauncherInstall.FirstStep(oldSetup, false) == StartStep.HandToInstalled, "install folder: a downloaded copy hands over to it");
                // a start that Windows refuses: everything points at the copy that works again
                var other = LauncherInstall.FolderFor(Path.Combine(root, "E"));
                var refused = false;
                try { LauncherInstall.Install(Paths.InstalledExe, null, (_, _) => throw new System.ComponentModel.Win32Exception(5), other); } catch (System.ComponentModel.Win32Exception) { refused = true; }
                using (var run = Registry.CurrentUser.OpenSubKey(LauncherInstall.RegistryBase + @"\Run"))
                using (var apps = Registry.CurrentUser.OpenSubKey(LauncherInstall.RegistryBase + @"\Uninstall\ApexLauncher"))
                    check(refused && Paths.InstallDir == chosen && File.Exists(Paths.InstalledExe) && Gone(Path.Combine(other, "ApexLauncher.exe")) && run?.GetValue("Apex Launcher") is string r && r.Contains(chosen)
                        && apps?.GetValue("InstallLocation") is string at && at == chosen, "install folder: a move that fails leaves the copy there was, and all that points at it");
                // a copy cut short in the folder is made again
                Directory.CreateDirectory(other);
                File.WriteAllText(Path.Combine(other, "ApexLauncher.exe"), "cut");
                LauncherInstall.Install(Paths.InstalledExe, null, Start, other);
                check(new FileInfo(Path.Combine(other, "ApexLauncher.exe")).Length > 3 && Gone(Path.Combine(chosen, "ApexLauncher.exe")), "install folder: a copy cut short there is made again");
                LauncherInstall.Install(Paths.InstalledExe, null, Start, chosen);
                check(LauncherInstall.LastingArgs(["--tray", @"--moved-from=C:\x.exe", "--was-setup", "--replace", "--handover", @"C:\Games\The Sims 3"]).SequenceEqual([@"C:\Games\The Sims 3"]), "install folder: the next copy gets only what lasts of the command line");
                File.WriteAllText(Path.Combine(chosen, "mine.txt"), "");
                LauncherInstall.Install(Paths.InstalledExe, null, Start, Paths.DefaultInstallDir);
                check(Paths.InstallDir == Paths.DefaultInstallDir && File.Exists(Paths.InstalledExe) && File.Exists(Path.Combine(chosen, "mine.txt")) && !File.Exists(Path.Combine(chosen, "ApexLauncher.exe")),
                    "install folder: moved back, a file of the player's in the chosen one stays");
                LauncherInstall.Install(Paths.InstalledExe, null, Start, chosen);
                Data();
                Uninstall.Launcher(Paths.InstalledExe, keepData: true);
                check(NoTraces() && Gone(Path.Combine(chosen, "ApexLauncher.exe")) && File.Exists(Path.Combine(chosen, "mine.txt")) && Paths.InstallDir == Paths.DefaultInstallDir,
                    "install folder: uninstalled from it, only its own files went and the choice with them");
                LauncherInstall.Install(NewExe(Path.Combine(root, "Again")), null, Start);
            }

            // 8. uninstall from each mode: no key, shortcut, Run value, data or exe left; saves and mods untouched
            LauncherInstall.SetRunAtStartup(Paths.InstalledExe, true);
            Data();
            Uninstall.Launcher(Paths.InstalledExe);
            check(NoTraces() && Gone(Paths.InstallDir, Paths.Data) && DocsIntact(), "uninstall (installed): traces, install folder and data gone; saves and mods kept");
            var p2 = LauncherInstall.MakePortable(NewExe(Path.Combine(root, "Port2")), null, null, Start);
            Data();
            Uninstall.Launcher(p2);
            check(NoTraces() && Gone(p2, LauncherInstall.MarkerOf(p2), Paths.Data) && Directory.Exists(Path.Combine(root, "Port2")) && DocsIntact(),
                "uninstall (portable): exe, marker and data gone, its folder stays; saves and mods kept");
            using (var k = Registry.CurrentUser.OpenSubKey(LauncherInstall.RegistryBase + @"\Uninstall\ApexLauncher"))
                check(k is null, "uninstall: no Installed apps key left");
        }
        finally
        {
            Registry.CurrentUser.DeleteSubKeyTree(LauncherInstall.RegistryBase, false);
            (Paths.LocalAppData, LauncherInstall.RegistryBase, LauncherInstall.StartMenuDir, LauncherInstall.DesktopDir) = (oldLocal, oldBase, oldStart, oldDesk);
            Environment.SetEnvironmentVariable(LauncherInstall.PortableVar, oldEnv);
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }
}
