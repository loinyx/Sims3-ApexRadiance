// Repro commands of the in-game test agent: they set the state that makes a feature's code path run and repeat it in a
// loop, so a load fires that path many times instead of waiting for play to cause it. Ported from ts3agent
// (sims3-modernization, MIT, see ../LICENSE-sims3-modernization). Only the in-game camera moves (the game window is never
// moved or focused), effects are visual only, spawned Sims are removed again, and nothing is saved.
//   camera_jumps  jumps the camera across far lots (lot streaming: lot, object and texture loads) or to the most populated
//                 lots; per jump optionally Sims change outfit (a Sim build each) and new Sims spawn into view
//   vfx_burst     a ring of fire, spell, fairy, lightning and sparkle effects around the camera target, re-created every
//                 few seconds (effects and their scene nodes)
//   remove_sims   removes every Sim the agent spawned
//   set_mode      build, buy, cas, edittown or live: the game's own mode transitions (world mode switches)
using System;
using System.Collections.Generic;
using Sims3.Gameplay;
using Sims3.Gameplay.Actors;
using Sims3.Gameplay.CAS;
using Sims3.Gameplay.Core;
using Sims3.Gameplay.Utilities;
using Sims3.SimIFace;
using Sims3.SimIFace.CAS;
using Sims3.UI;
using Sims3.UI.CAS;
using OneShotFunctionTask = Sims3.Gameplay.OneShotFunctionTask;

namespace ApexTestAgent
{
    public static partial class Handlers
    {
        static void RegisterRepros()
        {
            Dispatcher.Register("camera_jumps", CameraJumps);
            Dispatcher.Register("vfx_burst", VfxBurst);
            Dispatcher.Register("remove_sims", RemoveSims);
            Dispatcher.Register("set_mode", SetMode);
        }

        static int Int(Args a, string key, int dflt) { return (int)a.Double(key, dflt); }

        // ------------------------------------------------------------ camera_jumps
        static int sJumps, sJumpOutfits, sJumpSpawned, sJumpLoops;

        // camera_jumps {order?: far|populated, lots?, dwellS?, height?, loops?, outfits?, spawn?} | {stop:true} | {status:true}
        // A game task jumps the camera (no pan) to one lot after another and holds it still dwellS (default 8) wall-clock
        // seconds at street zoom (height, default 25 m), so the game streams each lot in (lots load only under a slow
        // camera). order far (default): `lots` (default 10) lots in farthest-next order, every jump crosses the town;
        // populated: the `lots` lots with the most Sims on them, re-ranked every loop. Per jump: outfits = up to that many
        // humans on the target lot switch outfit (athletic / everyday by loop parity: a Sim build each); spawn = that many
        // new Sims stand around the lot centre in view; the Sims spawned by the previous loop are removed first, so every
        // loop builds new ones. loops (default 1000) = until stop. Shares camera_pan's generation: a new camera_jumps or
        // camera_pan ends a running one. stop also removes the spawned Sims. Returns the first loop's path.
        static object CameraJumps(Args a)
        {
            Dictionary<string, object> r = Obj();
            if (a.Has("status") || a.Has("stop"))
            {
                if (a.Has("stop")) { ++sPanGen; r["stopped"] = true; r["removed"] = RemoveSpawned(); }
                r["jumps"] = sJumps; r["loops"] = sJumpLoops; r["outfitChanges"] = sJumpOutfits; r["spawned"] = sJumpSpawned;
                return r;
            }
            RequireLive();
            int gen = ++sPanGen;
            bool populated = a.Str("order", "far").ToLowerInvariant() == "populated";
            int k = Int(a, "lots", 10), loops = Int(a, "loops", 1000), outfits = Int(a, "outfits", 0), spawn = Int(a, "spawn", 0);
            int dwellMs = (int)(a.Double("dwellS", 8) * 1000);
            float height = (float)a.Double("height", 25);
            if (k < 2 || dwellMs < 500 || spawn < 0 || spawn > 10) throw new AgentException("lots >= 2, dwellS >= 0.5, spawn 0..10");
            List<Lot> first = populated ? PopulatedLots(k) : FarLots(k);
            if (first.Count < 2) throw new AgentException("fewer than 2 lots to jump between");
            sJumps = sJumpOutfits = sJumpSpawned = sJumpLoops = 0;
            Simulator.AddObject(new OneShotFunctionTask(delegate
            {
                for (int loop = 0; loop < loops && sPanGen == gen; loop++)
                {
                    if (spawn > 0) RemoveSpawned();
                    List<Lot> path = loop == 0 ? first : populated ? PopulatedLots(k) : FarLots(k);
                    OutfitCategories cat = loop % 2 == 0 ? OutfitCategories.Athletic : OutfitCategories.Everyday;
                    foreach (Lot lot in path)
                    {
                        if (sPanGen != gen) break;
                        Vector3 c = lot.GetCenterPosition();
                        Vector3 target = new Vector3(c.x, World.GetTerrainHeight(c.x, c.z), c.z);
                        CameraController.SetPositionAndTarget(target + new Vector3(-height * 0.7f, height, -height * 0.7f), target);
                        sJumps++;
                        if (spawn > 0) sJumpSpawned += SpawnAround(c, spawn);
                        if (outfits > 0) sJumpOutfits += ChangeOutfitsOn(lot, outfits, cat);
                        int t0 = System.Environment.TickCount;
                        while (sPanGen == gen && System.Environment.TickCount - t0 < dwellMs) Simulator.Sleep(0);
                    }
                    sJumpLoops++;
                }
            }));
            List<object> pts = new List<object>();
            foreach (Lot l in first) pts.Add(l.Name);
            r["path"] = pts;
            r["dwellMs"] = dwellMs;
            return r;
        }

