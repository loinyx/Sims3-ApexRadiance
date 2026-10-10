from pathlib import Path
import sys

# Exercise the actual persistence/package functions without linking the game's patch registry.
s = Path('apex_config.cpp').read_text(encoding='utf-8')
def between(start, end):
    a=s.index(start)
    return s[a:s.index(end,a)]
out = '''#include "apex_config.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "apex_log.h"
#include "apex_version.h"
#include "profile_package.h"
#include "performance.h"
#include <sstream>
#include <algorithm>
#include <cctype>
namespace ApexConfig { namespace {
'''
out += between('std::string Serialize(', '// Copies the keys')
out += between('std::string Upper(', '} // namespace')
out += between('unsigned FeaturePart(', 'bool IsProfileFeature(')
out += '}\n'
out += between('const char* ProfilePartName(', 'void CaptureProfileState(')
out += s[s.index('bool ReadProfile('):]
Path(sys.argv[1]).write_text(out,encoding='utf-8')
