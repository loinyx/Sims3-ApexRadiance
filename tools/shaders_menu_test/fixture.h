#pragma once
class ApexPatch;
namespace Fixture {
ApexPatch* StartLooks(); // the real LooksPatch, installed with the D3D side stubbed
// The look's state: compiling = a quality setting compiles; edited = values and settings differ from the look;
// offer = the import note card; batch: 0 no downloads, 1 running (Waiting/Downloading/Done/failed rows), 2 finished
// twin: MXAO runs and Apex's Ambient Occlusion and a LUT are on (the amber rows of the look card and list)
void LooksState(bool compiling, bool edited, bool offer, int batch, bool twin = false);
// Edge cases after LooksState: 0 none, 1 no look chosen, 2 an empty look, 3 every built-in shader with long names,
// 4 nothing installed (no pack downloaded, no file of the player's)
void EdgeState(int edge);
// confirm: the removal confirmation of Packs and files open on 0 nothing, 1 a pack, 2 one of the player's .fx files
void PacksState(int confirm);
// The live viewer: downloads progress over time
void Live();
// Save to this look as text (LookAsText) and read back (ParsePreset): removed shaders leave the file, switched-off ones
// stay listed (ApexOff=), the rest run, and the changes count as unsaved until then. Prints each failure; true = all good.
bool SaveRoundTrip();
// 0 idle, 1 recording, 2 saving; options: 0 defaults, 1 every optional row shown
void CaptureState(int video, int options);
}