        static List<Lot> JumpLots()
        {
            List<Lot> c = new List<Lot>();
            foreach (Lot l in LotManager.AllLots) if (l != null && !l.IsWorldLot) c.Add(l);
            return c;
        }

        // farthest-next order from the lot farthest from the camera: each jump goes to the remaining lot farthest from the last
        static List<Lot> FarLots(int k)
        {
            List<Lot> c = JumpLots(), path = new List<Lot>();
            Vector3 at = CameraController.GetTarget();
            while (path.Count < k && c.Count > 0)
            {
                int best = 0; float bestD = -1;
                for (int i = 0; i < c.Count; i++)
                {
                    float d = Flat(c[i].GetCenterPosition() - at).Length();
                    if (d > bestD) { bestD = d; best = i; }
                }
                at = c[best].GetCenterPosition();
                path.Add(c[best]);
                c.RemoveAt(best);
            }
            return path;
        }

        // lots by Sims on them, most first; lots with none fill up the list in their own order
        static List<Lot> PopulatedLots(int k)
        {
            Dictionary<ulong, int> n = new Dictionary<ulong, int>();
            foreach (Sim s in Sims3.Gameplay.Queries.GetObjects<Sim>())
                if (s != null && s.LotCurrent != null && !s.LotCurrent.IsWorldLot)
                {
                    int v; n.TryGetValue(s.LotCurrent.LotId, out v); n[s.LotCurrent.LotId] = v + 1;
                }
            List<Lot> c = JumpLots();
            c.Sort(delegate(Lot x, Lot y)
            {
                int vx, vy; n.TryGetValue(x.LotId, out vx); n.TryGetValue(y.LotId, out vy);
                return vy.CompareTo(vx);
            });
            if (c.Count > k) c.RemoveRange(k, c.Count - k);
            return c;
        }

        // ------------------------------------------------------------ spawned Sims
        static readonly List<ulong> sSpawned = new List<ulong>();

        // a new young adult, adult or elder joins a random non-active household with a home lot and stands within 6 m of pos
        static int SpawnAround(Vector3 pos, int n)
        {
            List<Household> homes = new List<Household>();
            foreach (Household h in Household.sHouseholdList)
                if (!h.IsActive && !h.IsSpecialHousehold && h.LotHome != null) homes.Add(h);
            if (homes.Count == 0) return 0;
            int made = 0;
            for (int i = 0; i < n; i++)
            {
                Household h = RandomUtil.GetRandomObjectFromList(homes);
                SimUtils.SimCreationSpec spec = new SimUtils.SimCreationSpec();
                spec.Age = CASAgeGenderFlags.YoungAdult | CASAgeGenderFlags.Adult | CASAgeGenderFlags.Elder;
                spec.Normalize();
                SimDescription sd = spec.Instantiate();
                h.Add(sd);
                Vector3 p = pos;
                p.x += RandomUtil.GetFloat(-6f, 6f);
                p.z += RandomUtil.GetFloat(-6f, 6f);
                sSpawned.Add(sd.SimDescriptionId);
                if (sd.Instantiate(p) != null) made++;
            }
            return made;
        }

