"""Builds features/looks_packs_index.h, the index of the shader packs on ReShade's official list.

ReShade's list is EffectPackages.ini on the `list` branch of crosire/reshade-shaders (the list ReShade's own installer
shows). For each pack the index keeps names only, never shader code: the pack's name, author, repository, branch and
the commit read, the licence GitHub reports, an approximate download size, and every .fx file with the techniques a
light scan finds in it (`technique <Name>` outside comments). Looks uses it to say which pack holds a missing .fx file
and to list the packs in Get shader packs; the packs themselves are downloaded by the player, from their authors.

Needs the GitHub CLI (`gh`, signed in). Reads only; the .fx files read for the scan are cached in --cache.
  python -I tools/looks_packs_index.py [--cache <folder>] [--list <EffectPackages.ini>]
"""
import argparse, base64, concurrent.futures, json, os, re, subprocess, sys

APEX = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HEADER = os.path.join(APEX, "features", "looks_packs_index.h")

# short names for packs whose ReShade name is generic (the menu shows "<name>" and "<author>" apart)
NAMES = {
    "crosire/reshade-shaders@slim": "ReShade standard effects",
    "crosire/reshade-shaders@legacy": "ReShade legacy effects",
    "prod80/prod80-ReShade-Repository": "prod80 color effects",
    "Daodan317081/reshade-shaders": "Daodan shaders",
    "BarbatosBachiko/Reshade-Shaders": "Barbatos shaders",
    "brussell1/Shaders": "brussell shaders",
}
TEXT_EXT = (".fx", ".fxh", ".txt", ".md", ".ini", ".h", ".hlsl", ".glsl", ".cfg", ".json", ".py", ".bat", ".cs", ".cpp")
IMAGE_EXT = (".png", ".jpg", ".jpeg", ".bmp", ".dds", ".tga")


def gh(path, raw=False):
    args = ["gh", "api", path] + (["-H", "Accept: application/vnd.github.raw"] if raw else [])
    r = subprocess.run(args, capture_output=True)
    if r.returncode:
        return None
    return r.stdout if raw else json.loads(r.stdout)


def packages(text):
    out, cur = [], None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("["):
            cur = {}
            out.append(cur)
        elif cur is not None and "=" in line:
            k, v = line.split("=", 1)
            cur[k] = v
    return out


