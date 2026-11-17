# The HLSL sources of a C++ file: each R"HLSL( ... )HLSL" literal, with adjacent literals (a source split because one
# literal is limited to 16 KB) joined into one source. Dot-sourced by the G-buffer checks' run.ps1.
function Get-HlslBlocks([string]$Path) {
    $text = Get-Content -LiteralPath $Path -Raw
    $blocks = [System.Collections.Generic.List[string]]::new()
    $last = -1
    foreach ($m in [regex]::Matches($text, 'R"HLSL\((.*?)\)HLSL"', [System.Text.RegularExpressions.RegexOptions]::Singleline)) {
        if ($last -ge 0 -and $text.Substring($last, $m.Index - $last).Trim() -eq '') { $blocks[$blocks.Count - 1] += $m.Groups[1].Value }
        else { $blocks.Add($m.Groups[1].Value) }
        $last = $m.Index + $m.Length
    }
    return , $blocks.ToArray()
}
