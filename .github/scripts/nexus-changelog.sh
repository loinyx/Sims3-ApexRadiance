#!/usr/bin/env bash
# Turns a GitHub release body (markdown, the shape of the release notes) into Nexus text.
#   nexus-changelog.sh --changelog        < notes.md   the Changelogs tab: one line per change (each line is a list item
#                                                      on Nexus); sub-items joined into their parent line with "; "
#   nexus-changelog.sh --description X.Y.Z < notes.md  the file description: a header, UPPER-CASE sections, "•" items,
#                                                      blank lines between sections, how to install at the end
# Release notes shape: "## Section", "- **Title:** text", "  - sub-item", plain paragraphs; "## Install" and what follows
# is left out (the description adds its own). Markdown marks go; backslashes go too (Nexus drops them): paths "A > B".
MODE="${1:---changelog}"
VERSION="${2:-}"
awk -v mode="$MODE" -v version="$VERSION" '
function clean(s) {
    gsub(/\*\*/, "", s); gsub(/`/, "", s)
    gsub(/\\[ .,;:)]/, "&", s)                       # (kept: handled below)
    while (match(s, /\\([ .,;:)]|$)/)) s = substr(s, 1, RSTART - 1) substr(s, RSTART + 1)
    gsub(/\\/, " > ", s)
    gsub(/[ \t]+$/, "", s)
    return s
}
function flush() {                                  # the pending item (with its sub-items)
    if (item == "") return
    line = item
    if (nsub > 0) {
        for (i = 1; i <= nsub; i++) sub(/[;,.]+$/, "", subs[i]) # their own punctuation goes: "; " joins them
        joined = subs[1]; for (i = 2; i <= nsub; i++) joined = joined "; " subs[i]
        line = line (line ~ /:$/ ? " " : ": ") joined "."
    }
    if (mode == "--description") print "• " line
    else print line
    item = ""; nsub = 0; collect = 0
}
function section(title) {
    flush()
    if (mode != "--description") return
    printf "%s%s\n", (printed ? "\n" : ""), toupper(title)
    printed = 1
}
BEGIN {
    item = ""; nsub = 0; printed = 0
    if (mode == "--description") {
        print "Apex Radiance " version ": lighting, visuals and performance for The Sims 3."
        printed = 1
    }
}
/^## Install/ { exit }
/^## / { section(clean(substr($0, 4))); next }
/^[ \t]+[-*] / { sub(/^[ \t]+[-*] /, ""); subs[++nsub] = clean($0); next }
/^[-*] / {
    if (collect) { sub(/^[-*] /, ""); subs[++nsub] = clean($0); next } # the list a "...:" paragraph introduces
    flush(); sub(/^[-*] /, ""); item = clean($0); next
}
/^[ \t]*$/ { flush(); next }
{   # a paragraph line: its own item (or the continuation of the current one)
    if (item != "" && nsub == 0) { item = item " " clean($0); next }
    flush(); item = clean($0); collect = item ~ /:$/
}
END {
    flush()
    if (mode == "--description") {
        print ""
        print "INSTALL"
        print "Run ApexRadianceSetup.exe (recommended), or by hand: close the game and copy ApexRadiance.asi from the Manual install folder into The Sims 3 > Game > Bin (needs an ASI loader). Your settings are kept."
    }
}'
