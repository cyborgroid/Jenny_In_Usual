using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace DollyForge.Studio;

/// <summary>libdollyforge(C 코어) P/Invoke. Python·Rust·C++ 도구와 같은 엔진을 쓴다.</summary>
internal static partial class Native
{
    private const string Lib = "dollyforge";

    static Native()
    {
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, Resolve);
    }

    private static IntPtr Resolve(string name, Assembly asm, DllImportSearchPath? path)
    {
        if (name != Lib) return IntPtr.Zero;
        var candidates = new List<string>();
        var env = Environment.GetEnvironmentVariable("DOLLYFORGE_LIB");
        if (!string.IsNullOrEmpty(env)) candidates.Add(env);
        candidates.Add(Path.Combine(AppContext.BaseDirectory, "libdollyforge.so"));
        candidates.Add(Path.Combine(Workspace.Home, "build", "libdollyforge.so"));
        candidates.Add(Path.Combine(Workspace.Home, "build", "libdollyforge.dylib"));
        candidates.Add(Path.Combine(Workspace.Home, "build", "Release", "dollyforge.dll"));
        foreach (var c in candidates)
            if (File.Exists(c) && NativeLibrary.TryLoad(c, out var h)) return h;
        throw new DllNotFoundException("libdollyforge not found. Build the C core first: cmake -S DollyForge -B DollyForge/build && cmake --build DollyForge/build");
    }

    [DllImport(Lib)] public static extern IntPtr df_version();
    [DllImport(Lib)] public static extern IntPtr df_forge_json(byte[] json, nuint len, byte[] baseDir, byte[] err, nuint errlen);
    [DllImport(Lib)] public static extern IntPtr df_forge_file(byte[] path, byte[] err, nuint errlen);
    [DllImport(Lib)] public static extern void df_skin_free(IntPtr skin);
    [DllImport(Lib)] public static extern int df_skin_model(IntPtr skin);
    [DllImport(Lib)] public static extern IntPtr df_skin_rgba_const(IntPtr skin);
    [DllImport(Lib)] public static extern int df_skin_save_png(IntPtr skin, byte[] path, byte[] err, nuint errlen);
    [DllImport(Lib)] public static extern int df_validate(IntPtr skin, byte[] buf, nuint buflen);
    [DllImport(Lib)] public static extern void df_render_flat(IntPtr skin, int view, byte[] out16x32);

    public static byte[] Z(string s) => Encoding.UTF8.GetBytes(s + "\0");
    public static string FromZ(byte[] buf) => Encoding.UTF8.GetString(buf, 0, Math.Max(0, Array.IndexOf(buf, (byte)0)));
}

/// <summary>C 코어가 소유한 스킨 핸들</summary>
internal sealed class Skin : IDisposable
{
    private IntPtr _h;
    private Skin(IntPtr h) => _h = h;

    public static Skin ForgeJson(string json, string baseDir)
    {
        var data = Encoding.UTF8.GetBytes(json);
        var err = new byte[512];
        var h = Native.df_forge_json(data, (nuint)data.Length, Native.Z(baseDir), err, (nuint)err.Length);
        if (h == IntPtr.Zero) throw new InvalidOperationException(Native.FromZ(err));
        return new Skin(h);
    }

    public bool Slim => Native.df_skin_model(_h) == 1;

    public byte[] Rgba()
    {
        var px = new byte[64 * 64 * 4];
        Marshal.Copy(Native.df_skin_rgba_const(_h), px, 0, px.Length);
        return px;
    }

    public byte[] Flat(int view)
    {
        var px = new byte[16 * 32 * 4];
        Native.df_render_flat(_h, view, px);
        return px;
    }

    public (int Errors, string Report) Validate()
    {
        var buf = new byte[8192];
        var n = Native.df_validate(_h, buf, (nuint)buf.Length);
        return (n, Native.FromZ(buf));
    }

    public void Save(string path)
    {
        var err = new byte[256];
        if (Native.df_skin_save_png(_h, Native.Z(path), err, (nuint)err.Length) == 0)
            throw new IOException(Native.FromZ(err));
    }

    public void Dispose()
    {
        if (_h != IntPtr.Zero) Native.df_skin_free(_h);
        _h = IntPtr.Zero;
    }
}