        // humans of child age or older in their normal form on one lot switch to the outfit category: one Sim build each
        static int ChangeOutfitsOn(Lot lot, int max, OutfitCategories cat)
        {
            int done = 0;
            foreach (Sim s in Sims3.Gameplay.Queries.GetObjects<Sim>())
            {
                if (done >= max) break;
                SimDescription sd = s == null ? null : s.SimDescription;
                if (sd == null || s.LotCurrent != lot || !sd.IsHuman || !sd.ChildOrAbove || sd.IsSupernaturalForm) continue;
                s.SwitchToOutfitWithoutSpin(Sim.ClothesChangeReason.Force, cat);
                done++;
            }
            return done;
        }

        // remove_sims {}: removes and disposes every Sim the agent spawned; no Sim of the save is touched
        static object RemoveSims(Args a)
        {
            Dictionary<string, object> r = Obj();
            r["removed"] = RemoveSpawned();
            return r;
        }

        // A Sim in a vehicle or walking with a linked partner is reset first (the game's own reset gets it out) and disposed
        // 30 ticks later: disposing a rider in place can leave its partner linked to a destroyed agent.
        static int RemoveSpawned()
        {
            int removed = 0;
            List<SimDescription> later = new List<SimDescription>();
            foreach (ulong id in sSpawned.ToArray())
            {
                SimDescription sd = SimDescription.Find(id);
                sSpawned.Remove(id);
                if (sd == null) continue;
                Sim s = sd.CreatedSim;
                if (s != null && !s.HasBeenDestroyed && (s.Parent != null || s.IsRouting)) { s.SetObjectToReset(); later.Add(sd); continue; }
                if (sd.Household != null) sd.Household.Remove(sd);
                sd.Dispose(true, false, true);
                removed++;
            }
            if (later.Count > 0)
                Simulator.AddObject(new OneShotFunctionTask(delegate
                {
                    Simulator.Sleep(30);
                    foreach (SimDescription sd in later)
                    {
                        try { if (sd.Household != null) sd.Household.Remove(sd); sd.Dispose(true, false, true); }
                        catch (Exception) { } // already gone with the world
                    }
                }));
            return removed + later.Count;
        }

        // ------------------------------------------------------------ vfx_burst
        static volatile int sVfxGen;
        static int sVfxStarted, sVfxMissing, sVfxRounds;
        static readonly string[] kVfxDefault = { "fireSystemFx", "fireSystemMidFx", "ep7WandSpellIce_main", "ep7FairyFlight_main",
            "ep11LightningHit_main", "ep8LightningHit", "ep7BuffSparkly_main", "ep7WandFail_main" };

        // vfx_burst {names?, count?, radius?, seconds?, restartS?} | {stop:true} | {status:true}: a game task creates `count`
        // (default 16) effects, names round-robin (default: the game's own fire, ice spell, fairy, lightning and sparkle
        // effect names), on a ring of `radius` m (default 8) around the camera target, starts them, and every restartS
        // (default 4) wall-clock seconds stops, disposes and re-creates them at the current target, for `seconds` (default
        // 600). Effects only: no fire object, nothing burns, spreads or saves. An effect a pack lacks counts as missing.
        // A new vfx_burst or {stop:true} ends a running one and disposes its effects.
        static object VfxBurst(Args a)
        {
            Dictionary<string, object> r = Obj();
            if (a.Has("status") || a.Has("stop"))
            {
                if (a.Has("stop")) { ++sVfxGen; r["stopped"] = true; }
                r["rounds"] = sVfxRounds; r["started"] = sVfxStarted; r["missing"] = sVfxMissing;
                return r;
            }
            RequireLive();
            int gen = ++sVfxGen;
            List<string> names = new List<string>();
            List<object> given = a.Raw("names") as List<object>;
            if (given != null) foreach (object o in given) names.Add(Convert.ToString(o));
            if (names.Count == 0) names.AddRange(kVfxDefault);
            int count = Int(a, "count", 16), totalMs = (int)(a.Double("seconds", 600) * 1000), roundMs = (int)(a.Double("restartS", 4) * 1000);
            float radius = (float)a.Double("radius", 8);
            if (count < 1 || count > 64 || roundMs < 500) throw new AgentException("count 1..64, restartS >= 0.5");
            sVfxStarted = sVfxMissing = sVfxRounds = 0;
            Simulator.AddObject(new OneShotFunctionTask(delegate
            {
                List<VisualEffect> live = new List<VisualEffect>();
                int t0 = System.Environment.TickCount;
                try
                {
                    while (sVfxGen == gen && System.Environment.TickCount - t0 < totalMs)
                    {
                        DisposeAll(live);
                        Vector3 c = CameraController.GetTarget();
                        for (int i = 0; i < count; i++)
                        {
                            VisualEffect fx = VisualEffect.Create(names[i % names.Count]);
                            if (fx == null) { sVfxMissing++; continue; }
                            double ang = 2 * Math.PI * i / count;
                            float x = c.x + radius * (float)Math.Cos(ang), z = c.z + radius * (float)Math.Sin(ang);
                            fx.SetPosAndOrient(new Vector3(x, World.GetTerrainHeight(x, z), z), Vector3.UnitZ, Vector3.UnitY);
                            fx.Start();
                            live.Add(fx);
                            sVfxStarted++;
                        }
                        sVfxRounds++;
                        int r0 = System.Environment.TickCount;
                        while (sVfxGen == gen && System.Environment.TickCount - r0 < roundMs) Simulator.Sleep(0);
                    }
                }
                finally { DisposeAll(live); }
            }));
            r["names"] = names;
            r["count"] = count;
            return r;
        }

