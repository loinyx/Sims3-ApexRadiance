from pathlib import Path
import sys
r=Path(__file__).resolve().parents[2]
s=(r/'apex_gui.cpp').read_text(encoding='utf-8-sig')
Path(sys.argv[1]).write_text(s[s.index('bool PublicTestPending()'):s.index('// The version button:',s.index('bool PublicTestPending()'))],encoding='utf-8')
w=(r/'ui/widgets.cpp').read_text(encoding='utf-8-sig')
Path(sys.argv[1]).with_name('widgets_recorded.cpp').write_text(w.replace('bool IconTextButton(const char* label,','bool RecordedIconTextButton(const char* label,',1),encoding='utf-8')
