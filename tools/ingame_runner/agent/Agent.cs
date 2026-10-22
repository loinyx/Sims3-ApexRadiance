// In-game test agent: a script mod that lets tools/ingame_runner/run.ps1 drive the game without clicks (load a save,
// set the speed, move the camera, save, quit). Trimmed from ts3agent (sims3-modernization, MIT, see
// ../LICENSE-sims3-modernization), itself forked from Sims3-MCP (MIT, Copyright (c) 2026 Critical-Reynolds, see
// LICENSE-Sims3-MCP). Built by tools/ingame_runner/build_agent.mjs; installed only for the length of a run.
//
// Transport: a 1 MB byte[] mailbox found by its magic header. The runner reads and writes it from outside the process
// (ReadProcessMemory / WriteProcessMemory, MailboxClient.cs); the game thread services it once per simulator tick.
// Mono's Boehm GC never moves objects, so the address stays valid. Layout must match MailboxClient.cs.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Text;
using Sims3.Gameplay;
using Sims3.Gameplay.Core;
using Sims3.Gameplay.Utilities;
using Sims3.SimIFace;
using Sims3.UI;
using Sims3.UI.GameEntry;
using GF = Sims3.Gameplay.Gameflow;
using GameSpeed = Sims3.SimIFace.Gameflow.GameSpeed;

[assembly: Tunable] // TuningControl.TuneAssemblies skips assemblies without it, and tuning starts the agent
[assembly: System.Reflection.AssemblyTitle("ApexTestAgent")]
[assembly: System.Reflection.AssemblyVersion("1.0.0.0")]

namespace ApexTestAgent
{
    // Standard script-mod entry point: the game parses the _XML tuning resource whose instance id is
    // FNV64("ApexTestAgent.Instantiator"), which touches this [Tunable] field and runs the static constructor.
    public class Instantiator
    {
        [Tunable] protected static bool kInstantiator = false;
        public const string Version = "1.1.0";
        public static string StartError; // why the commands did not register, reported by game_state and the marker

        static Instantiator()
        {
            // Idle unless run.ps1 started the game: a package left in Mods opens no mailbox, starts no task, writes no file
            try { if (CommandLine.FindSwitch("ApexTestAgent") == null) return; } catch (Exception) { return; }
            // Mailbox and marker first: whatever fails below, the runner still finds the agent and can say what failed
            Mailbox.Open();
            try { Handlers.Register(); } catch (Exception e) { StartError = e.GetType().Name + ": " + e.Message; }
            Marker.Write("loaded");
            World.OnStartupAppEventHandler += delegate { Mailbox.Open(); PollTask.Start(); };
            World.OnWorldLoadFinishedEventHandler += delegate { Mailbox.Open(); Mailbox.SetWorldLoaded(true); PollTask.Start(); };
            World.OnWorldQuitEventHandler += delegate { Mailbox.SetWorldLoaded(false); PollTask.Start(); };
            try { PollTask.Start(); } catch (Exception) { } // tuning may load after the startup event already fired
        }
    }

    // Tells run.ps1 from outside that the agent's code ran, without the memory scan: one ScriptError_*.xml in the user
    // folder (the game's only file API for scripts), holding an <ApexTestAgent .../> line. run.ps1 moves it into the
    // run's folder. Retried on the first poll tick when the simulator could not write it yet.
    public static class Marker
    {
        static bool sWritten;

        public static void Write(string stage)
        {
            if (sWritten) return;
            try
            {
                uint handle = 0;
                Simulator.CreateScriptErrorFile(ref handle);
                if (handle == 0) return;
                string text = "<ApexTestAgent version=\"" + Instantiator.Version + "\" stage=\"" + stage + "\" commands=\"" +
                    (Instantiator.StartError ?? "ok").Replace("&", "&amp;").Replace("\"", "&quot;").Replace("<", "&lt;") + "\" />\n";
                Simulator.AppendToScriptErrorFile(handle, text.ToCharArray());
                Simulator.CloseScriptErrorFile(handle);
                sWritten = true;
            }
            catch (Exception) { }
        }
    }

    // Services the mailbox every tick on the game thread, at the main menu and in a world.
    public class PollTask : Task
    {
        static PollTask sInstance;
        bool mRunning = true;

        public static void Start()
        {
            if (sInstance != null) sInstance.mRunning = false;
            sInstance = new PollTask();
            Simulator.AddObject(sInstance);
        }

