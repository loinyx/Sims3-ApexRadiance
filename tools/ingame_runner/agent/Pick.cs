// Pick commands of the in-game test agent: what the game's own scene pick holds under the cursor, where a Sim, object or
// ground point shows on screen, and a Sim walk, so a job can aim the mouse at a target and check what it picks.
//   pick       the scene window's current pick: SceneMgrWindow.GetPickArgs (UIManager.GetPickInfo), the pick the game's
//              hover, tooltips and clicks read; type (ScenePickObjectType: Sim, Object, Floor, Terrain, Wall ...), object
//              id, the game's cursor point, the world point, and the object's class and name
//   screenpos  a Sim ("selected" or a full or first name), an object (class or catalog name, nearest the selected Sim) or
//              a world point, plus an offset, projected to screen pixels with World.GetScreenSpacePos; focus: true first
//              points the camera at it (the projection then shows the old camera until the next frame: call again)
//   walk       a Sim walks to a point through the game's own Go Here interaction (Terrain.GoHere), as a player's click does
using System;
using System.Collections.Generic;
using Sims3.Gameplay;
using Sims3.Gameplay.Abstracts;
using Sims3.Gameplay.Actors;
using Sims3.Gameplay.Autonomy;
using Sims3.Gameplay.Core;
using Sims3.Gameplay.Interactions;
using Sims3.SimIFace;
using Sims3.UI;

namespace ApexTestAgent
{
    public static partial class Handlers
    {
        static void RegisterPick()
        {
            Dispatcher.Register("pick", PickNow);
            Dispatcher.Register("screenpos", ScreenPos);
            Dispatcher.Register("walk", Walk);
        }

        static object PickNow(Args a)
        {
            RequireLive();
            SceneMgrWindow w = UIManager.GetSceneWindow();
            if (w == null) throw new AgentException("no scene window");
            ScenePickArgs p = w.GetPickArgs();
            Dictionary<string, object> r = Obj();
            r["type"] = p.mObjectType.ToString();
            r["x"] = p.mX;
            r["y"] = p.mY;
            r["world"] = Pos(p.mWorldPos);
            Describe(r, p.mObjectId);
            return r;
        }

        // id (hex: an object id is a 64-bit value), and for a game object its class and name (a Sim's full name, else the
        // catalog name); ids differ between loads, names do not
        static void Describe(Dictionary<string, object> r, ulong id)
        {
            r["id"] = id.ToString("X16");
            GameObject o = id == 0 ? null : GameObject.GetObject(new ObjectGuid(id));
            if (o == null) return;
            r["class"] = o.GetType().Name;
            Sim s = o as Sim;
            string name = null;
            if (s != null && s.SimDescription != null) name = s.SimDescription.FullName;
            else { try { name = o.CatalogName; } catch (Exception) { } }
            r["name"] = string.IsNullOrEmpty(name) ? o.GetType().Name : name;
        }

        static Sim FindSim(string name)
        {
            if (name == "selected")
            {
                if (Sim.ActiveActor == null) throw new AgentException("no selected Sim");
                return Sim.ActiveActor;
            }
            foreach (Sim s in Sims3.Gameplay.Queries.GetObjects<Sim>())
            {
                if (s.SimDescription == null) continue;
                if (string.Equals(s.SimDescription.FullName, name, StringComparison.OrdinalIgnoreCase)) return s;
            }
            foreach (Sim s in Sims3.Gameplay.Queries.GetObjects<Sim>())
                if (s.SimDescription != null && string.Equals(s.SimDescription.FirstName, name, StringComparison.OrdinalIgnoreCase)) return s;
            throw new AgentException("no Sim named '" + name + "'");
        }

