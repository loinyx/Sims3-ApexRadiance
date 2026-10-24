using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;

namespace Sims3.SimIFace
{
    public class ResetException : Exception { }
    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Assembly)] public class TunableAttribute : Attribute { }
    public struct ObjectGuid { public ulong Value; public ObjectGuid(ulong value) { Value = value; } public override string ToString() { return Value.ToString("X16"); } }
    public static class World
    {
        public static event EventHandler OnWorldLoadFinishedEventHandler, OnWorldQuitEventHandler;
        public static void Load() { OnWorldLoadFinishedEventHandler(null, EventArgs.Empty); }
        public static void Quit() { OnWorldQuitEventHandler(null, EventArgs.Empty); }
    }
    public class StopWatch { public enum TickStyles { Seconds } }
    public class StateMachine
    {
        public bool mbPauseUpdate = false, mbEnabled = true, mPendingShutdown = false, mbPendingReturn = false;
        public object mPendingCall = null;
        public ObjectGuid mObjectId = new ObjectGuid(90000);
        public StateMachineStatus mStatus = new StateMachineStatus();
    }
    public class StateMachineStatus { public object mCurState = new FakeState(); public object mNextState = null; }
    public class FakeState { public int mStateId = 1, mNextStateId = 14; public string mStateName = "BuildMode"; }
    public static class Simulator
    {
        public static ObjectGuid CurrentTask { get { return new ObjectGuid(99); } }
        public static List<string> Files = new List<string>();
        public static List<object> Scheduled = new List<object>();
        public static int Stacks;
        public static ObjectGuid AddObject(object task) { Scheduled.Add(task); return new ObjectGuid(99); }
        public static string GetObjectStackTrace(ObjectGuid task) { ++Stacks; return "Suspended task " + task.Value + " in WaitForSceneReadiness"; }
        public static string CreateScriptErrorFile(ref uint handle) { handle = 1; Files.Add(""); return "test"; }
        public static bool AppendToScriptErrorFile(uint handle, char[] text) {
            if (text.Length == 0 || text[text.Length - 1] != '\0') throw new Exception("Native writer requires a terminated buffer");
            Files[Files.Count - 1] += new string(text, 0, text.Length - 1);
            return true;
        }
        public static bool CloseScriptErrorFile(uint handle) { return true; }
    }
}
namespace Sims3.UI
{
    public delegate void Function();
    public class OneShotFunctionTask
    {
        public Function mFunction;
        public bool PauseModal, PauseSimulator;
        public OneShotFunctionTask(Function f, Sims3.SimIFace.StopWatch.TickStyles style, float delay, bool modal, bool sim) { mFunction = f; PauseModal = modal; PauseSimulator = sim; }
    }
}
class FakeDialog
{
    public ulong mCurrentTask = 5;
    public int mPauseMode = 0;
    public ulong mDelayScript = 0;
    public object mModalDialogWindow = null;
    public bool mGlideWindow = false;
}
class Check
{
    static Type Probe = typeof(ApexRadiance.LoadingStudy.Bootstrap);
    static object Call(string name, params object[] args) { return Probe.GetMethod(name, BindingFlags.Static | BindingFlags.NonPublic).Invoke(null, args); }
    static void Assert(bool ok) { if (!ok) throw new Exception("Script observer check failed"); }
    static Type DynamicType(string assembly, string name, string field, Type fieldType)
    {
        AssemblyBuilder a = AppDomain.CurrentDomain.DefineDynamicAssembly(new AssemblyName(assembly), AssemblyBuilderAccess.Run);
        TypeBuilder t = a.DefineDynamicModule(assembly).DefineType(name, TypeAttributes.Public);
        t.DefineField(field, fieldType, FieldAttributes.Private | FieldAttributes.Static);
        return t.CreateType();
    }
    static void Main()
    {
        Type dialogType = DynamicType("UI", "Sims3.UI.ProgressDialog", "sDialog", typeof(object));
        Type registryType = DynamicType("ScriptCore", "ScriptCore.Simulator", "mObjHash", typeof(IDictionary));
        IDictionary tasks = new Dictionary<ulong, object>();
        for (ulong i = 0; i < 70; ++i) tasks[i] = new Sims3.UI.OneShotFunctionTask(delegate { }, Sims3.SimIFace.StopWatch.TickStyles.Seconds, 1, false, false);
        for (ulong i = 1000; i < 25000; ++i) tasks[i] = new object();
        tasks[90000ul] = new Sims3.SimIFace.StateMachine(); // Beyond the previous registry traversal limit.
        registryType.GetField("mObjHash", BindingFlags.Static | BindingFlags.NonPublic).SetValue(null, tasks);
        ApexRadiance.LoadingStudy.Bootstrap.kInstantiator = true;
        Sims3.SimIFace.World.Load();
        Assert(Sims3.SimIFace.Simulator.Scheduled.Count == 1);
        var scheduled = (Sims3.UI.OneShotFunctionTask)Sims3.SimIFace.Simulator.Scheduled[0];
        Assert(!scheduled.PauseModal && !scheduled.PauseSimulator);
        FakeDialog dialog = new FakeDialog();
        dialogType.GetField("sDialog", BindingFlags.Static | BindingFlags.NonPublic).SetValue(null, dialog);
        Call("Tick");
        Assert(Sims3.SimIFace.Simulator.Stacks == 0);
        Call("Capture", dialog, (uint)12000, true);
        string text = Sims3.SimIFace.Simulator.Files[Sims3.SimIFace.Simulator.Files.Count - 1];
        Assert(text.Contains("inspected 24071; relevant 71; selected 64; truncated True"));
        Assert(text.Contains("Machine mCurState: Sims3.SimIFace.FakeState"));
        Assert(text.Contains("State mNextStateId: 14"));
        Assert(text.Contains("WaitForSceneReadiness"));
        Assert(text.Contains("Dialog mCurrentTask: 5"));
        Assert(Sims3.SimIFace.Simulator.Stacks == 64);
        int captured = Sims3.SimIFace.Simulator.Files.Count;
        dialogType.GetField("sDialog", BindingFlags.Static | BindingFlags.NonPublic).SetValue(null, null);
        Call("Tick");
        Assert(Sims3.SimIFace.Simulator.Files.Count == captured + 1);
        Assert(Sims3.SimIFace.Simulator.Files[captured].Contains("ProgressDialog cleared"));
        for (int i = 0; i < 30; ++i) Call("Write", "bounded output");
        Assert(Sims3.SimIFace.Simulator.Files.Count == 16);
        foreach (string file in Sims3.SimIFace.Simulator.Files) { System.Xml.XmlDocument xml = new System.Xml.XmlDocument(); xml.LoadXml(file); }
        Sims3.SimIFace.World.Quit();
        int before = Sims3.SimIFace.Simulator.Files.Count;
        Call("Tick");
        Assert(Sims3.SimIFace.Simulator.Files.Count == before);
        Console.WriteLine("Script observer: modal-safe scheduling, passive field reads, task stacks, selection limits, dialog closure, XML output and session limits passed");
    }
}
