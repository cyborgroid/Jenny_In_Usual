using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Platform.Storage;

namespace DollyForge.Studio;

/// <summary>
/// 메이크업 스튜디오: 레시피를 고르고, 합쳐진 팔레트(피부·눈·머리·옷 색)를 실시간으로 바꿔 보고,
/// 결과를 스킨 PNG 또는 "메이크업 변형 레시피"(extends + 바뀐 팔레트)로 저장한다.
/// </summary>
internal sealed class MainWindow : Window
{
    private static readonly IBrush Ink = new SolidColorBrush(Color.Parse("#2E2238"));
    private static readonly IBrush Muted = new SolidColorBrush(Color.Parse("#76688A"));
    private static readonly IBrush Panel = new SolidColorBrush(Color.Parse("#FFFFFF"));
    private static readonly IBrush Page = new SolidColorBrush(Color.Parse("#F4EEF8"));
    private static readonly IBrush Accent = new SolidColorBrush(Color.Parse("#6E3B98"));
    private static readonly IBrush Stage = new SolidColorBrush(Color.Parse("#E6DDEE"));

    private readonly ComboBox _recipes = new() { MinWidth = 300 };
    private readonly StackPanel _palette = new() { Spacing = 4 };
    private readonly Image _front = PixelImage(16, 32, 10);
    private readonly Image _back = PixelImage(16, 32, 10);
    private readonly Image _texture = PixelImage(64, 64, 4);
    private readonly TextBlock _status = new() { TextWrapping = TextWrapping.Wrap, Foreground = Ink };
    private readonly TextBlock _report = new() { TextWrapping = TextWrapping.Wrap, FontFamily = new FontFamily("monospace"), FontSize = 12, Foreground = Muted };
    private readonly TextBox _variantId = new() { Watermark = "variant id (예: jenny_cyber_rose)" };
    private readonly TextBox _variantTitle = new() { Watermark = "표시 이름 (예: 제니 (로즈))" };

    private readonly Dictionary<string, string> _overrides = new();
    private List<KeyValuePair<string, string>> _baseColors = new();
    private List<string> _recipePaths = new();
    private string? _recipe;
    private bool _loading;

    public MainWindow(string? initialRecipe = null)
    {
        Title = "Dolly Forge Studio";
        Width = 1440;
        Height = 860;
        Background = Page;
        Content = BuildLayout();
        _recipes.SelectionChanged += (_, _) =>
        {
            if (_loading || _recipes.SelectedIndex < 0) return;
            Open(_recipePaths[_recipes.SelectedIndex]);
        };
        RefreshRecipes(initialRecipe);
    }

    // ---- 화면 구성 ----

