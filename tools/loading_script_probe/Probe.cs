using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using Sims3.SimIFace;
using Sims3.UI;

[assembly: Tunable]
namespace ApexRadiance.LoadingStudy
{
    // Temporary script-side observer. It never resumes tasks or changes loading flags.
    public static class Bootstrap
    {
        [Tunable] public static bool kInstantiator = false;
        private static bool running;
        private static int generation;
        private static uint openedAt, lastReport;
        private static object observed;
        private static int bytes, reports;
        private static int files;
        private const BindingFlags Fields = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance;
        static Bootstrap()
        {
            World.OnWorldLoadFinishedEventHandler += OnLoad;
            World.OnWorldQuitEventHandler += OnQuit;
        }
        private static uint Now { get { return unchecked((uint)System.Environment.TickCount); } }
        private static void OnLoad(object sender, EventArgs args)
        {
            if (running) return;
            running = true;
            ++generation;
            observed = null;
            reports = bytes = 0;
            files = 0;
            Write("Session started. Passive script task and dialog observer. Build loading-study.1. No task is resumed, cancelled or replaced.");
            Schedule();
        }
        private static void OnQuit(object sender, EventArgs args) { running = false; ++generation; }
        private static void Schedule()
        {
            int current = generation;
            Simulator.AddObject(new OneShotFunctionTask(delegate { if (current == generation) Tick(); }, StopWatch.TickStyles.Seconds, 1f, false, false));
        }
        private static object Read(Type type, object instance, string name)
        {
            for (Type t = type; t != null; t = t.BaseType)
            {
                FieldInfo f = t.GetField(name, Fields | BindingFlags.DeclaredOnly);
                if (f != null) return f.GetValue(instance);
            }
            return "<field unavailable: " + name + ">";
        }
        private static object Static(string assemblyName, string typeName, string field)
        {
            foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
                if (assembly.GetName().Name == assemblyName)
                {
                    Type t = assembly.GetType(typeName, false);
                    return t == null ? "<type unavailable>" : Read(t, null, field);
                }
            return "<assembly unavailable>";
        }
        private static void Tick()
        {
            if (!running || reports >= 6 || bytes >= 1048576) return;
            try
            {
                object dialog = Static("UI", "Sims3.UI.ProgressDialog", "sDialog");
                if (dialog is string) { Write("Dialog field unavailable; " + dialog); running = false; return; }
                uint now = Now;
                if (!Object.ReferenceEquals(dialog, observed))
                {
                    Write(dialog == null ? "ProgressDialog cleared; observed lifetime " + unchecked(now - openedAt) + " ms" : "ProgressDialog appeared");
                    observed = dialog;
                    openedAt = now;
                    lastReport = now;
                    if (dialog != null) Capture(dialog, 0, false);
                }
                if (dialog != null && unchecked(now - openedAt) >= 10000 && unchecked(now - lastReport) >= 10000)
                {
                    lastReport = now;
                    Capture(dialog, unchecked(now - openedAt), true);
                }
            }
            catch (ResetException) { throw; }
            catch (Exception e) { Write("Observer error: " + e.GetType().FullName + ": " + e.Message); }
            if (running) Schedule();
        }
        private static string DescribeTask(object task)
        {
            if (task == null) return "<null task>";
            string name = task.GetType().FullName;
            object function = Read(task.GetType(), task, "mFunction");
            Delegate d = function as Delegate;
            if (d != null) name += " -> " + d.Method.DeclaringType.FullName + "::" + d.Method.Name;
            return name;
        }
        private static bool Relevant(string name)
        {
            return name.IndexOf("OneShot", StringComparison.Ordinal) >= 0 || name.IndexOf("StateMachine", StringComparison.Ordinal) >= 0 ||
                name.IndexOf("SceneReady", StringComparison.Ordinal) >= 0 || name.IndexOf("MiniLoad", StringComparison.Ordinal) >= 0;
        }
        private static void Capture(object dialog, uint age, bool stacks)
        {
            ++reports;
            StringBuilder text = new StringBuilder();
            text.AppendLine("Snapshot " + reports + "; dialog age " + age + " ms; observer task " + Simulator.CurrentTask.Value.ToString("X16"));
            object mini = Static("UI", "Sims3.UI.GameEntry.MiniLoad", "sMiniLoad");
            text.AppendLine("MiniLoad: " + (mini == null ? "absent" : mini is string ? mini.ToString() : "present"));
            if (mini != null && !(mini is string))
                foreach (string field in new string[] { "mLotIDToWaitFor", "mWaitForSims", "mScreenMaskedFunc", "mLotsLoadedFunc", "mScreenUnmaskedFunc" })
                {
                    object value = Read(mini.GetType(), mini, field);
                    Delegate callback = value as Delegate;
                    text.AppendLine("MiniLoad " + field + ": " + (callback == null ? value : callback.Method.DeclaringType.FullName + "::" + callback.Method.Name));
                }
            object game = Static("Sims3GameplaySystems", "Sims3.Gameplay.GameStates", "sSingleton");
            if (game != null && !(game is string))
            {
                object world = Read(game.GetType(), game, "mInWorldState");
                if (world != null && !(world is string))
                    foreach (string field in new string[] { "mPrevStateId", "mNextStateId", "mToggleState" })
                        text.AppendLine("InWorldState " + field + ": " + Read(world.GetType(), world, field));
                text.AppendLine("State change dialog: " + Static("Sims3GameplaySystems", "Sims3.Gameplay.InWorldState", "sStateChangeDialogUp"));
            }
            foreach (string field in new string[] { "mCurrentTask", "mPauseMode", "mDelayScript", "mModalDialogWindow", "mGlideWindow" })
                text.AppendLine("Dialog " + field + ": " + Read(dialog.GetType(), dialog, field));
            object hash = Static("ScriptCore", "ScriptCore.Simulator", "mObjHash");
            IDictionary tasks = hash as IDictionary;
            if (tasks == null) { text.AppendLine("Task registry unavailable: " + hash); Write(text.ToString()); return; }
            List<KeyValuePair<ulong, object>> selected = new List<KeyValuePair<ulong, object>>();
            int visited = 0, relevant = 0;
            foreach (DictionaryEntry entry in tasks)
            {
                if (++visited > 16384) break;
                ulong id = (ulong)entry.Key;
                if (id == Simulator.CurrentTask.Value) continue;
                string name = DescribeTask(entry.Value);
                if (!Relevant(name)) continue;
                ++relevant;
                if (selected.Count < 64) selected.Add(new KeyValuePair<ulong, object>(id, entry.Value));
            }
            text.AppendLine("Task registry count " + tasks.Count + "; inspected " + visited + "; relevant " + relevant + "; selected " + selected.Count + "; truncated " + (visited > 16384 || relevant > 64));
            foreach (KeyValuePair<ulong, object> task in selected)
            {
                text.AppendLine("Task " + task.Key.ToString("X16") + " " + DescribeTask(task.Value));
                if (stacks)
                {
                    try
                    {
                        string stack = Simulator.GetObjectStackTrace(new ObjectGuid(task.Key));
                        if (stack != null && stack.Length > 4096) stack = stack.Substring(0, 4096) + " <truncated>";
                        text.AppendLine(stack);
                    }
                    catch (ResetException) { throw; }
                    catch (Exception e) { text.AppendLine("Stack unavailable: " + e.GetType().FullName); }
                }
            }
            Write(text.ToString());
        }
        private static void Write(string message)
        {
            if (bytes >= 1048576 || files >= 16) return;
            uint handle = 0;
            try
            {
                string text = "[LoadingScriptTasks] uptime " + Now + " ms: " + message + "\n";
                if (text.Length > 65536) text = text.Substring(0, 65536) + " <snapshot truncated>\n";
                text = "<ApexLoadingScriptStudy><![CDATA[" + text.Replace("]]>", "]]]]><![CDATA[>") + "]]></ApexLoadingScriptStudy>";
                string created = Simulator.CreateScriptErrorFile(ref handle);
                if (String.IsNullOrEmpty(created)) { running = false; return; }
                ++files;
                if (!Simulator.AppendToScriptErrorFile(handle, text.ToCharArray())) running = false;
                bytes += Encoding.UTF8.GetByteCount(text);
            }
            catch (ResetException) { throw; }
            catch { running = false; }
            finally { if (handle != 0) Simulator.CloseScriptErrorFile(handle); }
        }
    }
}
