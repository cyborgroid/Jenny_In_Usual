using System.Text.Json;
using System.Text.Json.Nodes;

namespace DollyForge.Studio;

/// <summary>DollyForge 폴더(characters/, build/)를 찾고 레시피 체인을 읽는다.</summary>
internal static class Workspace
{
    public static readonly string Home = FindHome();

    private static string FindHome()
    {
        var env = Environment.GetEnvironmentVariable("DOLLYFORGE_HOME");
        if (!string.IsNullOrEmpty(env)) return env;
        foreach (var start in new[] { AppContext.BaseDirectory, Directory.GetCurrentDirectory() })
        {
            for (var d = new DirectoryInfo(start); d != null; d = d.Parent)
            {
                if (Directory.Exists(Path.Combine(d.FullName, "characters")) && Directory.Exists(Path.Combine(d.FullName, "core")))
                    return d.FullName;
                var nested = Path.Combine(d.FullName, "DollyForge");
                if (Directory.Exists(Path.Combine(nested, "characters"))) return nested;
            }
        }
        return Directory.GetCurrentDirectory();
    }

    public static readonly JsonDocumentOptions JsonOptions = new()
    {
        CommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    public static JsonObject Load(string path) =>
        JsonNode.Parse(File.ReadAllText(path), documentOptions: JsonOptions)?.AsObject()
        ?? throw new InvalidDataException(path);

    /// <summary>추상(abstract) 베이스를 뺀 모든 레시피</summary>
    public static List<string> Recipes()
    {
        var dir = Path.Combine(Home, "characters");
        if (!Directory.Exists(dir)) return new();
        return Directory.GetFiles(dir, "*.dolly.json", SearchOption.AllDirectories)
            .Where(p => Load(p)["meta"]?["abstract"]?.GetValue<bool>() != true)
            .OrderBy(p => p, StringComparer.Ordinal)
            .ToList();
    }

    /// <summary>
    /// extends 그래프를 C 코어와 같은 규칙으로 펼친 순서: 부모들(적힌 순서, 공통 조상은 한 번) → 자신.
    /// </summary>
    public static List<JsonObject> Chain(string recipePath)
    {
        var order = new List<JsonObject>();
        var seen = new HashSet<string>();
        void Visit(string path, int depth)
        {
            if (depth > 16) throw new InvalidDataException("extends nested too deep: " + path);
            var doc = Load(path);
            var parents = doc["extends"] switch
            {
                JsonArray arr => arr.Select(n => n!.GetValue<string>()).ToList(),
                JsonValue v => new List<string> { v.GetValue<string>() },
                _ => new List<string>(),
            };
            foreach (var rel in parents)
            {
                var full = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(path)!, rel));
                if (seen.Add(full)) Visit(full, depth + 1);
            }
            order.Add(doc);
        }
        var root = Path.GetFullPath(recipePath);
        seen.Add(root);
        Visit(root, 0);
        return order;
    }

    /// <summary>체인 순서대로 합친 팔레트 (뒤에 오는 쪽이 덮어씀, "//" 키는 주석)</summary>
    public static List<KeyValuePair<string, string>> MergedPalette(string recipePath)
    {
        var merged = new List<KeyValuePair<string, string>>();
        foreach (var doc in Chain(recipePath))
        {
            if (doc["palette"] is not JsonObject pal) continue;
            foreach (var (k, v) in pal)
            {
                if (k.StartsWith("//")) continue;
                var value = v?.GetValue<string>() ?? "";
                var i = merged.FindIndex(e => e.Key == k);
                if (i >= 0) merged[i] = new(k, value);
                else merged.Add(new(k, value));
            }
        }
        return merged;
    }

    public static string Title(string recipePath)
    {
        var meta = Load(recipePath)["meta"];
        return meta?["title"]?["ko_KR"]?.GetValue<string>()
            ?? meta?["title"]?["en_US"]?.GetValue<string>()
            ?? Path.GetFileName(recipePath);
    }

    /// <summary>원본 레시피를 extends 하고 바뀐 팔레트만 덮어쓰는 "메이크업 변형" 레시피</summary>
    public static JsonObject Variant(string recipePath, IReadOnlyDictionary<string, string> overrides, JsonObject? meta = null,
                                     IReadOnlyDictionary<string, double>? makeup = null)
    {
        var pal = new JsonObject();
        foreach (var (k, v) in overrides) pal[k] = v;
        var o = new JsonObject { ["dollyforge"] = 1 };
        if (meta != null) o["meta"] = meta;
        o["extends"] = Path.GetFileName(recipePath);
        o["palette"] = pal;
        if (makeup is { Count: > 0 })
        {
            var mk = new JsonObject();
            foreach (var (k, v) in makeup) mk[k] = Math.Round(v, 3);
            o["doll"] = new JsonObject { ["makeup"] = mk };
        }
        return o;
    }

    /// <summary>메이크업 곡선 값: 체인 뒤쪽(자식)부터 찾고 없으면 엔진 기본값 (C 코어와 같은 규칙)</summary>
    public static double MakeupValue(string recipePath, string key, double fallback)
    {
        var chain = Chain(recipePath);
        for (var i = chain.Count - 1; i >= 0; i--)
            if (chain[i]["doll"]?["makeup"]?[key] is JsonValue v && v.TryGetValue<double>(out var d)) return d;
        return fallback;
    }
}
