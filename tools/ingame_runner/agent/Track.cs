// Record and replay of the tester's own path: while the tester plays, the camera (10 times a second) and the lights near
// the camera (on or off, colour, place; 5 times a second) are kept; a replay puts the camera through the same path at the
// same pace and does the same light edits, so a lighting check done by hand runs again the same way on another build.
//   track_record  {start:true} | {stop:true}: starts recording, or stops and returns {cam:[[t,px,py,pz,tx,ty,tz],...],
//                 lamps:[{type,x,y,z,on,color,r,g,b, placed?,key,level,fx,fz,at}...] (each light's first state; placed: put
//                 down during the recording, with its resource key), edits:[[t,lamp,x,y,z,fx,fz,on,color,r,g,b],...],
//                 levels:[[t,lotId,level],...] (the floor shown on each lot near the camera), modes:[[t,state],...]}
//   track_load    {reset?:true, cam?:[...], lamps?:[...], edits?:[...]}: a recording sent in pieces (a request is at most 64 KB)
//   track_play    {} | {status:true} | {stop:true}: replays the loaded recording; the lights are matched by type and first place
// Both run in game tasks (the simulation): in build and buy mode the game's simulation waits, so a light moved there is
// recorded, and replayed, as one move when live mode is back.
using System;
using System.Collections.Generic;
using Sims3.Gameplay.Abstracts;
using Sims3.Gameplay.Core;
using Sims3.SimIFace;
using OneShotFunctionTask = Sims3.Gameplay.OneShotFunctionTask;

namespace ApexTestAgent
{
    public static partial class Handlers
    {
        static void RegisterTrack()
        {
            Dispatcher.Register("track_record", TrackRecord);
            Dispatcher.Register("track_load", TrackLoad);
            Dispatcher.Register("track_play", TrackPlay);
        }

        const float kTrackLampRadius = 60f;
        static volatile int sRecGen, sPlayGen;
        static string sPlayState = "idle";
        static List<object> sRecCam = new List<object>(), sRecLamps = new List<object>(), sRecEdits = new List<object>(),
                            sRecLevels = new List<object>(), sRecModes = new List<object>();
        static List<object> sLoadCam = new List<object>(), sLoadLamps = new List<object>(), sLoadEdits = new List<object>(),
                            sLoadLevels = new List<object>(), sLoadModes = new List<object>();
        static List<GameObject> sPlaced = new List<GameObject>(); // the lights a replay put down (removed at its end)

        // The lots near a point and the active lot: their floor shown is recorded
        static List<Lot> LotsNear(Vector3 at, float radius)
        {
            List<Lot> r = new List<Lot>();
            foreach (Lot l in LotManager.AllLots)
                if (l != null && !l.IsWorldLot && ((l.GetCenterPosition() - at).Length() <= radius || l == LotManager.ActiveLot)) r.Add(l);
            return r;
        }

        static double R2(double v) { return Math.Round(v, 2); }

        // The lights of the lots near a point (a light belongs to a lot; the world's street lights to the world lot).
        static List<LightGameObject> LightsNear(Vector3 at, float radius)
        {
            List<LightGameObject> r = new List<LightGameObject>();
            foreach (Lot l in LotManager.AllLots)
            {
                if (l == null || (!l.IsWorldLot && (l.GetCenterPosition() - at).Length() > radius + 80f)) continue; // (lots far away skipped)
                foreach (LightGameObject o in l.GetObjects<LightGameObject>())
                    if (o != null && (o.Position - at).Length() <= radius) r.Add(o);
            }
            return r;
        }

