#pragma once
class ApexPatch;
namespace Fixture {
ApexPatch* StartLooks(); // the real LooksPatch, installed with the D3D side stubbed
// The look's state: compiling = a quality setting compiles; edited = values and settings differ from the look;
// offer = the import note card; batch: 0 no downloads, 1 running (Waiting/Downloading/Done/failed rows), 2 finished
// twin: MXAO runs and Apex's Ambient Occlusion and a LUT are on (the amber rows of the look card and list)
void LooksState(bool compiling, bool edited, bool offer, int batch, bool twin = false);
// picks: packs ticked on the Packs tab (the bar is choosing); confirm: the Library's removal confirmation open on
// 0 nothing, 1 a pack, 2 one of the player's .fx files
void PacksState(bool picks, int confirm);
// 0 idle, 1 recording, 2 saving; options: 0 defaults, 1 every optional row shown
void CaptureState(int video, int options);
}