        // the object whose class or catalog name contains `name`, nearest the selected Sim (else the camera target), on
        // that point's lot (else within 40 m)
        static GameObject FindObject(string name)
        {
            Vector3 c = Sim.ActiveActor != null ? Sim.ActiveActor.Position : CameraController.GetTarget();
            Lot lot = LotManager.GetLotAtPoint(c);
            IEnumerable<GameObject> all = lot != null && !lot.IsWorldLot
                ? (IEnumerable<GameObject>)lot.GetObjects<GameObject>() : Sims3.Gameplay.Queries.GetObjects<GameObject>(c, 40f);
            string want = name.ToLowerInvariant();
            GameObject best = null;
            float bestD = float.MaxValue;
            foreach (GameObject o in all)
            {
                if (o == null || o is Sim) continue;
                string cat = null;
                try { cat = o.CatalogName; } catch (Exception) { }
                if (!o.GetType().Name.ToLowerInvariant().Contains(want) && (cat == null || !cat.ToLowerInvariant().Contains(want))) continue;
                float d = (o.Position - c).LengthSqr();
                if (d < bestD) { bestD = d; best = o; }
            }
            if (best == null) throw new AgentException("no object '" + name + "' near " + c);
            return best;
        }

        // screenpos {sim | object | at:{x,y,z}, dx?, dy?, dz?, focus?, height? (8 m)}: the point (the target's position
        // plus the offset; dy lifts it, so a still cursor above a ground point looks at a Sim standing there) in screen
        // pixels; base is the point without dy (where a walk to it ends)
        static object ScreenPos(Args a)
        {
            RequireLive();
            Dictionary<string, object> r = Obj();
            GameObject o = null;
            Vector3 at;
            if (a.Has("sim")) { o = FindSim(a.Str("sim")); at = o.Position; }
            else if (a.Has("object")) { o = FindObject(a.Str("object")); at = o.Position; }
            else if (a.Has("at")) at = Vec(a, "at");
            else at = CameraController.GetTarget();
            Vector3 basePt = at + new Vector3((float)a.Double("dx", 0), 0, (float)a.Double("dz", 0));
            Vector3 pt = basePt + new Vector3(0, (float)a.Double("dy", 0), 0);
            if (a.Bool("focus", false))
            {
                float h = (float)a.Double("height", 8);
                CameraController.SetPositionAndTarget(pt + new Vector3(-h * 0.7f, h, -h * 0.7f), pt);
            }
            Vector2 sp;
            r["on_screen"] = World.GetScreenSpacePos(pt, out sp);
            r["x"] = Math.Round(sp.x, 2);
            r["y"] = Math.Round(sp.y, 2);
            r["world"] = Pos(pt);
            r["base"] = Pos(basePt);
            if (o != null) Describe(r, o.ObjectId.Value);
            return r;
        }

        // walk {sim? ("selected"), to:{x,y,z}}: the Sim's queue is cleared and Go Here to the point added, user-directed
        static object Walk(Args a)
        {
            RequireLive();
            Sim s = FindSim(a.Str("sim", "selected"));
            Vector3 to = Vec(a, "to");
            InteractionDefinition def = Terrain.GoHere.GetSingleton(s, to);
            Lot lot = LotManager.GetLotAtPoint(to);
            GameObjectHit hit = new GameObjectHit(lot != null && !lot.IsWorldLot ? GameObjectHitType.LotTerrain : GameObjectHitType.Terrain);
            hit.mPoint = to;
            InteractionInstanceParameters p = new InteractionInstanceParameters(new InteractionObjectPair(def, Terrain.Singleton), s,
                new InteractionPriority(InteractionPriorityLevel.UserDirected), false, true, hit);
            InteractionInstance ii = def.CreateInstanceFromParameters(ref p);
            s.InteractionQueue.CancelAllInteractions();
            Dictionary<string, object> r = Obj();
            r["queued"] = s.InteractionQueue.Add(ii);
            r["sim"] = s.SimDescription != null ? s.SimDescription.FullName : "";
            r["from"] = Pos(s.Position);
            r["to"] = Pos(to);
            return r;
        }
    }
}