        static object TrackRecord(Args a)
        {
            Dictionary<string, object> r = Obj();
            if (a.Has("stop"))
            {
                sRecGen++;
                r["cam"] = sRecCam; r["lamps"] = sRecLamps; r["edits"] = sRecEdits; r["levels"] = sRecLevels; r["modes"] = sRecModes;
                return r;
            }
            RequireLive();
            sRecCam = new List<object>(); sRecLamps = new List<object>(); sRecEdits = new List<object>();
            sRecLevels = new List<object>(); sRecModes = new List<object>();
            // the lights there now: any other one seen later was put down during the recording
            Dictionary<ObjectGuid, bool> existing = new Dictionary<ObjectGuid, bool>();
            foreach (Lot l in LotManager.AllLots)
                if (l != null)
                    foreach (LightGameObject o in l.GetObjects<LightGameObject>()) if (o != null) existing[o.ObjectId] = true;
            int gen = ++sRecGen;
            Simulator.AddObject(new OneShotFunctionTask(delegate
            {
                // per light (by object): its index in sRecLamps and the last state recorded
                Dictionary<ObjectGuid, int> index = new Dictionary<ObjectGuid, int>();
                Dictionary<ObjectGuid, string> last = new Dictionary<ObjectGuid, string>();
                Dictionary<ulong, int> lastLevel = new Dictionary<ulong, int>();
                string lastMode = null;
                int t0 = System.Environment.TickCount, nextCam = 0, nextLamp = 0;
                while (sRecGen == gen)
                {
                    int t = System.Environment.TickCount - t0;
                    if (t >= nextCam)
                    {
                        nextCam = t + 100;
                        Vector3 p = CameraController.GetPosition(), g = CameraController.GetTarget();
                        sRecCam.Add(new List<object> { t, R2(p.x), R2(p.y), R2(p.z), R2(g.x), R2(g.y), R2(g.z) });
                    }
                    if (t >= nextLamp)
                    {
                        nextLamp = t + 200;
                        string mode = SubState();
                        if (mode != lastMode) { lastMode = mode; sRecModes.Add(new List<object> { t, mode }); }
                        foreach (Lot lot in LotsNear(CameraController.GetTarget(), 120f))
                        {
                            int lv = World.LotGetDisplayLevelInfo(lot.LotId).mCurrent, was;
                            if (lastLevel.TryGetValue(lot.LotId, out was) && was == lv) continue;
                            lastLevel[lot.LotId] = lv;
                            sRecLevels.Add(new List<object> { t, lot.LotId.ToString(), lv });
                        }
                        foreach (LightGameObject o in LightsNear(CameraController.GetTarget(), kTrackLampRadius))
                        {
                            Vector3 p = o.Position, f = o.ForwardVector;
                            string st = R2(p.x) + "," + R2(p.y) + "," + R2(p.z) + "," + R2(f.x) + "," + R2(f.z) + "," + o.IsLightOn() + "," + o.Color + "," +
                                        R2(o.CustomColorRed) + "," + R2(o.CustomColorGreen) + "," + R2(o.CustomColorBlue);
                            int i;
                            if (!index.TryGetValue(o.ObjectId, out i))
                            {
                                i = sRecLamps.Count;
                                index[o.ObjectId] = i;
                                Dictionary<string, object> l = Obj();
                                l["type"] = o.GetType().Name; l["x"] = R2(p.x); l["y"] = R2(p.y); l["z"] = R2(p.z);
                                l["on"] = o.IsLightOn(); l["color"] = o.Color.ToString();
                                l["r"] = R2(o.CustomColorRed); l["g"] = R2(o.CustomColorGreen); l["b"] = R2(o.CustomColorBlue);
                                if (!existing.ContainsKey(o.ObjectId))
                                {
                                    ResourceKey k = o.GetResourceKey();
                                    l["placed"] = true; l["at"] = t; l["level"] = o.Level; l["fx"] = R2(f.x); l["fz"] = R2(f.z);
                                    l["key"] = k.InstanceId.ToString("X16") + ":" + k.TypeId.ToString("X8") + ":" + k.GroupId.ToString("X8");
                                }
                                sRecLamps.Add(l);
                                last[o.ObjectId] = st;
                                continue;
                            }
                            if (last[o.ObjectId] == st) continue;
                            last[o.ObjectId] = st;
                            sRecEdits.Add(new List<object> { t, i, R2(p.x), R2(p.y), R2(p.z), R2(f.x), R2(f.z), o.IsLightOn(), o.Color.ToString(),
                                                             R2(o.CustomColorRed), R2(o.CustomColorGreen), R2(o.CustomColorBlue) });
                        }
                    }
                    Simulator.Sleep(0);
                }
            }));
            r["recording"] = true;
            return r;
        }

