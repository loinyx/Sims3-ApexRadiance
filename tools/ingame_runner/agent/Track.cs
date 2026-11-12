// Record and replay of the tester's own path: while the tester plays, the camera (10 times a second) and the lights near
// the camera (on or off, colour, place; 5 times a second) are kept; a replay puts the camera through the same path at the
// same pace and does the same light edits, so a lighting check done by hand runs again the same way on another build.
//   track_record  {start:true} | {stop:true}: starts recording, or stops and returns {cam:[[t,px,py,pz,tx,ty,tz],...],
//                 lamps:[{type,x,y,z,on,color,r,g,b}...] (each light's first state), edits:[[t,lamp,x,y,z,fx,fz,on,color,r,g,b],...]}
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
        static List<object> sRecCam = new List<object>(), sRecLamps = new List<object>(), sRecEdits = new List<object>();
        static List<object> sLoadCam = new List<object>(), sLoadLamps = new List<object>(), sLoadEdits = new List<object>();

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
                r["cam"] = sRecCam; r["lamps"] = sRecLamps; r["edits"] = sRecEdits;
                return r;
            }
            RequireLive();
            sRecCam = new List<object>(); sRecLamps = new List<object>(); sRecEdits = new List<object>();
            int gen = ++sRecGen;
            Simulator.AddObject(new OneShotFunctionTask(delegate
            {
                // per light (by object): its index in sRecLamps and the last state recorded
                Dictionary<ObjectGuid, int> index = new Dictionary<ObjectGuid, int>();
                Dictionary<ObjectGuid, string> last = new Dictionary<ObjectGuid, string>();
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
            if (a.Has("reset")) { sLoadCam = new List<object>(); sLoadLamps = new List<object>(); sLoadEdits = new List<object>(); }
            foreach (string k in new string[] { "cam", "lamps", "edits" })
            {
                List<object> part = a.Raw(k) as List<object>;
                if (part != null) (k == "cam" ? sLoadCam : k == "lamps" ? sLoadLamps : sLoadEdits).AddRange(part);
            }
            Dictionary<string, object> r = Obj();
            r["cam"] = sLoadCam.Count; r["lamps"] = sLoadLamps.Count; r["edits"] = sLoadEdits.Count;
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

        static object TrackPlay(Args a)
        {
            Dictionary<string, object> r = Obj();
            if (a.Has("status")) { r["state"] = sPlayState; return r; }
            if (a.Has("stop")) { sPlayGen++; sPlayState = "stopped"; return r; }
            RequireLive();
            if (sLoadCam.Count < 2) throw new AgentException("no recording loaded");
            List<object> cam = sLoadCam, edits = sLoadEdits;
            // each recorded light matched now, and set to its state when the recording started
            LightGameObject[] lamps = new LightGameObject[sLoadLamps.Count];
            int matched = 0;
            for (int i = 0; i < lamps.Length; i++)
            {
                Dictionary<string, object> l = (Dictionary<string, object>)sLoadLamps[i];
                lamps[i] = MatchLamp(l);
                if (lamps[i] == null) continue;
                matched++;
                ApplyLamp(lamps[i], (bool)l["on"], (string)l["color"], D(l["r"]), D(l["g"]), D(l["b"]));
            }
            int gen = ++sPlayGen;
            sPlayState = "playing";
            Simulator.AddObject(new OneShotFunctionTask(delegate
            {
                int t0 = System.Environment.TickCount, c = 0, e = 0;
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
            }));
            r["lamps"] = lamps.Length;
            r["matched"] = matched;
            r["edits"] = edits.Count;
            r["seconds"] = Math.Round(D(((List<object>)cam[cam.Count - 1])[0]) / 1000, 1);
            return r;
        }
    }
}