    private Control BuildLayout()
    {
        var header = new DockPanel { Margin = new Thickness(20, 16, 20, 8) };
        var titles = new StackPanel { Spacing = 2 };
        titles.Children.Add(new TextBlock { Text = "Dolly Forge Studio", FontSize = 26, FontWeight = FontWeight.Bold, Foreground = Accent });
        titles.Children.Add(new TextBlock { Text = "마인크래프트 캐릭터 메이크업 & 개발 · libdollyforge " + Version(), Foreground = Muted });
        var pick = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        pick.Children.Add(new TextBlock { Text = "레시피", VerticalAlignment = VerticalAlignment.Center, Foreground = Ink });
        pick.Children.Add(_recipes);
        DockPanel.SetDock(pick, Dock.Right);
        header.Children.Add(pick);
        header.Children.Add(titles);

        var left = Card("메이크업 팔레트", new ScrollViewer { Content = _palette, VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto });

        var views = new WrapPanel { HorizontalAlignment = HorizontalAlignment.Center };
        views.Children.Add(Labeled("정면", _front));
        views.Children.Add(Labeled("후면", _back));
        views.Children.Add(Labeled("64×64 텍스처", _texture));
        var center = Card("미리보기", new ScrollViewer { Content = views });

        var actions = new StackPanel { Spacing = 8 };
        actions.Children.Add(_status);
        actions.Children.Add(Button("스킨 PNG 저장…", SaveSkinAsync));
        actions.Children.Add(Button("팔레트 되돌리기", () => { _overrides.Clear(); Open(_recipe!); return Task.CompletedTask; }));
        actions.Children.Add(new Separator { Margin = new Thickness(0, 6) });
        actions.Children.Add(new TextBlock { Text = "메이크업 변형으로 저장", FontWeight = FontWeight.SemiBold, Foreground = Ink });
        actions.Children.Add(new TextBlock { Text = "원본 레시피를 extends 하고 바꾼 색만 담은 새 레시피를 만든다. 옷 모양과 얼굴은 원본을 그대로 따른다.", TextWrapping = TextWrapping.Wrap, Foreground = Muted, FontSize = 12 });
        actions.Children.Add(_variantId);
        actions.Children.Add(_variantTitle);
        actions.Children.Add(Button("변형 레시피 저장", SaveVariantAsync));
        actions.Children.Add(new Separator { Margin = new Thickness(0, 6) });
        actions.Children.Add(new TextBlock { Text = "검증 (libdollyforge)", FontWeight = FontWeight.SemiBold, Foreground = Ink });
        actions.Children.Add(_report);
        var right = Card("작업", new ScrollViewer { Content = actions });

        var grid = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("372,*,320"),
            Margin = new Thickness(12, 0, 12, 12),
        };
        Grid.SetColumn(left, 0);
        Grid.SetColumn(center, 1);
        Grid.SetColumn(right, 2);
        grid.Children.Add(left);
        grid.Children.Add(center);
        grid.Children.Add(right);

