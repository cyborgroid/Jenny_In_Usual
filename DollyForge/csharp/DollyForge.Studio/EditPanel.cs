using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;

namespace DollyForge.Studio;

/// <summary>
/// 편집 작업실 — python/tools/gemini_studio.py 세션을 Studio 에서 다룬다.
/// Gemini(나노 바나나) 또는 로컬 FLUX 로 자연어 편집, 드래그한 영역만 편집, AI 없는 직접 조정,
/// 되돌리기·다시 하기·단계 이동. 모든 단계는 세션 폴더에 남는다.
/// </summary>
internal sealed class EditPanel : Grid
{
    private const int ViewW = 420, ViewH = 630;

    private readonly Func<(string Json, string Dir, string Id)?> _design;
    private readonly Image _image = new() { Width = ViewW, Height = ViewH, Stretch = Stretch.Uniform };
    private readonly Canvas _overlay = new() { Width = ViewW, Height = ViewH, Background = Brushes.Transparent };
    private readonly Rectangle _selection = new() { Stroke = Brushes.Gold, StrokeThickness = 2, StrokeDashArray = new Avalonia.Collections.AvaloniaList<double> { 4, 3 }, IsVisible = false };
    private readonly ComboBox _backend = new() { ItemsSource = new[] { "Gemini (나노 바나나)", "로컬 FLUX" }, SelectedIndex = 0, MinWidth = 180 };
    private readonly TextBox _model = new() { Watermark = "모델 (비우면 기본: gemini-nano-banana-2.1)", FontSize = 12 };
    private readonly CheckBox _refs = new() { Content = "3D 레퍼런스 이미지 첨부", IsChecked = true };
    private readonly TextBox _instruction = new() { Watermark = "편집 지시 (예: 코트 단추를 은색으로, 배경에 떠다니는 주문서 추가)", AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, Height = 72 };
    private readonly ListBox _history = new() { Height = 150, FontSize = 12 };
    private readonly TextBlock _status = new() { TextWrapping = TextWrapping.Wrap, FontSize = 12, Foreground = new SolidColorBrush(Color.Parse("#76688A")) };
    private readonly Slider _bright = Adjust(0.7, 1.3), _contrast = Adjust(0.7, 1.3), _sat = Adjust(0.5, 1.5), _warm = new() { Minimum = -0.15, Maximum = 0.15, Value = 0 };

    private string? _session;
    private int[] _size = { ViewW, ViewH };
    private Point? _dragStart;
    private int[]? _region;
    private bool _busy, _loadingHistory;

    public EditPanel(Func<(string Json, string Dir, string Id)?> design)
    {
        _design = design;
        ColumnDefinitions = new ColumnDefinitions("Auto,*");
        ColumnSpacing = 14;

        var stage = new Grid();
        stage.Children.Add(new Border { Background = new SolidColorBrush(Color.Parse("#2A1F30")), CornerRadius = new CornerRadius(10), Child = _image });
        _overlay.Children.Add(_selection);
        stage.Children.Add(_overlay);
        _overlay.PointerPressed += (_, e) => { _dragStart = e.GetPosition(_overlay); _selection.IsVisible = false; _region = null; };
        _overlay.PointerMoved += (_, e) => { if (_dragStart is { } s) DrawSelection(s, e.GetPosition(_overlay)); };
        _overlay.PointerReleased += (_, e) => { if (_dragStart is { } s) { DrawSelection(s, e.GetPosition(_overlay)); FinishSelection(); } _dragStart = null; };
        var left = new StackPanel { Spacing = 6 };
        left.Children.Add(stage);
        left.Children.Add(new TextBlock { Text = "이미지 위를 드래그하면 그 영역만 편집합니다", FontSize = 12, Foreground = _status.Foreground, HorizontalAlignment = HorizontalAlignment.Center });
        SetColumn(left, 0);
        Children.Add(left);

        var right = new StackPanel { Spacing = 8 };
        var start = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        start.Children.Add(Btn("현재 디자인으로 시작", NewFromDesign));
        start.Children.Add(Btn("이미지로 시작…", NewFromImage));
        start.Children.Add(Btn("세션 열기…", OpenSession));
        right.Children.Add(start);
        var engine = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        engine.Children.Add(_backend);
        engine.Children.Add(_refs);
        right.Children.Add(engine);
        right.Children.Add(_model);
        right.Children.Add(_instruction);
        var apply = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        apply.Children.Add(Btn("지시 적용", ApplyInstruction));
        apply.Children.Add(Btn("영역 해제", () => { _region = null; _selection.IsVisible = false; Status("영역 해제 — 전체 편집"); return Task.CompletedTask; }));
        apply.Children.Add(Btn("되돌리기", () => Run("undo")));
        apply.Children.Add(Btn("다시 하기", () => Run("redo")));
        right.Children.Add(apply);

        right.Children.Add(new TextBlock { Text = "직접 조정 (AI 없이)", FontWeight = FontWeight.SemiBold });
        right.Children.Add(Row("밝기", _bright));
        right.Children.Add(Row("대비", _contrast));
        right.Children.Add(Row("채도", _sat));
        right.Children.Add(Row("색온도", _warm));
        right.Children.Add(Btn("조정 적용", ApplyAdjust));

        right.Children.Add(new TextBlock { Text = "편집 기록 (눌러서 그 단계로)", FontWeight = FontWeight.SemiBold });
        right.Children.Add(_history);
        _history.SelectionChanged += async (_, _) =>
        {
            if (_loadingHistory || _history.SelectedItem is not ListBoxItem { Tag: int step }) return;
            await Run("goto", step.ToString());
        };
        var tail = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        tail.Children.Add(Btn("내보내기…", Export));
        tail.Children.Add(Btn("호환성 점검", Doctor));
        right.Children.Add(tail);
        right.Children.Add(_status);
        SetColumn(right, 1);
        Children.Add(new ScrollViewer { Content = right, [Grid.ColumnProperty] = 1 });
        Status("세션을 시작하세요. Gemini 는 API 키가 필요합니다 (Gemini CLI 와 같은 GEMINI_API_KEY / ~/.gemini/.env).");
    }

