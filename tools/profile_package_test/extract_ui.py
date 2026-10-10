from pathlib import Path
import sys
s=Path('apex_gui.cpp').read_text(encoding='utf-8')
def part(a,b):
    start=s.index(a)
    return s[start:s.index(b,start)]
root=Path(sys.argv[1])
root.joinpath('preset_state.inc').write_text(part('struct ProfileItem {','void PickProfileFile('),encoding='utf-8')
root.joinpath('preset_helpers.inc').write_text(part('int ProfileNameFilter(','// Opens the Profiles folder')+part('void ProfileIconPicker(','void StartPresetDialog('),encoding='utf-8')
root.joinpath('preset_page.inc').write_text(part('void StartPresetDialog(','void CompatibilityTab('),encoding='utf-8')
root.joinpath('preset_footer.inc').write_text(part('bool ReportDialogActions(','void ReportOptionalNotes('),encoding='utf-8')
widgets=Path('ui/widgets.cpp').read_text(encoding='utf-8')
root.joinpath('widgets_recorded.cpp').write_text(widgets.replace('bool IconTextButton(const char* label,','bool RecordedIconTextButton(const char* label,',1).replace('bool TextButton(const char* label,','bool RecordedTextButton(const char* label,',1),encoding='utf-8')
# CPU previews use the same rasterizer as the existing Report fixture, with synchronous PPM output.
render=Path('tools/report_check/menu_253_check.cpp').read_text(encoding='utf-8')
render=render[render.index('void Rasterize('):render.index('\nint main(')]
render=render[:render.index('    { std::lock_guard<std::mutex> lk(Captures::g_lock);')]
render+='''    std::ofstream output(file,std::ios::binary);
    output << "P6\\n" << width << " " << height << "\\n255\\n";
    for(size_t i=0;i<bgr.size();i+=3) {output.put(char(bgr[i+2]));output.put(char(bgr[i+1]));output.put(char(bgr[i]));}
}
'''
root.joinpath('preset_raster.inc').write_text(render,encoding='utf-8')
