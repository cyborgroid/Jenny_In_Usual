using Avalonia;
using Avalonia.Headless;
using Avalonia.Threading;

namespace DollyForge.Studio;

internal static class Program
{
    // dolly-forge-studio [recipe.dolly.json]
    // dolly-forge-studio --screenshot out.png [recipe.dolly.json] [--set name=#RRGGBB]...
    [STAThread]
    public static int Main(string[] args)
    {
        string? shot = null, recipe = null;
        var sets = new List<(string, string)>();
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == "--screenshot" && i + 1 < args.Length) shot = args[++i];
            else if (args[i] == "--set" && i + 1 < args.Length)
            {
                var kv = args[++i].Split('=', 2);
                if (kv.Length == 2) sets.Add((kv[0], kv[1]));
            }
            else recipe = Path.GetFullPath(args[i]);
        }
        if (shot != null) return Screenshot(shot, recipe, sets);
        return AppBuilder.Configure(() => new App { InitialRecipe = recipe })
            .UsePlatformDetect()
            .LogToTrace()
            .StartWithClassicDesktopLifetime(args);
    }

    /// <summary>화면 없이 실제 Skia 렌더로 창을 그려 PNG 로 저장 (CI·문서용)</summary>
    private static int Screenshot(string path, string? recipe, List<(string Name, string Hex)> sets)
    {
        AppBuilder.Configure<App>()
            .UseSkia()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
            .SetupWithoutStarting();
        MainWindow.SyncRender = true;
        var win = new MainWindow(recipe);
        foreach (var (name, value) in sets)
        {
            if (MainWindow.IsMakeupKey(name) && double.TryParse(value, System.Globalization.CultureInfo.InvariantCulture, out var v)) win.SetMakeup(name, v);
            else win.SetColor(name, value);
        }
        win.Show();
        for (var i = 0; i < 3; i++)
        {
            Dispatcher.UIThread.RunJobs();
            AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        }
        var frame = win.CaptureRenderedFrame();
        if (frame == null) { Console.Error.WriteLine("no frame rendered"); return 1; }
        frame.Save(path);
        Console.WriteLine("screenshot -> " + path);
        return 0;
    }
}