    private static Slider Adjust(double min, double max) => new() { Minimum = min, Maximum = max, Value = 1 };

    private static Control Row(string label, Slider s)
    {
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("60,*") };
        g.Children.Add(new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center });
        SetColumn(s, 1);
        g.Children.Add(s);
        return g;
    }

    private Button Btn(string text, Func<Task> act)
    {
        var b = new Button { Content = text };
        b.Click += async (_, _) =>
        {
            if (_busy) return;
            _busy = true;
            try { await act(); }
            catch (Exception e) { Status("⚠ " + e.Message); }
            finally { _busy = false; }
        };
        return b;
    }

    private void Status(string s) => _status.Text = s;

    // ---- 영역 선택 (화면 좌표 → 이미지 픽셀) ----

    private void DrawSelection(Point a, Point b)
    {
        double x = Math.Min(a.X, b.X), y = Math.Min(a.Y, b.Y);
        Canvas.SetLeft(_selection, x);
        Canvas.SetTop(_selection, y);
        _selection.Width = Math.Abs(a.X - b.X);
        _selection.Height = Math.Abs(a.Y - b.Y);
        _selection.IsVisible = true;
    }

    private void FinishSelection()
    {
        if (_selection.Width < 12 || _selection.Height < 12) { _selection.IsVisible = false; _region = null; return; }
        /* Stretch=Uniform 이므로 실제 그림이 놓인 사각형을 계산한다 */
        double scale = Math.Min(ViewW / (double)_size[0], ViewH / (double)_size[1]);
        double ox = (ViewW - _size[0] * scale) / 2, oy = (ViewH - _size[1] * scale) / 2;
        int px = (int)Math.Max(0, (Canvas.GetLeft(_selection) - ox) / scale), py = (int)Math.Max(0, (Canvas.GetTop(_selection) - oy) / scale);
        _region = new[] { px, py, (int)(_selection.Width / scale), (int)(_selection.Height / scale) };
        Status($"영역 {_region[0]},{_region[1]} {_region[2]}×{_region[3]} — 이 영역만 편집합니다");
    }

    // ---- gemini_studio.py 호출 ----

    private static string Python()
    {
        var venv = System.IO.Path.Combine(Workspace.Home, "python", ".venv", "bin", "python");
        return File.Exists(venv) ? venv : "python3";
    }

    private async Task<JsonNode?> Tool(params string[] args)
    {
        var psi = new ProcessStartInfo(Python()) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
        psi.ArgumentList.Add(System.IO.Path.Combine(Workspace.Home, "python", "tools", "gemini_studio.py"));
        psi.ArgumentList.Add("--json");
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        var outTask = p.StandardOutput.ReadToEndAsync();
        var err = await p.StandardError.ReadToEndAsync();
        await p.WaitForExitAsync();
        var output = (await outTask).Trim();
        if (p.ExitCode != 0 && args[0] != "doctor")
            throw new InvalidOperationException((err.Trim().Split('\n').LastOrDefault() ?? "") + " " + output);
        var last = output.Split('\n').LastOrDefault(l => l.StartsWith('{'));
        return last == null ? null : JsonNode.Parse(last);
    }

    private void Show(JsonNode? state)
    {
        if (state == null) return;
        _session = state["folder"]?.GetValue<string>() ?? _session;
        if (state["size"] is JsonArray sz) _size = new[] { sz[0]!.GetValue<int>(), sz[1]!.GetValue<int>() };
        var img = state["image"]?.GetValue<string>();
        if (img != null)
        {
            using var fs = File.OpenRead(img);
            _image.Source = new Bitmap(fs);
        }
        _loadingHistory = true;
        var head = state["head"]?.GetValue<int>() ?? 0;
        var items = new List<ListBoxItem>();
        foreach (var s in state["steps"]?.AsArray() ?? new JsonArray())
        {
            var id = s!["id"]!.GetValue<int>();
            var op = s["op"]?.GetValue<string>();
            var what = s["instruction"]?.GetValue<string>() ?? s["source"]?.GetValue<string>() ?? "";
            var backend = s["backend"]?.GetValue<string>();
            items.Add(new ListBoxItem { Tag = id, Content = $"{(id == head ? "▶" : "  ")} {id}  {op}{(backend != null ? " · " + backend : "")}  {what}" });
        }
        _history.ItemsSource = items;
        _history.SelectedIndex = -1;
        _loadingHistory = false;
        _selection.IsVisible = false;
        _region = null;
    }

    private async Task Run(params string[] args)
    {
        if (_session == null) throw new InvalidOperationException("먼저 세션을 시작하세요.");
        var all = new List<string> { args[0], _session };
        all.AddRange(args.Skip(1));
        Show(await Tool(all.ToArray()));
        Status($"{args[0]} 완료");
    }

    private string NewSessionName(string id) => System.IO.Path.Combine(Workspace.Home, "build", "sessions", $"{id}-{DateTime.Now:yyyyMMdd-HHmmss}");

    private async Task NewFromDesign()
    {
        var d = _design() ?? throw new InvalidOperationException("레시피를 먼저 고르세요.");
        var tmp = System.IO.Path.Combine(d.Dir, ".studio_edit.dolly.json");
        File.WriteAllText(tmp, d.Json);
        try
        {
            Status("엔진 렌더로 세션을 만드는 중…");
            var args = new List<string> { "new", NewSessionName(d.Id), "--recipe", tmp };
            if (_refs.IsChecked != true) args.Add("--no-ref");
            Show(await Tool(args.ToArray()));
            Status("세션 시작 — 지시를 적고 '지시 적용', 또는 영역을 드래그하세요.");
        }
        finally { File.Delete(tmp); }
    }

    private async Task NewFromImage()
    {
        var top = TopLevel.GetTopLevel(this)!;
        var files = await top.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "편집할 이미지 (Gemini 에서 만든 이미지도 됩니다)",
            FileTypeFilter = new[] { new FilePickerFileType("이미지") { Patterns = new[] { "*.png", "*.jpg", "*.jpeg", "*.webp" } } },
        });
        var path = files.FirstOrDefault()?.TryGetLocalPath();
        if (path == null) return;
        Show(await Tool("new", NewSessionName(System.IO.Path.GetFileNameWithoutExtension(path)), "--image", path));
        Status("이미지로 세션 시작");
    }

    private async Task OpenSession()
    {
        var top = TopLevel.GetTopLevel(this)!;
        var dirs = await top.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "세션 폴더 (session.json 이 있는 곳)" });
        var path = dirs.FirstOrDefault()?.TryGetLocalPath();
        if (path == null) return;
        Show(await Tool("log", path));
    }

    private async Task ApplyInstruction()
    {
        var text = (_instruction.Text ?? "").Trim();
        if (text.Length == 0) throw new InvalidOperationException("편집 지시를 적어 주세요.");
        var args = new List<string> { text, "--backend", _backend.SelectedIndex == 0 ? "gemini" : "flux" };
        if (_region != null) { args.Add("--region"); args.Add(string.Join(",", _region)); }
        if (!string.IsNullOrWhiteSpace(_model.Text)) { args.Add("--model"); args.Add(_model.Text.Trim()); }
        if (_refs.IsChecked != true) args.Add("--no-ref");
        Status(_backend.SelectedIndex == 0 ? "Gemini 로 편집 중…" : "로컬 FLUX 로 편집 중… (처음에는 모델을 올리느라 오래 걸립니다)");
        args.Insert(0, "edit");
        await Run(args.ToArray());
    }

    private Task ApplyAdjust() => Run("adjust",
        "--brightness", _bright.Value.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture),
        "--contrast", _contrast.Value.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture),
        "--saturation", _sat.Value.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture),
        "--warmth", _warm.Value.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture));

    private async Task Export()
    {
        if (_session == null) throw new InvalidOperationException("먼저 세션을 시작하세요.");
        var top = TopLevel.GetTopLevel(this)!;
        var file = await top.StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            SuggestedFileName = System.IO.Path.GetFileName(_session) + ".png",
            FileTypeChoices = new[] { new FilePickerFileType("PNG") { Patterns = new[] { "*.png" } } },
        });
        var path = file?.TryGetLocalPath();
        if (path == null) return;
        await Tool("export", _session, "-o", path);
        Status("내보냄: " + path);
    }

    private async Task Doctor()
    {
        Status("호환성 점검 중…");
        var rep = await Tool("doctor");
        var lines = (rep?["checks"]?.AsArray() ?? new JsonArray())
            .Select(c => $"{(c!["ok"]!.GetValue<bool>() ? "✓" : "✗")} {c["name"]}: {c["detail"]}");
        Status(string.Join("\n", lines));
    }

    /// <summary>스크린샷용: 이미 있는 세션을 연다</summary>
    public async Task OpenForScreenshot(string folder) => Show(await Tool("log", folder));
}