def techniques(src):
    src = re.sub(r"/\*.*?\*/", " ", src.decode("utf-8", "replace"), flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    seen = []
    for m in re.finditer(r"^\s*technique\s+([A-Za-z_]\w*)", src, re.M):
        if m.group(1) not in seen:
            seen.append(m.group(1))
    return seen


def shader_path(path):
    """Where Apex puts an .fx: under the pack's Shaders\\, relative to the first "Shaders" folder (as ReShade's installer)"""
    parts = path.split("/")
    low = [p.lower() for p in parts]
    return "/".join(parts[low.index("shaders") + 1:]) if "shaders" in low[:-1] else path


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=os.path.join(os.environ.get("TEMP", "."), "apex_packs_index_cache"))
    ap.add_argument("--list")
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    if a.list:
        ini = open(a.list, "rb").read().decode("utf-8")
    else:
        ini = base64.b64decode(gh("repos/crosire/reshade-shaders/contents/EffectPackages.ini?ref=list")["content"]).decode("utf-8")
    packs = []
    for p in packages(ini):
        m = re.match(r"https://github\.com/([^/]+)/([^/]+)/archive/(?:refs/heads/)?(.+)\.zip$", p.get("DownloadUrl", ""))
        if not m:
            print("skipped (no GitHub zip):", p.get("PackageName"), file=sys.stderr)
            continue
        owner, repo, branch = m.groups()
        full = f"{owner}/{repo}"
        name, _, author = p["PackageName"].partition(" by ")
        name = NAMES.get(f"{full}@{branch}", NAMES.get(full, name.strip()))
        head = gh(f"repos/{full}/commits/{branch}")
        if not head:  # a renamed branch (GitHub still redirects the list's old zip link): the default branch
            branch = gh(f"repos/{full}")["default_branch"]
            head = gh(f"repos/{full}/commits/{branch}")
        commit = head["sha"]
        tree = gh(f"repos/{full}/git/trees/{commit}?recursive=1")["tree"]
        lic = gh(f"repos/{full}/license")
        spdx = lic["license"]["spdx_id"] if lic and lic.get("license") else ""
        licence = "" if not spdx else "Author's terms" if spdx == "NOASSERTION" else spdx
        blobs = [t for t in tree if t["type"] == "blob"]
        # a zip of the branch: text compresses to about a third, images hardly at all
        zip_bytes = sum(t["size"] * (0.3 if t["path"].lower().endswith(TEXT_EXT) else 1.0) for t in blobs)
        deny = {x.strip().lower() for x in p.get("DenyEffectFiles", "").split(",") if x.strip()}
        fx = sorted((t for t in blobs if t["path"].lower().endswith(".fx") and os.path.basename(t["path"]).lower() not in deny),
                    key=lambda t: os.path.basename(t["path"]).lower())

        def scan(t):
            cached = os.path.join(a.cache, t["sha"])
            if not os.path.exists(cached):
                data = gh(f"repos/{full}/contents/{t['path']}?ref={commit}", raw=True) or b""
                open(cached, "wb").write(data)
            return techniques(open(cached, "rb").read())

        with concurrent.futures.ThreadPoolExecutor(8) as ex:
            techs = list(ex.map(scan, fx))
        folder = re.sub(r"[^A-Za-z0-9 ._-]", "", name).strip() or repo
        packs.append(dict(rank=len(packs), name=name, author=author.strip() or owner, folder=folder, repo=full, branch=branch, commit=commit,
                          licence=licence, kb=int(zip_bytes / 1024) + 1,
                          files=[(os.path.basename(t["path"]), techs[i]) for i, t in enumerate(fx)]))
        print(f"{name}: {len(fx)} .fx, {sum(len(x) for x in techs)} techniques, {licence or 'no licence'}", file=sys.stderr)
    packs.sort(key=lambda p: p["name"].lower())

    out = ["// Generated by tools/looks_packs_index.py from ReShade's official pack list (crosire/reshade-shaders, branch list):",
           "// names only, no shader code. Do not edit; run the script again.",
           "#pragma once",
           "#include <cstdint>",
           "",
           "namespace Looks::Packs {",
           "",
           "struct IndexFile { const char* file; const char* techniques; }; // techniques: comma-separated",
           "struct IndexPack",
           "{",
           "    const char* name; const char* author; const char* folder; // folder: Shaders\\Packs\\<folder>",
           "    const char* repo; const char* branch; const char* commit;  // github.com/<repo>, the branch downloaded, the commit indexed",
           "    const char* licence;                                     // SPDX id, \"Author's terms\", or \"\" (none stated)",
           "    uint32_t downloadKB;                                     // approximate size of the download",
           "    uint16_t techniques;                                     // found by the scan",
           "    uint16_t rank;                                           // place on ReShade's list (a file in several packs: the first)",
           "    uint16_t fileCount;",
           "    const IndexFile* files;",
           "};",
           ""]
    for i, p in enumerate(packs):
        out.append(f"inline constexpr IndexFile kFiles{i}[] = {{")
        for f, t in p["files"]:
            out.append(f"    {{ {c_str(f)}, {c_str(','.join(t))} }},")
        if not p["files"]:
            out.append("    { \"\", \"\" },")
        out.append("};")
    out.append("")
    out.append("inline constexpr IndexPack kIndex[] = {")
    for i, p in enumerate(packs):
        n = sum(len(t.split(",")) if t else 0 for _, t in [(f, ",".join(t)) for f, t in p["files"]])
        out.append(f"    {{ {c_str(p['name'])}, {c_str(p['author'])}, {c_str(p['folder'])}, {c_str(p['repo'])}, {c_str(p['branch'])}, "
                   f"{c_str(p['commit'])}, {c_str(p['licence'])}, {p['kb']}u, {n}, {p['rank']}, {len(p['files'])}, kFiles{i} }},")
    out.append("};")
    out.append("inline constexpr int kIndexCount = (int)(sizeof(kIndex) / sizeof(kIndex[0]));")
    out.append("")
    out.append("} // namespace Looks::Packs")
    open(HEADER, "w", newline="\n", encoding="utf-8").write("\n".join(out) + "\n")
    print(f"{HEADER}: {len(packs)} packs", file=sys.stderr)


if __name__ == "__main__":
    main()