        public override void Simulate()
        {
            Marker.Write("tick");
            while (mRunning)
            {
                try { Mailbox.Poll(); } catch (Exception) { }
                Simulator.Sleep(0);
            }
            Simulator.DestroyObject(ObjectId);
        }
    }

    public static class Mailbox
    {
        public const int Version = 1;
        public const uint CheckSalt = 0x41505854;
        public const int Header = 64, ReqCap = 64 * 1024, Capacity = 1024 * 1024;
        const int OffVersion = 16, OffNonce = 20, OffReqSeq = 24, OffRespSeq = 28, OffReqLen = 32, OffRespLen = 36,
                  OffCapacity = 40, OffCheck = 44, OffHeartbeat = 48, OffState = 56;

        static byte[] sBuffer; // the static reference keeps the buffer alive

        public static void Open()
        {
            if (sBuffer != null) return;
            byte[] buf = new byte[Capacity];
            uint nonce = (uint)new Random().Next() ^ (uint)System.Environment.TickCount;
            PutU32(buf, OffVersion, Version);
            PutU32(buf, OffNonce, nonce);
            PutU32(buf, OffCapacity, Capacity);
            PutU32(buf, OffCheck, (uint)Version ^ nonce ^ (uint)Capacity ^ CheckSalt);
            // "APEXTEST-MBOX-01" XOR 0x5A, decoded at run time so the DLL itself never matches a scan; written last so a
            // scanner never sees a half-written header
            byte[] enc = { 0x1B, 0x0A, 0x1F, 0x02, 0x0E, 0x1F, 0x09, 0x0E, 0x77, 0x17, 0x18, 0x15, 0x02, 0x77, 0x6A, 0x6B };
            for (int i = 0; i < enc.Length; i++) buf[i] = (byte)(enc[i] ^ 0x5A);
            sBuffer = buf;
        }

        public static void SetWorldLoaded(bool loaded)
        {
            if (sBuffer != null) PutU32(sBuffer, OffState, loaded ? 1u : 0u);
        }

        public static void Poll()
        {
            byte[] buf = sBuffer;
            if (buf == null) return;
            PutU32(buf, OffHeartbeat, GetU32(buf, OffHeartbeat) + 1);
            uint req = GetU32(buf, OffReqSeq);
            if (req == GetU32(buf, OffRespSeq)) return;
            string reply;
            try
            {
                int len = (int)GetU32(buf, OffReqLen);
                if (len < 0 || len > ReqCap) throw new InvalidOperationException("bad request length " + len);
                reply = Dispatcher.Handle(Encoding.UTF8.GetString(buf, Header, len));
            }
            catch (Exception e) { reply = Dispatcher.ErrorReply(0, e); }
            byte[] data = Encoding.UTF8.GetBytes(reply);
            if (data.Length > Capacity - Header - ReqCap)
                data = Encoding.UTF8.GetBytes(Dispatcher.ErrorReply(0, new InvalidOperationException("response too large")));
            Buffer.BlockCopy(data, 0, buf, Header + ReqCap, data.Length);
            PutU32(buf, OffRespLen, (uint)data.Length);
            PutU32(buf, OffRespSeq, req); // publish last
        }