        static object TrackLoad(Args a)
        {
            if (a.Has("reset"))
            {
                sLoadCam = new List<object>(); sLoadLamps = new List<object>(); sLoadEdits = new List<object>();
                sLoadLevels = new List<object>(); sLoadModes = new List<object>();
            }
            Dictionary<string, List<object>> to = new Dictionary<string, List<object>>();
            to["cam"] = sLoadCam; to["lamps"] = sLoadLamps; to["edits"] = sLoadEdits; to["levels"] = sLoadLevels; to["modes"] = sLoadModes;
            Dictionary<string, object> r = Obj();
            foreach (KeyValuePair<string, List<object>> k in to)
            {
                List<object> part = a.Raw(k.Key) as List<object>;
                if (part != null) k.Value.AddRange(part);
                r[k.Key] = k.Value.Count;
            }
            return r;
        }

        static double D(object v) { return Convert.ToDouble(v, System.Globalization.CultureInfo.InvariantCulture); }
        static Vector3 V(List<object> s, int i) { return new Vector3((float)D(s[i]), (float)D(s[i + 1]), (float)D(s[i + 2])); }

        // The recorded light: the light of that type nearest its first place, within 0.5 m (a light placed during the
        // recording and not saved is not there on another load: its edits are skipped).
        static LightGameObject MatchLamp(Dictionary<string, object> l)
        {
            Vector3 at = new Vector3((float)D(l["x"]), (float)D(l["y"]), (float)D(l["z"]));
            LightGameObject best = null;
            float bestD = 0.5f;
            foreach (LightGameObject o in LightsNear(at, 1f))
            {
                float d = (o.Position - at).Length();
                if (o.GetType().Name == (string)l["type"] && d <= bestD) { best = o; bestD = d; }
            }
            return best;
        }

        static void ApplyLamp(LightGameObject o, bool on, string color, double r, double g, double b)
        {
            LightGameObject.LightColor c = (LightGameObject.LightColor)Enum.Parse(typeof(LightGameObject.LightColor), color, true);
            if (c == LightGameObject.LightColor.CustomColor)
            {
                o.CustomColorRed = (float)r; o.CustomColorGreen = (float)g; o.CustomColorBlue = (float)b;
            }
            if (c != o.Color || c == LightGameObject.LightColor.CustomColor) LightGameObject.SetColorLight(o, c);
            if (on != o.IsLightOn()) o.SwitchLight(on, true);
        }

        // A light put down during the recording, put down again (its resource key, place, floor and facing)
        static LightGameObject PutDown(Dictionary<string, object> l)
        {
            try
            {
                System.Globalization.NumberStyles hex = System.Globalization.NumberStyles.HexNumber;
                string[] k = ((string)l["key"]).Split(':');
                ResourceKey key = new ResourceKey(ulong.Parse(k[0], hex), uint.Parse(k[1], hex), uint.Parse(k[2], hex));
                LightGameObject o = Sims3.Gameplay.GlobalFunctions.CreateObject(key, new Vector3((float)D(l["x"]), (float)D(l["y"]), (float)D(l["z"])),
                    (int)D(l["level"]), new Vector3((float)D(l["fx"]), 0f, (float)D(l["fz"]))) as LightGameObject;
                if (o != null) sPlaced.Add(o);
                return o;
            }
            catch { return null; }
        }

        static void RemovePlaced()
        {
            foreach (GameObject o in sPlaced) try { if (o != null && !o.HasBeenDestroyed) o.Destroy(); } catch { }
            sPlaced.Clear();
        }