        static void DisposeAll(List<VisualEffect> live)
        {
            foreach (VisualEffect fx in live)
            {
                try { fx.Stop(); fx.Dispose(); }
                catch (Exception) { } // already gone with the world
            }
            live.Clear();
        }

        // ------------------------------------------------------------ set_mode
        // set_mode {mode: build|buy|cas|edittown|live}: the game's own transitions, run as a game task; the runner waits for
        // the sub state (BuildModeState, BuyModeState, CASFullModeState, EditTownState, LiveModeState). build and buy open
        // on the active household's lot; cas edits the active Sim (else the first household member CAS accepts) on a copy
        // and live from CAS leaves it without saving, as the close button's "exit without saving" does.
        static object SetMode(Args a)
        {
            string mode = a.Str("mode").ToLowerInvariant(), from = SubState();
            Dictionary<string, object> r = Obj();
            r["from"] = from;
            if (mode == "live")
            {
                if (from == "CASFullModeState")
                    RunAsync("set_mode live (CAS)", delegate
                    {
                        ICASModel m = Sims3.Gameplay.UI.Responder.Instance.CASModel;
                        int t0 = System.Environment.TickCount;
                        while (m.IsProcessing() && System.Environment.TickCount - t0 < 30000) Simulator.Sleep(0);
                        m.RequestClearChangeReport();
                        CASController.Singleton.Shutdown(); // CASModel.Done -> Quit -> GameStates.ReturnFromCAS
                    });
                else RunAsync("set_mode live", delegate { GameStates.TransitionToLiveMode(); });
                return r;
            }
            RequireLive();
            Household hh = Household.ActiveHousehold;
            Lot home = hh == null ? null : hh.LotHome;
            switch (mode)
            {
                case "build":
                    if (home == null) throw new AgentException("no home lot for the active household");
                    RunAsync("set_mode build", delegate { GameStates.TransitionToBuildMode(home, null); });
                    break;
                case "buy":
                    if (home == null) throw new AgentException("no home lot for the active household");
                    RunAsync("set_mode buy", delegate { GameStates.TransitionToBuyMode(home, null); });
                    break;
                case "edittown":
                    RunAsync("set_mode edittown", delegate { GameStates.TransitionToEditTown(); });
                    break;
                case "cas":
                    SimDescription sd = CasSim();
                    if (sd == null) throw new AgentException("no Sim of the active household can be edited in CAS");
                    r["sim"] = sd.FullName;
                    RunAsync("set_mode cas", delegate
                    {
                        CASLogic cas = CASLogic.GetSingleton();
                        cas.LoadSim(sd, OutfitCategories.Everyday, 0);
                        cas.UseTempSimDesc = true;
                        GameStates.TransitionToCASMode();
                    });
                    break;
                default: throw new AgentException("mode must be build, buy, cas, edittown or live");
            }
            return r;
        }

        // the selected Sim, else the first household member that CAS accepts (human, not a baby, not pregnant)
        static SimDescription CasSim()
        {
            Household hh = Household.ActiveHousehold;
            if (hh == null) return null;
            List<Sim> c = new List<Sim>();
            if (PlumbBob.SelectedActor != null) c.Add(PlumbBob.SelectedActor);
            foreach (Sim s in hh.Sims) c.Add(s);
            foreach (Sim s in c)
            {
                SimDescription sd = s.SimDescription;
                if (sd.Household == hh && s.IsHuman && !sd.Baby && !sd.IsPregnant && !sd.IsVisuallyPregnant && sd.GetOutfitCount(OutfitCategories.Everyday) > 0) return sd;
            }
            return null;
        }
    }
}
