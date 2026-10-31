// Minimal JSON reader and writer for the in-game test agent. From ts3agent (sims3-modernization, MIT), forked from
// Sims3-MCP (MIT, Copyright (c) 2026 Critical-Reynolds, see LICENSE-Sims3-MCP).
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace ApexTestAgent
{
    // Minimal JSON reader/writer. The game ships no JSON library and the
    // sandboxed mscorlib is old, so keep this to plain C# 2/3 features.
    public static class Json
    {
        // ------------------------------------------------------------ writing
        public static string Write(object value)
        {
            StringBuilder sb = new StringBuilder();
            WriteValue(sb, value, 0);
            return sb.ToString();
        }

        static void WriteValue(StringBuilder sb, object v, int depth)
        {
            if (depth > 32) { sb.Append("\"<max depth>\""); return; }
            if (v == null) { sb.Append("null"); return; }
            if (v is string) { WriteString(sb, (string)v); return; }
            if (v is bool) { sb.Append((bool)v ? "true" : "false"); return; }
            if (v is float)
            {
                // Format as float so 64.6f doesn't become 64.5999984741211.
                float f = (float)v;
                if (float.IsNaN(f) || float.IsInfinity(f)) sb.Append("null");
                else sb.Append(f.ToString("R", CultureInfo.InvariantCulture));
                return;
            }
            if (v is double)
            {
                double d = (double)v;
                if (double.IsNaN(d) || double.IsInfinity(d)) sb.Append("null");
                else sb.Append(d.ToString("R", CultureInfo.InvariantCulture));
                return;
            }
            if (v is int || v is long || v is short || v is byte || v is uint || v is ulong || v is ushort || v is sbyte)
            {
                sb.Append(Convert.ToString(v, CultureInfo.InvariantCulture));
                return;
            }
            if (v is Enum) { WriteString(sb, v.ToString()); return; }
            IDictionary dict = v as IDictionary;
            if (dict != null)
            {
                sb.Append('{');
                bool first = true;
                foreach (DictionaryEntry e in dict)
                {
                    if (!first) sb.Append(',');
                    first = false;
                    WriteString(sb, Convert.ToString(e.Key, CultureInfo.InvariantCulture));
                    sb.Append(':');
                    WriteValue(sb, e.Value, depth + 1);
                }
                sb.Append('}');
                return;
            }
            IEnumerable list = v as IEnumerable;
            if (list != null)
            {
                sb.Append('[');
                bool first = true;
                foreach (object item in list)
                {
                    if (!first) sb.Append(',');
                    first = false;
                    WriteValue(sb, item, depth + 1);
                }
                sb.Append(']');
                return;
            }
            WriteString(sb, v.ToString());
        }

        static void WriteString(StringBuilder sb, string s)
        {
            sb.Append('"');
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                        else sb.Append(c);
                        break;
                }
            }
            sb.Append('"');
        }

        // ------------------------------------------------------------ reading
        public static object Parse(string text)
        {
            int pos = 0;
            object v = ParseValue(text, ref pos);
            SkipWs(text, ref pos);
            if (pos != text.Length) throw new FormatException("trailing characters in JSON at " + pos);
            return v;
        }

        static void SkipWs(string s, ref int p)
        {
            while (p < s.Length && char.IsWhiteSpace(s[p])) p++;
        }

        static object ParseValue(string s, ref int p)
        {
            SkipWs(s, ref p);
            if (p >= s.Length) throw new FormatException("unexpected end of JSON");
            char c = s[p];
            if (c == '{') return ParseObject(s, ref p);
            if (c == '[') return ParseArray(s, ref p);
            if (c == '"') return ParseString(s, ref p);
            if (Lit(s, ref p, "true")) return true;
            if (Lit(s, ref p, "false")) return false;
            if (Lit(s, ref p, "null")) return null;
            return ParseNumber(s, ref p);
        }

        static bool Lit(string s, ref int p, string lit)
        {
            if (string.CompareOrdinal(s, p, lit, 0, lit.Length) == 0) { p += lit.Length; return true; }
            return false;
        }

        static Dictionary<string, object> ParseObject(string s, ref int p)
        {
            Dictionary<string, object> d = new Dictionary<string, object>();
            p++;
            SkipWs(s, ref p);
            if (p < s.Length && s[p] == '}') { p++; return d; }
            while (true)
            {
                SkipWs(s, ref p);
                string key = ParseString(s, ref p);
                SkipWs(s, ref p);
                if (p >= s.Length || s[p] != ':') throw new FormatException("expected ':' at " + p);
                p++;
                d[key] = ParseValue(s, ref p);
                SkipWs(s, ref p);
                if (p < s.Length && s[p] == ',') { p++; continue; }
                if (p < s.Length && s[p] == '}') { p++; return d; }
                throw new FormatException("expected ',' or '}' at " + p);
            }
        }

        static List<object> ParseArray(string s, ref int p)
        {
            List<object> l = new List<object>();
            p++;
            SkipWs(s, ref p);
            if (p < s.Length && s[p] == ']') { p++; return l; }
            while (true)
            {
                l.Add(ParseValue(s, ref p));
                SkipWs(s, ref p);
                if (p < s.Length && s[p] == ',') { p++; continue; }
                if (p < s.Length && s[p] == ']') { p++; return l; }
                throw new FormatException("expected ',' or ']' at " + p);
            }
        }

        static string ParseString(string s, ref int p)
        {
            if (s[p] != '"') throw new FormatException("expected string at " + p);
            p++;
            StringBuilder sb = new StringBuilder();
            while (p < s.Length)
            {
                char c = s[p++];
                if (c == '"') return sb.ToString();
                if (c != '\\') { sb.Append(c); continue; }
                char e = s[p++];
                switch (e)
                {
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'u':
                        sb.Append((char)int.Parse(s.Substring(p, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture));
                        p += 4;
                        break;
                    default: sb.Append(e); break;
                }
            }
            throw new FormatException("unterminated string");
        }

        static object ParseNumber(string s, ref int p)
        {
            int start = p;
            while (p < s.Length && "+-0123456789.eE".IndexOf(s[p]) >= 0) p++;
            string num = s.Substring(start, p - start);
            if (num.Length == 0) throw new FormatException("unexpected character '" + s[start] + "' at " + start);
            if (num.IndexOf('.') < 0 && num.IndexOf('e') < 0 && num.IndexOf('E') < 0)
            {
                long l;
                if (long.TryParse(num, NumberStyles.Integer, CultureInfo.InvariantCulture, out l)) return l;
            }
            return double.Parse(num, NumberStyles.Float, CultureInfo.InvariantCulture);
        }
    }
}
