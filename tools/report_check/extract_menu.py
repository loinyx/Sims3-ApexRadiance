from pathlib import Path
import sys
source = Path('apex_gui.cpp').read_text(encoding='utf-8')
report_start = source.index('struct ReportState {')
report_end = source.index('// ---- System > Developer', report_start)
Path(sys.argv[1]).write_text(source[report_start:report_end], encoding='utf-8')
Path(sys.argv[1]).with_name('notice_under_test.inc').write_text(
    source[source.index('void PlaceScreenNotice('):source.index('void Banner()')], encoding='utf-8')
# The native interaction fixture wraps the real button to collect its rectangle and disabled state.
widgets = Path('ui/widgets.cpp').read_text(encoding='utf-8')
Path(sys.argv[1]).with_name('widgets_recorded.cpp').write_text(
    widgets.replace('bool IconTextButton(const char* label,', 'bool RecordedIconTextButton(const char* label,', 1)
           .replace('bool BeginAdvanced(const char* id,', 'bool RecordedBeginAdvanced(const char* id,', 1), encoding='utf-8')
recorder = Path('features/recorder.cpp').read_text(encoding='utf-8')
Path(sys.argv[1]).with_name('recorder_under_test.inc').write_text(recorder[recorder.index('namespace Recorder {'):], encoding='utf-8')
gate_start = source.index('std::atomic<bool> g_menuAvailable')
Path(sys.argv[1]).with_name('startup_gate_under_test.inc').write_text(source[gate_start:source.index('bool BannerNeeded()', gate_start)], encoding='utf-8')

# Real profile picker for the multi-frame control sizing/style-stack regression fixture.
Path(sys.argv[1]).with_name("profile_picker_under_test.inc").write_text(
    source[source.index("void ProfileIconPicker("):source.index("void StartPresetDialog(")], encoding="utf-8")