        static object TrackPlay(Args a)
        {
            Dictionary<string, object> r = Obj();
            if (a.Has("status")) { r["state"] = sPlayState; return r; }
            if (a.Has("stop")) { sPlayGen++; sPlayState = "stopped"; RemovePlaced(); return r; }
            RequireLive();
            if (sLoadCam.Count < 2) throw new AgentException("no recording loaded");
            List<object> cam = sLoadCam, edits = sLoadEdits, levels = sLoadLevels, modes = sLoadModes, lampDefs = sLoadLamps;
            RemovePlaced();
            // each recorded light matched now, and set to its state when the recording started
            LightGameObject[] lamps = new LightGameObject[sLoadLamps.Count];
            int matched = 0;
            for (int i = 0; i < lamps.Length; i++)
            {
                Dictionary<string, object> l = (Dictionary<string, object>)sLoadLamps[i];
                if (l.ContainsKey("placed")) continue; // (put down at its time)
                lamps[i] = MatchLamp(l);
                if (lamps[i] == null) continue;
                matched++;
                ApplyLamp(lamps[i], (bool)l["on"], (string)l["color"], D(l["r"]), D(l["g"]), D(l["b"]));
            }
            int gen = ++sPlayGen;
            sPlayState = "playing";
            Simulator.AddObject(new OneShotFunctionTask(delegate
            {
                int t0 = System.Environment.TickCount, c = 0, e = 0, lv = 0, md = 0;
                int end = (int)D(((List<object>)cam[cam.Count - 1])[0]);
                while (sPlayGen == gen)
                {
                    int t = System.Environment.TickCount - t0;
                    while (c + 1 < cam.Count && D(((List<object>)cam[c + 1])[0]) <= t) c++;
                    List<object> s0 = (List<object>)cam[c], s1 = (List<object>)cam[Math.Min(c + 1, cam.Count - 1)];
                    double span = D(s1[0]) - D(s0[0]);
                    float f = span > 0 ? (float)Math.Min(1, Math.Max(0, (t - D(s0[0])) / span)) : 0f;
                    if (span > 400) f = 0f; // (a gap: the game was in build mode or loading; the camera jumps, as it did)
                    CameraController.SetPositionAndTarget(V(s0, 1) + (V(s1, 1) - V(s0, 1)) * f, V(s0, 4) + (V(s1, 4) - V(s0, 4)) * f);
                    for (int i = 0; i < lampDefs.Count; i++) // the lights put down by then
                    {
                        Dictionary<string, object> l = (Dictionary<string, object>)lampDefs[i];
                        if (lamps[i] != null || !l.ContainsKey("placed") || l.ContainsKey("failed") || D(l["at"]) > t) continue;
                        lamps[i] = PutDown(l);
                        if (lamps[i] == null) { l["failed"] = true; continue; }
                        ApplyLamp(lamps[i], (bool)l["on"], (string)l["color"], D(l["r"]), D(l["g"]), D(l["b"]));
                    }
                    for (; lv < levels.Count && D(((List<object>)levels[lv])[0]) <= t; lv++)
                    {
                        List<object> x = (List<object>)levels[lv];
                        try { World.LotSetDisplayLevel(ulong.Parse((string)x[1]), (int)D(x[2])); } catch { }
                    }
                    for (; md < modes.Count && D(((List<object>)modes[md])[0]) <= t; md++)
                    {
                        string want = (string)((List<object>)modes[md])[1] ?? "";
                        string kind = want.StartsWith("Build") ? "build" : want.StartsWith("Buy") ? "buy" : want.StartsWith("LiveMode") ? "live" : null;
                        string now = SubState() ?? "";
                        if (kind == null || (kind == "live" && now.StartsWith("LiveMode")) || (kind != "live" && now.StartsWith(want))) continue;
                        try { SetMode(new Args(new Dictionary<string, object> { { "mode", kind } })); } catch { }
                    }
                    for (; e < edits.Count && D(((List<object>)edits[e])[0]) <= t; e++)
                    {
                        List<object> x = (List<object>)edits[e];
                        LightGameObject o = lamps[(int)D(x[1])];
                        if (o == null) continue;
                        Vector3 p = V(x, 2);
                        if ((o.Position - p).Length() > 0.01f)
                        {
                            o.SetPosition(p);
                            o.SetForward(new Vector3((float)D(x[5]), 0f, (float)D(x[6])));
                        }
                        ApplyLamp(o, (bool)x[7], (string)x[8], D(x[9]), D(x[10]), D(x[11]));
                    }
                    if (t > end) break;
                    Simulator.Sleep(0);
                }
                if (sPlayGen == gen) sPlayState = "done";
                if (!(SubState() ?? "").StartsWith("LiveMode")) try { SetMode(new Args(new Dictionary<string, object> { { "mode", "live" } })); } catch { }
            }));
            r["lamps"] = lamps.Length;
            r["matched"] = matched;
            int placed = 0;
            foreach (object l in lampDefs) if (((Dictionary<string, object>)l).ContainsKey("placed")) placed++;
            r["placed"] = placed;
            r["levels"] = levels.Count;
            r["modes"] = modes.Count;
            r["edits"] = edits.Count;
            r["seconds"] = Math.Round(D(((List<object>)cam[cam.Count - 1])[0]) / 1000, 1);
            return r;
        }
    }
}