        var root = new DockPanel();
        DockPanel.SetDock(header, Dock.Top);
        root.Children.Add(header);
        root.Children.Add(grid);
        return root;
    }

    private static Border Card(string title, Control body)
    {
        var dock = new DockPanel();
        var head = new TextBlock { Text = title, FontSize = 16, FontWeight = FontWeight.SemiBold, Foreground = Ink, Margin = new Thickness(0, 0, 0, 10) };
        DockPanel.SetDock(head, Dock.Top);
        dock.Children.Add(head);
        dock.Children.Add(body);
        return new Border { Background = Panel, CornerRadius = new CornerRadius(12), Padding = new Thickness(16), Margin = new Thickness(8), Child = dock };
    }

    private static Control Labeled(string label, Image img)
    {
        var s = new StackPanel { Margin = new Thickness(8), Spacing = 6 };
        s.Children.Add(new Border { Background = Stage, CornerRadius = new CornerRadius(8), Padding = new Thickness(10), Child = img });
        s.Children.Add(new TextBlock { Text = label, HorizontalAlignment = HorizontalAlignment.Center, Foreground = Muted });
        return s;
    }

    private Button Button(string text, Func<Task> onClick)
    {
        var b = new Button { Content = text, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Center };
        b.Click += async (_, _) =>
        {
            try { await onClick(); }
            catch (Exception e) { _status.Text = "⚠ " + e.Message; }
        };
        return b;
    }

    private static Image PixelImage(int w, int h, int scale)
    {
        var img = new Image { Width = w * scale, Height = h * scale, Stretch = Stretch.Fill };
        RenderOptions.SetBitmapInterpolationMode(img, Avalonia.Media.Imaging.BitmapInterpolationMode.None);
        return img;
    }

    private static string Version() => Marshal.PtrToStringUTF8(Native.df_version()) ?? "?";

    // ---- 레시피 / 팔레트 ----

    private void RefreshRecipes(string? select)
    {
        _loading = true;
        _recipePaths = Workspace.Recipes();
        _recipes.ItemsSource = _recipePaths.Select(p => $"{Workspace.Title(p)}  —  {Path.GetFileName(p)}").ToList();
        var idx = select == null ? 0 : _recipePaths.FindIndex(p => Path.GetFullPath(p) == Path.GetFullPath(select));
        _loading = false;
        if (_recipePaths.Count == 0) { _status.Text = "characters/ 에 레시피가 없습니다: " + Workspace.Home; return; }
        _recipes.SelectedIndex = Math.Max(0, idx);
        if (_recipe == null) Open(_recipePaths[Math.Max(0, idx)]);
    }

    public void Open(string recipePath)
    {
        if (_recipe != recipePath) _overrides.Clear();
        _recipe = recipePath;
        _baseColors = Workspace.MergedPalette(recipePath);
        _palette.Children.Clear();
        foreach (var (name, value) in _baseColors) _palette.Children.Add(PaletteRow(name, value));
        Reforge();
    }

    /// <summary>스크린샷/스크립트용: 색 하나를 바꾼다</summary>
    public void SetColor(string name, string hex)
    {
        _overrides[name] = hex;
        Open(_recipe!);
    }

    private Control PaletteRow(string name, string baseValue)
    {
        var current = _overrides.TryGetValue(name, out var o) ? o : baseValue;
        var isAlias = !current.StartsWith('#');
        var swatch = new ColorPicker { Width = 56, IsEnabled = !isAlias, Color = TryColor(current) ?? Colors.Transparent };
        var hex = new TextBox { Text = current, Width = 124, FontFamily = new FontFamily("monospace") };
        ToolTip.SetTip(hex, isAlias ? "파생색: 다른 색을 바꾸면 따라 바뀝니다. #RRGGBB 를 직접 넣으면 고정색이 됩니다." : null);
        var label = new TextBlock
        {
            Text = name + (_overrides.ContainsKey(name) ? "  •" : ""),
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = _overrides.ContainsKey(name) ? Accent : Ink,
            FontWeight = _overrides.ContainsKey(name) ? FontWeight.SemiBold : FontWeight.Normal,
        };

        var updating = false;
        void Apply(string value)
        {
            if (updating) return;
            updating = true;
            if (value == baseValue) _overrides.Remove(name); else _overrides[name] = value;
            label.Text = name + (_overrides.ContainsKey(name) ? "  •" : "");
            label.Foreground = _overrides.ContainsKey(name) ? Accent : Ink;
            Reforge();
            updating = false;
        }
        swatch.ColorChanged += (_, e) =>
        {
            var c = e.NewColor;
            var v = c.A == 255 ? $"#{c.R:X2}{c.G:X2}{c.B:X2}" : $"#{c.R:X2}{c.G:X2}{c.B:X2}{c.A:X2}";
            if (!updating) { hex.Text = v; Apply(v); }
        };
        hex.LostFocus += (_, _) => Commit();
        hex.KeyDown += (_, e) => { if (e.Key == Avalonia.Input.Key.Enter) Commit(); };
        void Commit()
        {
            var v = (hex.Text ?? "").Trim();
            if (v.Length == 0) return;
            if (TryColor(v) is { } c) { updating = true; swatch.Color = c; updating = false; }
            Apply(v);
        }

        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto"), ColumnSpacing = 8 };
        Grid.SetColumn(swatch, 0);
        Grid.SetColumn(label, 1);
        Grid.SetColumn(hex, 2);
        g.Children.Add(swatch);
        g.Children.Add(label);
        g.Children.Add(hex);
        return g;
    }

    private static Color? TryColor(string s) =>
        s.StartsWith('#') && Color.TryParse(s.Length == 9 ? "#" + s[7..9] + s[1..7] : s, out var c) ? c : null;

    private string VariantJson(JsonObject? meta = null) =>
        Workspace.Variant(_recipe!, _overrides, meta).ToJsonString(new JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping });

    private Skin? _skin;

    private void Reforge()
    {
        if (_recipe == null) return;
        try
        {
            var skin = Skin.ForgeJson(VariantJson(), Path.GetDirectoryName(_recipe)!);
            _skin?.Dispose();
            _skin = skin;
            _front.Source = Bitmap(skin.Flat(0), 16, 32);
            _back.Source = Bitmap(skin.Flat(1), 16, 32);
            _texture.Source = Bitmap(skin.Rgba(), 64, 64);
            var (errors, report) = skin.Validate();
            _report.Text = errors == 0 && report.Length == 0 ? "✓ 게임 규격 통과 (64×64, base 레이어 불투명)" : report;
            _status.Text = $"{Workspace.Title(_recipe)} · {(skin.Slim ? "slim (Alex 3px 팔)" : "classic (Steve 4px 팔)")} · 바꾼 색 {_overrides.Count}개";
        }
        catch (Exception e)
        {
            _report.Text = e.Message;
        }
    }

    private static WriteableBitmap Bitmap(byte[] rgba, int w, int h)
    {
        var bmp = new WriteableBitmap(new PixelSize(w, h), new Vector(96, 96), PixelFormats.Rgba8888, AlphaFormat.Unpremul);
        using var fb = bmp.Lock();
        for (var y = 0; y < h; y++)
            Marshal.Copy(rgba, y * w * 4, fb.Address + y * fb.RowBytes, w * 4);
        return bmp;
    }

    // ---- 저장 ----

    private async Task SaveSkinAsync()
    {
        if (_skin == null || _recipe == null) return;
        var id = Workspace.Load(_recipe)["meta"]?["id"]?.GetValue<string>() ?? "skin";
        string? path = null;
        if (StorageProvider.CanSave)
        {
            var file = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
            {
                SuggestedFileName = id + (_overrides.Count > 0 ? "_makeup" : "") + ".png",
                FileTypeChoices = new[] { new FilePickerFileType("PNG") { Patterns = new[] { "*.png" } } },
            });
            path = file?.TryGetLocalPath();
            if (file != null && path == null) { _status.Text = "로컬 파일로만 저장할 수 있습니다."; return; }
            if (file == null) return;
        }
        path ??= Path.Combine(Workspace.Home, "build", "studio", id + ".png");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        _skin.Save(path);
        _status.Text = "저장함: " + path;
    }

    private Task SaveVariantAsync()
    {
        if (_recipe == null) return Task.CompletedTask;
        var id = (_variantId.Text ?? "").Trim();
        if (!Regex.IsMatch(id, "^[a-z0-9_]{3,40}$"))
            throw new InvalidOperationException("variant id 는 영문 소문자·숫자·_ 3~40자 (Bedrock 스킨 id 로도 쓰임)");
        if (_overrides.Count == 0)
            throw new InvalidOperationException("바꾼 색이 없습니다. 팔레트에서 색을 먼저 바꿔 주세요.");
        var target = Path.Combine(Path.GetDirectoryName(_recipe)!, id + ".dolly.json");
        if (File.Exists(target)) throw new InvalidOperationException("이미 있는 레시피입니다: " + Path.GetFileName(target));

        var parentMeta = Workspace.Load(_recipe)["meta"] as JsonObject;
        var title = string.IsNullOrWhiteSpace(_variantTitle.Text) ? id : _variantTitle.Text!.Trim();
        var meta = new JsonObject
        {
            ["id"] = id,
            ["character"] = parentMeta?["character"]?.GetValue<string>() ?? "",
            ["title"] = new JsonObject { ["en_US"] = title, ["ko_KR"] = title },
            ["outfit"] = "메이크업 변형: " + string.Join(", ", _overrides.Keys),
            ["author"] = parentMeta?["author"]?.GetValue<string>() ?? "",
            ["license"] = parentMeta?["license"]?.GetValue<string>() ?? "CC-BY-NC-ND-4.0",
            ["rating"] = "all-ages",
        };
        // 저장하기 전에 C 코어로 한 번 더 확인
        using (var check = Skin.ForgeJson(VariantJson(meta), Path.GetDirectoryName(_recipe)!))
        {
            var (errors, report) = check.Validate();
            if (errors > 0) throw new InvalidOperationException("검증 실패:\n" + report);
        }
        File.WriteAllText(target, VariantJson(meta) + "\n");
        _overrides.Clear();
        _recipe = null;
        RefreshRecipes(target);
        _status.Text = "새 레시피: " + Path.GetFileName(target) + " (collection.json 에 추가하면 배포 팩에 들어갑니다)";
        return Task.CompletedTask;
    }
}