        static uint GetU32(byte[] b, int o) { return (uint)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)); }

        static void PutU32(byte[] b, int o, uint v)
        {
            b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24);
        }
    }

    public delegate object Handler(Args args);

    // An expected failure (bad argument, wrong state): the reply carries the message without a stack.
    public class AgentException : Exception
    {
        public AgentException(string message) : base(message) { }
    }

    public static class Dispatcher
    {
        static readonly Dictionary<string, Handler> sHandlers = new Dictionary<string, Handler>();

        public static void Register(string name, Handler handler) { sHandlers[name] = handler; }

        public static string Handle(string requestJson)
        {
            Dictionary<string, object> req = Json.Parse(requestJson) as Dictionary<string, object>;
            if (req == null) throw new AgentException("request must be a JSON object");
            long id = req.ContainsKey("id") ? Convert.ToInt64(req["id"], CultureInfo.InvariantCulture) : 0;
            try
            {
                string cmd = req.ContainsKey("cmd") ? req["cmd"] as string : null;
                Handler h;
                if (cmd == null || !sHandlers.TryGetValue(cmd, out h)) throw new AgentException("unknown command '" + cmd + "'");
                Dictionary<string, object> a = req.ContainsKey("args") ? req["args"] as Dictionary<string, object> : null;
                Dictionary<string, object> ok = new Dictionary<string, object>();
                ok["id"] = id;
                ok["ok"] = true;
                ok["result"] = h(new Args(a ?? new Dictionary<string, object>()));
                return Json.Write(ok);
            }
            catch (Exception e) { return ErrorReply(id, e); }
        }

        public static string ErrorReply(long id, Exception e)
        {
            Dictionary<string, object> err = new Dictionary<string, object>();
            err["id"] = id;
            err["ok"] = false;
            err["error"] = e is AgentException ? e.Message : e.GetType().Name + ": " + e.Message + "\n" + e.StackTrace;
            return Json.Write(err);
        }
    }

    public class Args
    {
        readonly Dictionary<string, object> mValues;
        public Args(Dictionary<string, object> values) { mValues = values; }
        public bool Has(string key) { return mValues.ContainsKey(key) && mValues[key] != null; }
        public object Raw(string key) { return Has(key) ? mValues[key] : null; }

        public string Str(string key)
        {
            if (!Has(key)) throw new AgentException("missing argument '" + key + "'");
            return Convert.ToString(mValues[key], CultureInfo.InvariantCulture);
        }

        public string Str(string key, string dflt) { return Has(key) ? Str(key) : dflt; }
        public double Double(string key, double dflt) { return Has(key) ? Convert.ToDouble(mValues[key], CultureInfo.InvariantCulture) : dflt; }
        public bool Bool(string key, bool dflt) { return Has(key) ? Convert.ToString(mValues[key], CultureInfo.InvariantCulture).ToLowerInvariant() == "true" : dflt; }
    }

    public static partial class Handlers
    {
        public static void Register()
        {
            RegisterRepros(); // Repros.cs
            Dispatcher.Register("game_state", GameState);
            Dispatcher.Register("load_save", LoadSave);
            Dispatcher.Register("set_speed", SetSpeed);
            Dispatcher.Register("camera", CameraView);
            Dispatcher.Register("camera_pan", CameraPan);
            Dispatcher.Register("save_game", SaveGame);
            Dispatcher.Register("auto_dialog", AutoDialog);
            Dispatcher.Register("quit_game", QuitGame);
            Dispatcher.Register("async_log", AsyncLog);
        }

        static Dictionary<string, object> Obj() { return new Dictionary<string, object>(); }

        // ------------------------------------------------------------ async work
        // Anything that can open a modal dialog sleeps its task until the dialog closes, so it never runs on the poll task.
        static readonly List<string> sAsyncLog = new List<string>();
        delegate void Work();

        static void RunAsync(string label, Work work)
        {
            Simulator.AddObject(new Sims3.Gameplay.OneShotFunctionTask(delegate
            {
                string result;
                try { work(); result = "ok"; }
                catch (Exception e)
                {
                    Exception inner = e;
                    while (inner.InnerException != null) inner = inner.InnerException; // reflection wraps the real cause
                    result = "error: " + inner.GetType().Name + ": " + inner.Message;
                }
                lock (sAsyncLog)
                {
                    sAsyncLog.Add(SimClock.CurrentTime().ToString() + " " + label + " -> " + result);
                    if (sAsyncLog.Count > 30) sAsyncLog.RemoveAt(0);
                }
            }));
        }

        static object AsyncLog(Args a) { lock (sAsyncLog) return new List<string>(sAsyncLog); }

        // ------------------------------------------------------------ state, load, speed, save, quit

        // In-world sub state (LiveModeState, EditTownState, BuildModeState...), read from GameStates' private state machine.
        static string SubState()
        {
            try
            {
                object gs = typeof(GameStates).GetField("sSingleton", BindingFlags.NonPublic | BindingFlags.Static).GetValue(null);
                object inWorld = typeof(GameStates).GetField("mInWorldState", BindingFlags.NonPublic | BindingFlags.Instance).GetValue(gs);
                if (inWorld == null) return null;
                object sm = inWorld.GetType().GetField("mStateMachine", BindingFlags.NonPublic | BindingFlags.Instance).GetValue(inWorld);
                object cur = sm.GetType().GetProperty("CurState").GetValue(sm, null);
                return cur == null ? null : cur.GetType().Name;
            }
            catch (Exception e) { return "unknown: " + e.Message; }
        }

        static object GameState(Args a)
        {
            Dictionary<string, object> r = Obj();
            r["agent"] = Instantiator.Version;
            r["start_error"] = Instantiator.StartError;
            r["in_world"] = GameStates.IsInWorld();
            r["sub_state"] = SubState();
            r["load_file"] = GameStates.LoadFileName;
            r["save_in_progress"] = Sims3.Gameplay.UI.Responder.Instance.OptionsModel.SaveGameInProgress;
            r["modal"] = UIManager.GetModalWindow() != null;
            if (GameStates.IsInWorld())
            {
                r["speed"] = GF.CurrentGameSpeed.ToString();
                r["minutes_total"] = Math.Round(SimClock.ConvertFromTicks(SimClock.CurrentTicks, TimeUnit.Minutes), 1);
                r["clock"] = SimClock.Hours24.ToString("00") + ":" + SimClock.Minutes60.ToString("00");
            }
            return r;
        }

        // file: a save in Documents\Electronic Arts\The Sims 3\Saves, such as "ApexTest-Town.sims3". reload: true loads
        // it from inside a world through the game's in-game load path.
        static object LoadSave(Args a)
        {
            bool reload = a.Bool("reload", false);
            if (GameStates.IsInWorld() && !reload) throw new AgentException("already in a world; pass reload: true");
            string file = a.Str("file");
            SaveGameMetadata info = new SaveGameMetadata();
            info.mSaveFile = file;
            if (!Sims3.Gameplay.UI.Responder.Instance.MainMenuModel.GetSaveFileDetails(ref info))
                throw new AgentException("unknown save '" + file + "'");
            if (GameStates.IsInWorld())
                RunAsync("load_save " + file, delegate { GameStates.TransitionToGameStateReloadWorld(file, info, InWorldState.SubState.LiveMode, true); });
            else
                RunAsync("load_save " + file, delegate { GameStates.TransitionToGameStateToInWorld(file, info, InWorldState.SubState.LiveMode, true); });
            Dictionary<string, object> r = Obj();
            r["loading"] = file;
            return r;
        }

        static object SetSpeed(Args a)
        {
            GameSpeed target;
            switch (a.Str("speed").ToLowerInvariant())
            {
                case "pause": target = GameSpeed.Pause; break;
                case "normal": target = GameSpeed.Normal; break;
                case "fast": target = GameSpeed.Double; break;
                case "ultra": target = GameSpeed.Triple; break;
                default: throw new AgentException("speed must be pause, normal, fast or ultra");
            }
            if (GF.GameSpeedLocked && target != GameSpeed.Pause) throw new AgentException("game speed is locked (a dialog is open or the game is saving)");
            if (GF.CurrentGameSpeed != target) GF.SetGameSpeed(target, GF.SetGameSpeedContext.User); // Pause toggles when already paused
            return GameState(a);
        }

        // name: a new save name (a fresh file, the loaded save is never overwritten).
        static object SaveGame(Args a)
        {
            IOptionsModel om = Sims3.Gameplay.UI.Responder.Instance.OptionsModel;
            if (om.SaveGameInProgress) throw new AgentException("a save is already in progress");
            if (UIManager.GetModalWindow() != null) throw new AgentException("close the open dialog before saving");
            if (!om.CanSaveGame()) throw new AgentException("the game cannot save right now");
            om.SaveName = a.Str("name");
            RunAsync("save " + om.SaveName, delegate { om.SaveGame(false, true, false); }); // forceUseSaveName: a new file
            Dictionary<string, object> r = Obj();
            r["saving"] = om.SaveName;
            return r;
        }

        static object QuitGame(Args a)
        {
            RunAsync("quit_game", delegate { GameStates.TransitionToGameStateQuitNoCheck(); });
            Dictionary<string, object> r = Obj();
            r["quitting"] = true;
            return r;
        }

        // ------------------------------------------------------------ camera

        static void RequireLive()
        {
            if (SubState() != "LiveModeState") throw new AgentException("not in live mode (state " + SubState() + ")");
        }

        static Vector3 Vec(Args a, string key)
        {
            Dictionary<string, object> d = a.Raw(key) as Dictionary<string, object>;
            if (d == null || !d.ContainsKey("x") || !d.ContainsKey("y") || !d.ContainsKey("z")) throw new AgentException(key + " needs {x, y, z}");
            CultureInfo ic = CultureInfo.InvariantCulture;
            return new Vector3((float)Convert.ToDouble(d["x"], ic), (float)Convert.ToDouble(d["y"], ic), (float)Convert.ToDouble(d["z"], ic));
        }

        static object Pos(Vector3 v)
        {
            Dictionary<string, object> d = Obj();
            d["x"] = Math.Round(v.x, 2);
            d["y"] = Math.Round(v.y, 2);
            d["z"] = Math.Round(v.z, 2);
            return d;
        }

        // camera {pos?:{x,y,z}, target?:{x,y,z}}: sets and returns the game camera (the game window is never moved or focused).
        static object CameraView(Args a)
        {
            RequireLive();
            if (a.Has("pos") || a.Has("target"))
                CameraController.SetPositionAndTarget(a.Has("pos") ? Vec(a, "pos") : CameraController.GetPosition(),
                                                      a.Has("target") ? Vec(a, "target") : CameraController.GetTarget());
            Dictionary<string, object> r = Obj();
            r["pos"] = Pos(CameraController.GetPosition());
            r["target"] = Pos(CameraController.GetTarget());
            return r;
        }

        // camera_pan {lots?, height?, seconds?, dwellS?, passes?} | {stop:true}: pans the camera at street height over the
        // `lots` (default 8) lots of the densest neighbourhood, in a game task that moves it every tick by wall-clock time,
        // so lot loading and lot lighting see a steady camera speed. seconds (default 40) of movement per pass, dwellS
        // (default 0) still at every lot, passes (default 4) alternate there and back. Returns at once.
        static volatile int sPanGen;

        static object CameraPan(Args a)
        {
            RequireLive();
            int gen = ++sPanGen;
            Dictionary<string, object> r = Obj();
            if (a.Has("stop")) { r["stopped"] = true; return r; }
            List<Vector3> path = DenseLotPath((int)a.Double("lots", 8));
            if (path.Count < 2) throw new AgentException("no path: fewer than 2 lots in this world");
            float height = (float)a.Double("height", 25);
            int moveMs = (int)(a.Double("seconds", 40) * 1000), dwellMs = (int)(a.Double("dwellS", 0) * 1000);
            int passes = (int)a.Double("passes", 4);
            float total = 0;
            for (int i = 1; i < path.Count; i++) total += Flat(path[i] - path[i - 1]).Length();
            if (moveMs < 1000 || passes < 1 || total < 1) throw new AgentException("seconds >= 1, passes >= 1 and a path longer than 1 m needed");
            int passMs = moveMs + dwellMs * path.Count;
            Simulator.AddObject(new Sims3.Gameplay.OneShotFunctionTask(delegate
            {
                int t0 = System.Environment.TickCount;
                while (sPanGen == gen)
                {
                    int el = System.Environment.TickCount - t0;
                    if (el >= passMs * passes) break;
                    bool back = (el / passMs) % 2 == 1;
                    int left = el % passMs;
                    Vector3 at = path[back ? path.Count - 1 : 0], dir = Vector3.Empty;
                    for (int i = 0; i < path.Count; i++) // dwell at each point, then move along the next segment for its share
                    {
                        Vector3 p = path[back ? path.Count - 1 - i : i];
                        at = p;
                        if (left < dwellMs || i == path.Count - 1) break;
                        left -= dwellMs;
                        Vector3 q = path[back ? path.Count - 2 - i : i + 1], seg = Flat(q - p);
                        int segMs = (int)(moveMs * seg.Length() / total);
                        dir = seg;
                        if (left < segMs) { at = p + seg * ((float)left / Math.Max(1, segMs)); break; }
                        left -= segMs;
                    }
                    if (dir.Length() < 0.01f) dir = Flat(path[path.Count - 1] - path[0]) * (back ? -1f : 1f);
                    dir = dir * (1f / Math.Max(0.01f, dir.Length()));
                    Vector3 target = new Vector3(at.x, World.GetTerrainHeight(at.x, at.z), at.z);
                    CameraController.SetPositionAndTarget(target - dir * height + new Vector3(0, height, 0), target);
                    Simulator.Sleep(0);
                }
            }));
            r["lots"] = path.Count;
            r["lengthM"] = Math.Round(total);
            r["passMs"] = passMs;
            r["passes"] = passes;
            return r;
        }

        static Vector3 Flat(Vector3 v) { return new Vector3(v.x, 0, v.z); }

        // The k lots nearest the lot with the most other lots within 150 m, in x order.
        static List<Vector3> DenseLotPath(int k)
        {
            List<Vector3> c = new List<Vector3>();
            foreach (Lot l in LotManager.AllLots) if (l != null && !l.IsWorldLot) c.Add(l.GetCenterPosition());
            if (c.Count < 2) return c;
            int best = 0, bestN = -1;
            for (int i = 0; i < c.Count; i++)
            {
                int n = 0;
                for (int j = 0; j < c.Count; j++) if (Flat(c[j] - c[i]).Length() < 150f) n++;
                if (n > bestN) { bestN = n; best = i; }
            }
            Vector3 centre = c[best];
            c.Sort(delegate(Vector3 x, Vector3 y) { return Flat(x - centre).Length().CompareTo(Flat(y - centre).Length()); });
            if (c.Count > k) c.RemoveRange(k, c.Count - k);
            c.Sort(delegate(Vector3 x, Vector3 y) { return x.x.CompareTo(y.x); });
            return c;
        }

        // ------------------------------------------------------------ dialogs

        const uint kButtonClick = 678582774u;
        static readonly string[] kAcceptWords = { "ok", "accept", "yes", "continue", "done", "confirm", "close", "got it" };
        static readonly string[] kRejectWords = { "cancel", "no", "back", "quit", "exit", "decline", "never" };
        static readonly string[] kNames = { "Alex", "Sam", "Jamie", "Robin", "Casey", "Morgan", "Riley", "Quinn", "Avery", "Reese" };
        static int sNameIndex;

        // Dialogs where "accept" is the wrong answer for an unattended run: the first rule whose text fragment appears in
        // the dialog picks the first button containing one of its preferred words. Anything else falls back to Score().
        static readonly string[][][] kDialogRules = {
            new[] { new[] { "placing the following lots", "new lots available" }, new[] { "don't place", "dont place", "no" } },
            new[] { new[] { "save your town before continuing" }, new[] { "just continue", "continue" } },
            new[] { new[] { "name your saved game" }, new[] { "cancel" } },
            new[] { new[] { "come over", "invite", "would like to visit", "wants to visit", "join them" }, new[] { "decline", "not now", "no", "cancel" } },
            new[] { new[] { "mod scripts found" }, new[] { "accept", "ok", "continue" } },
        };

        static uint WinHandle(WindowBase w)
        {
            return (uint)typeof(WindowBase).GetField("mWinHandle", BindingFlags.NonPublic | BindingFlags.Instance).GetValue(w);
        }

        static bool IsInside(WindowBase w, uint root)
        {
            for (WindowBase p = w; p != null; p = p.Parent) if (WinHandle(p) == root) return true;
            return false;
        }

        // Every clickable control inside the modal window, with its glue handler from UIManager.mEventRegistry.
        static List<KeyValuePair<WindowBase, object>> DialogButtons(WindowBase modal)
        {
            List<KeyValuePair<WindowBase, object>> list = new List<KeyValuePair<WindowBase, object>>();
            IDictionary registry = typeof(UIManager).GetField("mEventRegistry", BindingFlags.NonPublic | BindingFlags.Static).GetValue(null) as IDictionary;
            if (registry == null) return list;
            uint root = WinHandle(modal);
            List<object> entries = new List<object>(); // snapshot: touching windows can register handlers
            foreach (object v in registry.Values) entries.Add(v);
            foreach (object data in entries)
            {
                if (data == null) continue;
                Type dt = data.GetType();
                WindowBase win = dt.GetField("Window").GetValue(data) as WindowBase;
                IDictionary callbacks = dt.GetField("EventTypesAndCallbacks").GetValue(data) as IDictionary;
                if (win == null || callbacks == null || !callbacks.Contains(kButtonClick)) continue;
                if (!win.Visible || !IsInside(win, root)) continue;
                list.Add(new KeyValuePair<WindowBase, object>(win, callbacks[kButtonClick]));
            }
            return list;
        }

        static void CollectTexts(WindowBase w, List<string> texts, int depth)
        {
            if (w == null || depth > 12 || texts.Count > 60) return;
            if (w is Text && w.Visible && !string.IsNullOrEmpty(w.Caption)) texts.Add(w.Caption);
            for (uint i = 0; i < 200; i++)
            {
                WindowBase child;
                try { child = w.GetChildByIndex(i); } catch (Exception) { break; }
                if (child == null) break;
                CollectTexts(child, texts, depth + 1);
            }
        }

        static void FindTextEdits(WindowBase w, List<TextEdit> found, int depth)
        {
            if (w == null || depth > 12) return;
            TextEdit te = w as TextEdit;
            if (te != null && te.Visible) found.Add(te);
            for (uint i = 0; i < 200; i++)
            {
                WindowBase child;
                try { child = w.GetChildByIndex(i); } catch (Exception) { break; }
                if (child == null) break;
                FindTextEdits(child, found, depth + 1);
            }
        }

        static string Words(WindowBase b)
        {
            string tip = null;
            Button button = b as Button;
            if (button != null) { try { tip = button.TooltipText; } catch (Exception) { } }
            return ((b.Caption ?? "") + " " + (tip ?? "")).Trim().ToLowerInvariant();
        }

        static int Score(string words)
        {
            foreach (string w in kRejectWords) if (words == w || words.StartsWith(w + " ")) return -1;
            foreach (string w in kAcceptWords) if (words == w || words.StartsWith(w + " ") || words.IndexOf(w) >= 0) return 2;
            return words.Length == 0 ? 0 : 1;
        }

        // Clears the modal dialog that blocks the game, if any: fills empty name fields (a newborn), then clicks the
        // preferred or most accepting enabled button. Tooltips count because EA icon buttons have no caption.
        static object AutoDialog(Args a)
        {
            WindowBase modal = UIManager.GetModalWindow();
            Dictionary<string, object> r = Obj();
            r["open"] = modal != null;
            if (modal == null) return r;
            List<string> texts = new List<string>();
            CollectTexts(modal, texts, 0);
            r["title"] = texts.Count > 0 ? texts[0] : null;
            string allText = string.Join(" ", texts.ToArray()).ToLowerInvariant();
            string[] preferred = null;
            foreach (string[][] rule in kDialogRules)
                foreach (string frag in rule[0])
                    if (preferred == null && allText.IndexOf(frag) >= 0) preferred = rule[1];
            List<KeyValuePair<WindowBase, object>> buttons = DialogButtons(modal);
            KeyValuePair<WindowBase, object>? best = null;
            int bestScore = int.MinValue;
            if (preferred != null)
                foreach (string want in preferred)
                {
                    foreach (KeyValuePair<WindowBase, object> kv in buttons)
                        if (kv.Key.Enabled && Words(kv.Key).IndexOf(want) >= 0) { best = kv; bestScore = 100; break; }
                    if (best != null) break;
                }
            if (preferred == null) // name fields only when about to accept, never for a dialog that gets cancelled
            {
                List<TextEdit> edits = new List<TextEdit>();
                FindTextEdits(modal, edits, 0);
                foreach (TextEdit te in edits) if (string.IsNullOrEmpty(te.Caption)) te.Caption = kNames[sNameIndex++ % kNames.Length];
            }
            List<string> seen = new List<string>();
            foreach (KeyValuePair<WindowBase, object> kv in buttons)
            {
                string words = Words(kv.Key);
                int s = kv.Key.Enabled ? Score(words) : -2;
                seen.Add("#" + kv.Key.ID + " '" + words + "' score " + s);
                if (s > bestScore) { bestScore = s; best = kv; }
            }
            r["buttons"] = seen;
            if (best == null || bestScore < 0) throw new AgentException("no acceptable button: " + string.Join(", ", seen.ToArray()));
            WindowBase btn = best.Value.Key;
            object glue = best.Value.Value;
            MethodInfo invoke = glue.GetType().GetMethod("Invoke", new Type[] { typeof(WindowBase), typeof(UIEventArgs) });
            UIButtonClickEventArgs args = new UIButtonClickEventArgs();
            args.Init(kButtonClick, btn, btn, (int)btn.ID, 0, 0, 0f, 0f, 0f, 0f, false, null);
            RunAsync("auto_dialog #" + btn.ID, delegate { invoke.Invoke(glue, new object[] { btn, args }); });
            r["clicked"] = "#" + btn.ID + " " + Words(btn);
            return r;
        }
    }
}
