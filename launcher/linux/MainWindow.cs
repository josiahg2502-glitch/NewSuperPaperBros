using System.Diagnostics;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace NewSuperPaperBros;

// The Linux launcher window: the Windows launcher (launcher\MainForm.cs) page for page, laid out like a game's title
// screen (after Dusklight's launcher): artwork filling the window, darkened from the left, the logo, a column of big
// menu words (Play, Game, Display, Controls, Mods, Quit), disc status bottom-left and version top-right. Game, Display,
// Controls and Mods open as a card over the dimmed art, with Back (or Escape) to return. Everything it changes goes to engine/UserData/Config.toml,
// read by the game at start. Keep the two in step when either changes.
sealed class MainWindow : Window
{
    public static readonly Color Cream = Color.Parse("#EEF0F6"), Paper = Colors.White, Ink = Color.Parse("#3B3B52"),
        Yellow = Color.Parse("#FFC83D"), Red = Color.Parse("#EC3A55"), Sky = Color.Parse("#6FC3FF"), Green = Color.Parse("#6FD36A"),
        Pink = Color.Parse("#FFD6EA"), Mint = Color.Parse("#D3F5EA");
    // Each page's colour: its card's border and heading.
    static readonly Color[] TileColors = { Color.Parse("#EC3A55"), Color.Parse("#3B8CFF"), Color.Parse("#2EC27E"), Color.Parse("#FF9A1F") };
    public static Color Mix(Color a, Color b, double f) =>
        Color.FromRgb((byte)(a.R + (b.R - a.R) * f), (byte)(a.G + (b.G - a.G) * f), (byte)(a.B + (b.B - a.B) * f));
    public static readonly IBrush InkBrush = new SolidColorBrush(Ink);
    static readonly FontFamily Font = new("fonts:Inter#Inter");
    // WinForms sizes are in points; these are the same sizes in pixels (10.5pt, 13pt, 18pt, 24pt).
    const double Body = 14, NavSize = 17.3, Heading = 24, PlaySize = 32;

    readonly bool selfTest;
    readonly string? screenshotDir;
    // play = the folder holding the game programs and their UserData: engine/ in a release.
    readonly string root, play, gameExe, userData, configPath, modsDir, discDir;
    static readonly string AppVersion = typeof(MainWindow).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "?";
    readonly GameConfig config;
    readonly Dictionary<string, Bitmap> sprites = new();
    readonly Panel content = new() { Margin = new Thickness(16, 12, 20, 16) };
    readonly List<(string name, Control page)> pages = new();
    readonly (string label, Action act)[] menu;
    TitleView view = null!;
    Grid pageLayer = null!;
    int openPage = -1;  // the page shown over the title screen, -1 = none
    TextBlock playStatus = null!, dumpHelp = null!, gameNote = null!;
    StackPanel modList = null!;
    PaperButton chooseDisc = null!;
    PaperButton? capturing;
    Func<bool>? cancelPadCapture;  // the Controls page's gamepad capture: Escape ends it
    string? captureKey;

    static readonly (string key, string label, string defaults)[] Actions =
    {
        ("up", "Up", "Up"), ("down", "Down", "Down"), ("left", "Left", "Left"), ("right", "Right", "Right"),
        ("jump", "Jump (2)", "X,Space"), ("run", "Pixl / action (1)", "Z,Left Shift"), ("a", "Flip (A)", "A"),
        ("shake", "Shake (stylish)", "C,Left Ctrl"), ("tilt_left", "Tilt left", "Q"), ("tilt_right", "Tilt right", "E"),
        ("plus", "Menu (+)", "Return,Escape"), ("minus", "Minus (-)", "-,Tab"),
    };

    public MainWindow(bool selfTest, string? screenshotDir = null)
    {
        this.selfTest = selfTest;
        this.screenshotDir = screenshotDir;
        (root, play, gameExe) = FindGame();
        userData = Path.Combine(play, "UserData");
        configPath = Path.Combine(userData, "Config.toml");
        modsDir = Path.Combine(userData, "mods");
        discDir = Path.Combine(root, "Disc", "DATA");
        config = new GameConfig(configPath);
        EnsureDefaults();
        Gamepads.Init(play);
        foreach (var name in new[] { "mario", "luigi", "merlon", "tippi", "sparkle" })
        {
            var file = Path.Combine(play, "startup", name + ".png");
            if (File.Exists(file)) sprites[name] = new Bitmap(file);
        }

        Title = "New Super Paper Bros.  v" + AppVersion;
        TippiCursor = MakeTippiCursor(clickable: false);
        TippiHand = MakeTippiCursor(clickable: true);
        Cursor = TippiCursor;  // inherited by every control without its own
        using (var icon = typeof(MainWindow).Assembly.GetManifestResourceStream("pixl.ico")) if (icon != null) Icon = new WindowIcon(icon);
        // 16:9, the shape of the title artwork, so all of it shows.
        Width = 1200;
        if (Screens.Primary is { } screen)
        {
            var area = screen.WorkingArea.Size.ToSize(screen.Scaling);
            Width = Math.Min(1200, Math.Min(area.Width - 40, (area.Height - 60) * 16 / 9));
        }
        Height = Width * 9 / 16;
        MinWidth = Math.Min(960, Width);
        MinHeight = Math.Min(580, Height);
        Background = new SolidColorBrush(Cream);
        FontFamily = Font;
        FontSize = Body;
        Foreground = InkBrush;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;

        // One full-window title screen; the pages open as cards on top of it, with Back (or Escape) to return.
        view = new TitleView(this);
        var back = Btn("◀  Back", ClosePage, Paper);
        back.Margin = new Thickness(24, 16, 0, 0);
        pageLayer = new Grid { RowDefinitions = new RowDefinitions("Auto,*"), IsVisible = false };
        pageLayer.Children.Add(back);
        content.Margin = new Thickness(20, 4, 20, 16);
        Grid.SetRow(content, 1);
        pageLayer.Children.Add(content);
        var grid = new Grid();
        grid.Children.Add(view);
        grid.Children.Add(pageLayer);
        Content = grid;

        var frame = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
        frame.Tick += (_, _) => { if (WindowState != WindowState.Minimized && openPage < 0) view.InvalidateVisual(); };
        frame.Start();

        menu = new (string, Action)[]
        {
            ("Play", Launch), ("Game", () => OpenPage(0)), ("Display", () => OpenPage(1)), ("Controls", () => OpenPage(2)),
            ("Mods", () => OpenPage(3)), ("Quit", Close),
        };
        AddPage("Game", BuildPlayPage());
        AddPage("Display", BuildDisplayPage());
        AddPage("Controls", BuildControlsPage());
        AddPage("Mods", BuildModsPage());
        RefreshStatus();
        AddHandler(KeyDownEvent, OnKey, RoutingStrategies.Tunnel);
        Opened += async (_, _) =>
        {
            view.IntroAt = T;
            if (screenshotDir != null)
            {
                // Headless visual check: the title screen and every page drawn to <dir>/<name>.png.
                Directory.CreateDirectory(screenshotDir);
                view.IntroAt = -10;  // intro animation already done
                async Task Shot(string name)
                {
                    await Task.Delay(400);
                    using var bmp = new RenderTargetBitmap(new PixelSize((int)Bounds.Width, (int)Bounds.Height));
                    bmp.Render(this);
                    bmp.Save(Path.Combine(screenshotDir, name + ".png"));
                }
                view.Hover = 0;
                await Shot("Title");
                view.Hover = -1;
                for (int i = 0; i < pages.Count; i++) { OpenPage(i); await Shot(pages[i].name); }
                ClosePage();
                using (var cur = new RenderTargetBitmap(new PixelSize(64, 32)))
                {
                    using (var g = cur.CreateDrawingContext())
                    {
                        g.FillRectangle(new SolidColorBrush(Sky), new Rect(0, 0, 64, 32));
                        for (int i = 0; i < cursorBitmaps.Count; i++) g.DrawImage(cursorBitmaps[i], new Rect(i * 32, 0, 32, 32));
                    }
                    cur.Save(Path.Combine(screenshotDir, "cursor.png"));
                }
                Close();
            }
            else if (selfTest)
            {
                for (int i = 0; i < pages.Count; i++) OpenPage(i);
                ClosePage();
                int code = !File.Exists(gameExe) ? 6 : !ModInstall.SelfCheck() ? 7 : !Gamepads.Ready ? 4 : Gamepads.Connected().All(p => Gamepads.Opens(p.id)) ? 0 : 5;
                // posted: the lifetime cannot shut down while it is still starting
                Dispatcher.UIThread.Post(() => (Application.Current?.ApplicationLifetime as IClassicDesktopStyleApplicationLifetime)?.Shutdown(code));
            }
        };
    }

    // The release keeps the game in engine/ next to this launcher; search up from the launcher's own folder.
    // engine/ holds one game program per disc region: nspb-engine-usa (R8PE01) and nspb-engine-pal (R8PP01).
    static (string root, string dir, string exe) FindGame()
    {
        var start = new DirectoryInfo(AppContext.BaseDirectory);
        for (var d = start; d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "engine", "nspb-engine-usa")))
                return (d.FullName, Path.Combine(d.FullName, "engine"), Path.Combine(d.FullName, "engine", "nspb-engine-usa"));
        return (start.FullName, Path.Combine(start.FullName, "engine"), Path.Combine(start.FullName, "engine", "nspb-engine-usa"));
    }

    // The extracted disc's id (R8PE01 USA, R8PP01 Europe), "" when none is set up.
    string DiscGameId()
    {
        try
        {
            var header = new byte[8];
            using var f = File.OpenRead(Path.Combine(DiscStatus().path, "sys", "boot.bin"));
            f.ReadExactly(header, 0, 8);
            return System.Text.Encoding.ASCII.GetString(header, 0, 6);
        }
        catch (Exception) { return ""; }
    }

    // This disc's save folder (per disc region and per extracted disc, as the game keeps them).
    string SaveDataDir() => WiiSave.DataDir(userData, DiscStatus().path, DiscGameId() is { Length: > 0 } id ? id : "R8PE01");

    // The packaged defaults: borderless 1080p, cinematic 2.39:1, 120 fps interpolation, HD textures. Keys the
    // user already has are left alone.
    void EnsureDefaults()
    {
        bool changed = false;
        foreach (var (key, value) in new[] { ("display_mode", "\"borderless\""), ("resolution_multiplier", "2.25"), ("aspect_mode", "3"),
                     ("frame_interpolation_fps", "120"), ("texture_replacements", "true"), ("widescreen", "true"), ("vsync", "true") })
            if (config.Get("video", key) == null) { config.Set("video", key, value); changed = true; }
        // The HD textures are part of the port, not an option.
        if (config.Get("video", "texture_replacements") != "true") { config.Set("video", "texture_replacements", "true"); changed = true; }
        if (changed) config.Save();
    }

    void AddPage(string name, Control page)
    {
        var accent = TileColors[pages.Count % TileColors.Length];
        if (page is Border card)
        {
            card.BorderBrush = new SolidColorBrush(Mix(accent, Colors.White, 0.35));  // the card's border and heading take the page's colour
            if (card.Child is ScrollViewer { Content: StackPanel body } && body.Children.Count > 0 && body.Children[0] is TextBlock heading)
                heading.Foreground = new SolidColorBrush(Mix(accent, Ink, 0.25));
        }
        page.IsVisible = false;
        content.Children.Add(page);
        pages.Add((name, page));
    }

    // A page opens as a card over the dimmed art, with Back (or Escape) to return to the title screen.
    void OpenPage(int index)
    {
        openPage = index;
        for (int i = 0; i < pages.Count; i++) pages[i].page.IsVisible = i == index;
        pageLayer.IsVisible = true;
        RefreshStatus();
        view.InvalidateVisual();
    }

    void ClosePage()
    {
        if (openPage < 0) return;
        cancelPadCapture?.Invoke();
        pageLayer.IsVisible = false;
        openPage = -1;
        RefreshStatus();
        view.InvalidateVisual();
    }

    // Everything moves on one clock.
    readonly Stopwatch clock = Stopwatch.StartNew();
    double T => clock.Elapsed.TotalSeconds;

    static double Lerp(double a, double b, float f) => a + (b - a) * f;

    // The pointer (Tippi) and the one over anything clickable (Tippi with a sparkle). Static so PaperButton can use it.
    public static Cursor TippiCursor = Cursor.Default, TippiHand = new(StandardCursorType.Hand);
    readonly List<Bitmap> cursorBitmaps = new();  // the pointer images, for the screenshot check

    // Tippi as the mouse pointer, clicking with her top-left wing tip. Over clickable things she is a little smaller
    // with a sparkle at her side, so it still shows what can be clicked.
    Cursor MakeTippiCursor(bool clickable)
    {
        if (!sprites.TryGetValue("tippi", out var img)) return clickable ? new Cursor(StandardCursorType.Hand) : Cursor.Default;
        const int size = 32;
        var bmp = new RenderTargetBitmap(new PixelSize(size, size));
        using (var g = bmp.CreateDrawingContext())
        {
            var b = Opaque(img);  // her drawn area, without the sprite's empty margin
            double fit = (clickable ? 0.78 : 1) * size / Math.Max(b.Width, b.Height);  // whole Tippi inside the square
            using (g.PushRenderOptions(new RenderOptions { BitmapInterpolationMode = BitmapInterpolationMode.HighQuality }))
                g.DrawImage(img, b, new Rect(0, 0, b.Width * fit, b.Height * fit));
            if (clickable)
            {
                // A bold four-point sparkle in the bottom-right corner.
                double cx = size * 0.76, cy = size * 0.76, r = size * 0.23, n = r * 0.32;
                g.DrawGeometry(new SolidColorBrush(Yellow), new Pen(InkBrush, Math.Max(1.5, size / 20.0), lineJoin: PenLineJoin.Round),
                    Polygon(new Point(cx, cy - r), new Point(cx + n, cy - n), new Point(cx + r, cy), new Point(cx + n, cy + n),
                            new Point(cx, cy + r), new Point(cx - n, cy + n), new Point(cx - r, cy), new Point(cx - n, cy - n)));
            }
        }
        var px = Pixels(bmp, out int stride);
        int hx = 0, hy = 0;
        for (int y = 0, found = 0; y < size && found == 0; y++)
            for (int x = 0; x < size / 2; x++)
                if (px[y * stride + x * 4 + 3] > 128) { hx = x; hy = y; found = 1; break; }
        cursorBitmaps.Add(bmp);
        return new Cursor(bmp, new PixelPoint(hx, hy));
    }

    static StreamGeometry Polygon(params Point[] points)
    {
        var geometry = new StreamGeometry();
        using (var c = geometry.Open())
        {
            c.BeginFigure(points[0], true);
            foreach (var p in points.Skip(1)) c.LineTo(p);
            c.EndFigure(true);
        }
        return geometry;
    }

    static byte[] Pixels(Bitmap b, out int stride)
    {
        stride = b.PixelSize.Width * 4;
        var buffer = new byte[stride * b.PixelSize.Height];
        unsafe { fixed (byte* p = buffer) b.CopyPixels(new PixelRect(b.PixelSize), (IntPtr)p, buffer.Length, stride); }
        return buffer;
    }

    // Bounding box of a sprite's visible pixels.
    static Rect Opaque(Bitmap b)
    {
        var px = Pixels(b, out int stride);
        int w = b.PixelSize.Width, h = b.PixelSize.Height, l = w, t = h, r = -1, bo = -1;
        for (int y = 0; y < h; y += 2)
            for (int x = 0; x < w; x += 2)
                if (px[y * stride + x * 4 + 3] > 24) { l = Math.Min(l, x); t = Math.Min(t, y); r = Math.Max(r, x); bo = Math.Max(bo, y); }
        return r < 0 ? new Rect(0, 0, w, h) : new Rect(l, t, r + 1 - l, bo + 1 - t);
    }

    // ----- The title screen -----
    static readonly Color Accent = Color.Parse("#FEE685"), ReadyGreen = Color.Parse("#D8F999"), ErrorPink = Color.Parse("#FFC9C9"),
        Muted = Color.Parse("#C9C3BE"), Night = Color.Parse("#16122B");

    // Like Dusklight's launcher: artwork fills the window (startup/title-*.jpg: the Super Paper Mario wallpaper by
    // FawfulTheGreat64, see CREDITS.txt) and darkens toward the left, where the logo and a column of big menu words sit;
    // disc status in the bottom-left corner, version in the top-right (the art has its own logo bottom-right).
    sealed class TitleView : Control
    {
        readonly MainWindow w;
        readonly Bitmap? art;
        readonly List<Rect> rows = new();
        Rect discRect;
        public int Hover = -1;          // the menu item under the mouse or chosen with the arrow keys
        public double IntroAt = 1e9;    // when the window first showed: items fade and rise in one after another

        public TitleView(MainWindow window)
        {
            w = window;
            ClipToBounds = true;
            RenderOptions.SetBitmapInterpolationMode(this, BitmapInterpolationMode.HighQuality);
            var dir = Path.Combine(w.play, "startup");
            var files = Directory.Exists(dir) ? Directory.GetFiles(dir, "title-*.jpg") : Array.Empty<string>();
            try { if (files.Length > 0) art = new Bitmap(files[Random.Shared.Next(files.Length)]); } catch (Exception) { }
            PointerMoved += (_, e) =>
            {
                if (w.openPage >= 0) return;
                var p = e.GetPosition(this);
                Hover = rows.FindIndex(r => r.Contains(p));
                Cursor = Hover >= 0 || discRect.Contains(p) ? TippiHand : TippiCursor;
            };
            PointerExited += (_, _) => Hover = -1;
            // A click on a menu word picks it; on the disc status, opens Game.
            PointerPressed += (_, e) =>
            {
                if (w.openPage >= 0 || !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
                var p = e.GetPosition(this);
                int i = rows.FindIndex(r => r.Contains(p));
                if (i >= 0) w.menu[i].act();
                else if (discRect.Contains(p)) w.OpenPage(0);
            };
        }

        public void Move(int step)
        {
            Hover = Hover < 0 ? 0 : (Hover + step + w.menu.Length) % w.menu.Length;
            InvalidateVisual();
        }

        // How far item `i` is into its fade-in (0..1).
        double Intro(int i) => Math.Clamp((w.T - IntroAt - 0.1 * i) / 0.35, 0, 1);

        static IBrush Solid(Color c, double alpha) => new SolidColorBrush(Color.FromArgb((byte)(255 * Math.Clamp(alpha, 0, 1)), c.R, c.G, c.B));

        static FormattedText Text(string text, double size, FontWeight weight, IBrush brush, FontStyle style = FontStyle.Normal) =>
            new(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, new Typeface(Font, style, weight), size, brush);

        // Text with Dusklight's soft dark shadow behind it, so it reads over the art.
        static void GlowText(DrawingContext g, string text, double size, FontWeight weight, Point at, Color color, double alpha)
        {
            var ft = Text(text, size, weight, Solid(Colors.Black, alpha * 0.8));
            g.DrawText(ft, at + new Point(1, 2));
            ft.SetForegroundBrush(Solid(color, alpha));
            g.DrawText(ft, at);
        }

        static LinearGradientBrush Fade(Point from, Point to, double a0, double a1) => new()
        {
            StartPoint = new RelativePoint(from, RelativeUnit.Relative), EndPoint = new RelativePoint(to, RelativeUnit.Relative),
            GradientStops =
            {
                new GradientStop(Color.FromArgb((byte)(255 * a0), Night.R, Night.G, Night.B), 0),
                new GradientStop(Color.FromArgb((byte)(255 * a1), Night.R, Night.G, Night.B), 1),
            },
        };

        public override void Render(DrawingContext g)
        {
            var size = Bounds.Size;
            if (size.Width <= 0 || size.Height <= 0) return;
            var r = new Rect(size);
            // The art, scaled to cover the window and centred, fading in from black.
            g.FillRectangle(new SolidColorBrush(Night), r);
            if (art != null)
            {
                double scale = Math.Max(size.Width / art.Size.Width, size.Height / art.Size.Height);
                double aw = art.Size.Width * scale, ah = art.Size.Height * scale;
                g.DrawImage(art, new Rect(art.Size), new Rect((size.Width - aw) / 2, (size.Height - ah) / 2, aw, ah));
            }
            double fade = Math.Clamp(w.T - IntroAt, 0, 1);
            if (fade < 1) g.FillRectangle(Solid(Colors.Black, 1 - fade), r);
            if (w.openPage >= 0) { g.FillRectangle(Solid(Night, 150 / 255.0), r); return; }

            // Dusklight's darkening: deep night from the left edge to clear past the middle, a little along the bottom and top.
            double ac = Intro(w.menu.Length + 1);
            g.FillRectangle(Fade(new Point(0, 0), new Point(1, 0), 215 / 255.0, 0), new Rect(0, 0, size.Width * 0.55, size.Height));
            g.FillRectangle(Fade(new Point(0, 0), new Point(0, 1), 0, 170 / 255.0), new Rect(0, size.Height - 150, size.Width, 150));
            g.FillRectangle(Fade(new Point(0, 0), new Point(0, 1), 150 / 255.0 * ac, 0), new Rect(0, 0, size.Width, 70));

            double x = 64, y = Math.Max(28, size.Height / 2 - 300);
            // The eyebrow and the logo: bubble letters with a soft shadow, a white rim and a red-to-yellow face.
            double a0 = Intro(0), rise = 10 * (1 - a0);
            GlowText(g, "JosiahG presents", 18.7, FontWeight.Normal, new Point(x + 4, y + rise), Colors.White, a0);
            y += 34;
            double em = Math.Min(77, size.Width * 0.45 / 6.2 * 1.33);
            foreach (var line in new[] { "NEW SUPER", "PAPER BROS." })
            {
                var ft = Text(line, em, FontWeight.Black, Solid(Colors.Black, 0.55 * a0), FontStyle.Italic);
                var at = new Point(x, y + rise);
                for (int k = 0; k < 16; k++) g.DrawText(ft, at + new Point(4 + Math.Cos(k * Math.PI / 8) * 6, 6 + Math.Sin(k * Math.PI / 8) * 6));
                ft.SetForegroundBrush(Solid(Colors.White, a0));
                for (int k = 0; k < 16; k++) g.DrawText(ft, at + new Point(Math.Cos(k * Math.PI / 8) * 5, Math.Sin(k * Math.PI / 8) * 5));
                ft.SetForegroundBrush(new LinearGradientBrush
                {
                    StartPoint = new RelativePoint(0, 0.2, RelativeUnit.Relative), EndPoint = new RelativePoint(0, 0.9, RelativeUnit.Relative), Opacity = a0,
                    GradientStops = { new GradientStop(Red, 0), new GradientStop(Yellow, 1) },
                });
                g.DrawText(ft, at);
                y += em * 0.98;
            }
            y += 28;

            // The menu: big words that light up with a yellow fade on hover, like Dusklight's.
            rows.Clear();
            const double rowH = 46, rowW = 380;
            for (int i = 0; i < w.menu.Length; i++)
            {
                double a = Intro(i + 1);
                var row = new Rect(x - 16, y + 10 * (1 - a), rowW, rowH);
                rows.Add(row);
                bool on = i == Hover;
                string word = w.menu[i].label.ToUpperInvariant();
                var label = Text(word, 29.3, FontWeight.Bold, Solid(Night, 1));
                var at = new Point(row.X + 16, row.Y + (rowH - label.Height) / 2);
                if (on)
                {
                    g.DrawRectangle(new LinearGradientBrush
                    {
                        StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(1, 0, RelativeUnit.Relative),
                        GradientStops = { new GradientStop(Accent, 0), new GradientStop(Color.FromArgb(0, Accent.R, Accent.G, Accent.B), 1) },
                    }, null, row, 10, 10);
                    g.DrawText(label, at);
                }
                else GlowText(g, word, 29.3, FontWeight.Bold, at, Colors.White, a);
                y += rowH + 4;
            }

            // Bottom-left: is a disc set up (click it to open Game).
            var (ok, _, _) = w.DiscStatus();
            double cy = size.Height - 84;
            string id = ok ? w.DiscGameId() : "";
            string status = ok ? $"✔  Super Paper Mario ({(id == "R8PP01" ? "Europe" : "USA")}) is ready" : "✖  No game disc yet: choose Game to set it up";
            GlowText(g, status, 16, FontWeight.Bold, new Point(x, cy), ok ? ReadyGreen : ErrorPink, ac);
            var statusText = Text(status, 16, FontWeight.Bold, Brushes.White);
            discRect = new Rect(x, cy, statusText.Width, statusText.Height);
            GlowText(g, "Mods: " + w.ModNames(), Body, FontWeight.Normal, new Point(x, cy + 30), Muted, ac);

            // Top-right: the version.
            var ver = "v" + AppVersion + "   made by JosiahG";
            GlowText(g, ver, Body, FontWeight.Normal, new Point(size.Width - 28 - Text(ver, Body, FontWeight.Normal, Brushes.White).Width, 16), Muted, ac);
        }
    }

    // A white card holding one page's controls, top to bottom, rimmed in its tile's colour (set by AddPage); it
    // scrolls when they are taller than the window.
    static (Control card, StackPanel body) NewCard(string heading)
    {
        var body = new StackPanel();
        body.Children.Add(new TextBlock { Text = heading, FontSize = Heading, FontWeight = FontWeight.Bold, Margin = new Thickness(0, 0, 0, 10) });
        var card = new Border
        {
            Background = new SolidColorBrush(Paper), BorderBrush = new SolidColorBrush(Sky), BorderThickness = new Thickness(5), CornerRadius = new CornerRadius(22),
            BoxShadow = BoxShadows.Parse("4 6 0 0 #281E1E46"), Margin = new Thickness(2, 2, 8, 8),
            Child = new ScrollViewer { Content = body, Padding = new Thickness(22, 16, 22, 16) },
        };
        return (card, body);
    }

    static TextBlock Note(string text, double width = 700) => new()
    {
        Text = text, TextWrapping = TextWrapping.Wrap, MaxWidth = width, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 6, 0, 6),
    };

    static PaperButton Btn(string text, Action? onClick, Color? back = null)
    {
        var b = new PaperButton(text, back ?? Yellow, Body);
        if (onClick != null) b.Click += onClick;
        return b;
    }

    static StackPanel Row(params Control[] children)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal };
        foreach (var c in children) row.Children.Add(c);
        return row;
    }

    // ----- Play -----
    Control BuildPlayPage()
    {
        var (card, body) = NewCard("Game");
        playStatus = Note("");
        playStatus.FontWeight = FontWeight.Bold;
        body.Children.Add(playStatus);
        AddDiscSetup(body);
        // The Wii system language the game sees (runtime SCGetLanguage, Config.toml [game] language). The European
        // disc ships English, French, German, Spanish and Italian text; the USA disc is English only.
        body.Children.Add(Choice("Language (European disc)", "language", new[] { ("English", "\"en\""), ("Français", "\"fr\""), ("Deutsch", "\"de\""), ("Español", "\"es\""), ("Italiano", "\"it\"") }, "game"));
        body.Children.Add(gameNote = Note("The game starts borderless at 1080p. " +
                               "In the game, F10 opens the settings menu (resolution, aspect, controllers, audio) and F11 switches between fullscreen and a window."));
        body.Children.Add(Row(
            Btn("Open game folder", () => Open(play), Paper),
            Btn("Open saves folder", () => Open(Directory.CreateDirectory(SaveDataDir()).FullName), Paper),
            Btn("Import Wii / Dolphin save...", ImportSave, Paper)));
        return card;
    }

    static void Open(string folder)
    {
        try { Process.Start(new ProcessStartInfo("xdg-open", $"\"{folder}\"") { UseShellExecute = false }); } catch (Exception) { }
    }

    // A Wii save (data.bin from SD: Wii Menu > Data Management > Save Data > Copy) or a Dolphin save folder.
    async void ImportSave()
    {
        const string saveDir = "Wii/title/00010000/52385045/data";
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var dolphin = new[] { ".local/share/dolphin-emu", ".var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu", ".dolphin-emu" }
            .Select(d => Path.Combine(home, d, saveDir)).FirstOrDefault(Directory.Exists);
        var file = await PickFile("Choose a Super Paper Mario save: data.bin from a Wii's SD card, or wiimario00 in a Dolphin save folder",
            "Wii or Dolphin save (data.bin, wiimario00)", new[] { "data.bin", "wiimario0*" }, dolphin);
        if (file == null) return;
        var dataDir = SaveDataDir();
        if (Directory.Exists(dataDir) && Directory.EnumerateFiles(dataDir).Any() &&
            !await Message("Import save", "This replaces your current save files. They are kept as a backup next to them (data.backup-<date>).\n\nImport the save?", cancel: true)) return;
        try
        {
            var files = WiiSave.Import(file, dataDir);
            await Message("Import save", "Imported " + string.Join(", ", files) + ".");
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or System.Security.Cryptography.CryptographicException)
        {
            await Message("Import save", "The save could not be imported:\n" + e.Message);
        }
    }

    // The desktop's file picker (XDG portal or GTK); a typed path where neither exists.
    async Task<string?> PickFile(string title, string typeName, string[] patterns, string? startDir)
    {
        try
        {
            if (StorageProvider.CanOpen)
            {
                var options = new FilePickerOpenOptions
                {
                    Title = title, AllowMultiple = false,
                    FileTypeFilter = new[] { new FilePickerFileType(typeName) { Patterns = patterns }, FilePickerFileTypes.All },
                };
                if (startDir != null) options.SuggestedStartLocation = await StorageProvider.TryGetFolderFromPathAsync(startDir);
                var files = await StorageProvider.OpenFilePickerAsync(options);
                return files.Count > 0 ? files[0].TryGetLocalPath() : null;
            }
        }
        catch (Exception) { }  // no portal or GTK after all: ask for the path instead
        var path = await AskPath(title);
        return string.IsNullOrWhiteSpace(path) ? null : path.Trim().Trim('"', '\'');
    }

    async Task<string?> AskPath(string title)
    {
        var box = new TextBox { Width = 560, Watermark = "/home/you/Downloads/Super Paper Mario.iso" };
        var dialog = Dialog(title, Note(title + "\n\nType or paste the file's full path:", 560), box, out var ok, out var cancel);
        ok.Click += () => dialog.Close(box.Text);
        cancel.Click += () => dialog.Close(null);
        return await dialog.ShowDialog<string?>(this);
    }

    // The Windows launcher's MessageBox: OK, or OK and Cancel.
    async Task<bool> Message(string title, string text, bool cancel = false)
    {
        var dialog = Dialog(title, Note(text, 520), null, out var ok, out var no);
        no.IsVisible = cancel;
        ok.Click += () => dialog.Close(true);
        no.Click += () => dialog.Close(false);
        return await dialog.ShowDialog<bool>(this);
    }

    static Window Dialog(string title, Control text, Control? extra, out PaperButton ok, out PaperButton cancel)
    {
        var body = new StackPanel { Margin = new Thickness(22, 16, 22, 16) };
        body.Children.Add(text);
        if (extra != null) body.Children.Add(extra);
        ok = Btn("OK", null);
        cancel = Btn("Cancel", null, Paper);
        var buttons = Row(ok, cancel);
        buttons.HorizontalAlignment = HorizontalAlignment.Right;
        body.Children.Add(buttons);
        return new Window
        {
            Title = title, Content = body, SizeToContent = SizeToContent.WidthAndHeight, CanResize = false, ShowInTaskbar = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner, Background = new SolidColorBrush(Cream), Foreground = InkBrush,
            FontFamily = Font, FontSize = Body, Cursor = TippiCursor,
        };
    }

    async void Launch()
    {
        var (ok, text, _) = DiscStatus();
        if (!ok)
        {
            await Message("New Super Paper Bros.", text + "\n\nChoose your disc image on the Game page first.");
            OpenPage(0);
            return;
        }
        // A disc put in Disc/DATA by hand (a Dolphin-extracted folder) has no [paths] dvd_root yet, and the game has no
        // folder picker on Linux to ask for it: point the game at the folder the launcher found.
        if (config.Get("paths", "dvd_root") == null) { config.SetString("paths", "dvd_root", "../../Disc/DATA"); config.Save(); }
        // One program per disc region (the Windows build's engine_usa.dll / engine_pal.dll).
        var exe = Path.Combine(play, DiscGameId() == "R8PP01" ? "nspb-engine-pal" : "nspb-engine-usa");
        if (!File.Exists(exe))
        {
            await Message("New Super Paper Bros.", "The game files (the engine folder) are missing. Unpack the whole download again, keeping the engine folder next to this launcher.");
            return;
        }
        Gamepads.Release();
        var start = new ProcessStartInfo(exe) { WorkingDirectory = play, UseShellExecute = false };
        // The game's few bundled libraries (libpng) sit beside it.
        start.Environment["LD_LIBRARY_PATH"] = play + (Environment.GetEnvironmentVariable("LD_LIBRARY_PATH") is { Length: > 0 } more ? ":" + more : "");
        Process? game = null;
        try { game = Process.Start(start); } catch (Exception e) { await Message("New Super Paper Bros.", "The game could not start: " + e.Message); }
        if (game != null)
        {
            game.EnableRaisingEvents = true;
            game.Exited += (_, _) => Dispatcher.UIThread.Post(Gamepads.Resume);
            WindowState = WindowState.Minimized;
        }
        else Gamepads.Resume();
    }

    // ----- Display -----
    Control BuildDisplayPage()
    {
        var (card, body) = NewCard("Display");
        body.Children.Add(Choice("Display mode", "display_mode", new[] { ("Borderless fullscreen", "\"borderless\""), ("Windowed", "\"windowed\""), ("Exclusive fullscreen", "\"exclusive\"") }));
        body.Children.Add(Choice("Resolution", "resolution_multiplier", new[] { ("1080p", "2.25"), ("4K (2160p)", "4.5"), ("Match the screen", "0"), ("1440p", "3"), ("720p", "1.5"), ("480p (original)", "1") }));
        body.Children.Add(Choice("Aspect ratio", "aspect_mode", new[] { ("Cinematic 2.39:1", "3"), ("Fill the screen", "2"), ("16:9", "1"), ("Original 4:3", "0") }));
        body.Children.Add(Choice("Frame interpolation", "frame_interpolation_fps", new[] { ("120 FPS", "120"), ("180 FPS", "180"), ("240 FPS (fast PCs)", "240"), ("Off (60 FPS)", "0") }));
        body.Children.Add(Choice("Vsync", "vsync", new[] { ("On (no screen tearing)", "true"), ("Off", "false") }));
        body.Children.Add(Note("Changes are saved at once and used the next time the game starts. The same settings are in the game's F10 menu. " +
                               "Cinematic and Fill widen the game's camera (more of the level is visible); 16:9 keeps the Wii picture with bars on wider screens. " +
                               "Frame interpolation inserts frames between the game's 60 fps; turn it off if you see artifacts."));
        return card;
    }

    Control Choice(string label, string key, (string label, string value)[] items, string section = "video")
    {
        var combo = new ComboBox { Width = 260, ItemsSource = items.Select(i => i.label).ToList(), Margin = new Thickness(3, 4, 3, 4) };
        string current = config.Get(section, key) ?? "";
        int index = Array.FindIndex(items, i => i.value.Trim('"') == current || (double.TryParse(i.value, CultureInfo.InvariantCulture, out var a) && double.TryParse(current, CultureInfo.InvariantCulture, out var b) && a == b));
        combo.SelectedIndex = index < 0 ? 0 : index;
        combo.SelectionChanged += (_, _) => { if (combo.SelectedIndex >= 0) { config.Set(section, key, items[combo.SelectedIndex].value); config.Save(); } };
        var row = Row(new TextBlock { Text = label, Width = 200, FontWeight = FontWeight.Bold, VerticalAlignment = VerticalAlignment.Center }, combo);
        row.Margin = new Thickness(0, 4, 0, 4);
        return row;
    }

    // ----- Controls -----
    // Every connected gamepad drives Mario (runtime virtual_remote.cpp GamepadIntent), laid out for its type; the
    // Gamepad view remaps one type's actions into Config.toml [gamepad] <type>_<action>. Only the inputs actually
    // connected are listed (the keyboard, plus each pad SDL sees), refreshed every second.
    static readonly string[] PadStyles = { "xbox", "playstation", "nintendo" };
    // SDL inputs by position with their printed names on Xbox, PlayStation and Nintendo pads.
    static readonly (string token, string xbox, string ps, string nintendo)[] PadInputs =
    {
        ("a", "A", "Cross", "B"), ("b", "B", "Circle", "A"), ("x", "X", "Square", "Y"), ("y", "Y", "Triangle", "X"),
        ("leftshoulder", "LB", "L1", "L"), ("rightshoulder", "RB", "R1", "R"), ("lefttrigger", "LT", "L2", "ZL"), ("righttrigger", "RT", "R2", "ZR"),
        ("back", "View", "Create", "Minus"), ("start", "Menu", "Options", "Plus"), ("touchpad", "Touchpad", "Touchpad", "Touchpad"),
        ("leftstick", "Left stick click", "L3", "Left stick click"), ("rightstick", "Right stick click", "R3", "Right stick click"),
        ("dpup", "D-pad up", "D-pad up", "D-pad up"), ("dpdown", "D-pad down", "D-pad down", "D-pad down"),
        ("dpleft", "D-pad left", "D-pad left", "D-pad left"), ("dpright", "D-pad right", "D-pad right", "D-pad right"),
        ("leftx-", "Left stick left", "Left stick left", "Left stick left"), ("leftx+", "Left stick right", "Left stick right", "Left stick right"),
        ("lefty-", "Left stick up", "Left stick up", "Left stick up"), ("lefty+", "Left stick down", "Left stick down", "Left stick down"),
        ("misc1", "Share", "Mic", "Capture"), ("paddle1", "Paddle 1", "Paddle 1", "Paddle 1"), ("paddle2", "Paddle 2", "Paddle 2", "Paddle 2"),
        ("paddle3", "Paddle 3", "Paddle 3", "Paddle 3"), ("paddle4", "Paddle 4", "Paddle 4", "Paddle 4"),
        ("rightx-", "Right stick left", "Right stick left", "Right stick left"), ("rightx+", "Right stick right", "Right stick right", "Right stick right"),
        ("righty-", "Right stick up", "Right stick up", "Right stick up"), ("righty+", "Right stick down", "Right stick down", "Right stick down"),
    };
    // Must match kPadDefaults in virtual_remote.cpp (Xbox, PlayStation, Nintendo), in Actions order.
    static readonly string[][] PadDefaults =
    {
        new[] { "lefty-,dpup", "lefty-,dpup", "lefty-,dpup" }, new[] { "lefty+,dpdown", "lefty+,dpdown", "lefty+,dpdown" },
        new[] { "leftx-,dpleft", "leftx-,dpleft", "leftx-,dpleft" }, new[] { "leftx+,dpright", "leftx+,dpright", "leftx+,dpright" },
        new[] { "a", "a", "a,b" }, new[] { "b,x", "b,x", "x" }, new[] { "y", "y", "y" },
        new[] { "rightshoulder,righttrigger", "rightshoulder,righttrigger", "rightshoulder,righttrigger" },
        new[] { "", "", "" }, new[] { "", "", "" }, new[] { "start", "start", "start" },
        new[] { "back,leftshoulder,lefttrigger", "back,touchpad,leftshoulder,lefttrigger", "back,leftshoulder,lefttrigger" },
    };
    static readonly string[] PadActionLabels = { "Remote D-pad up", "Remote D-pad down", "Remote D-pad left", "Remote D-pad right" };

    static string PadLabel(int style, string token)
    {
        var input = PadInputs.FirstOrDefault(i => i.token == token);
        if (input.token == null) return token;
        return style switch { 1 => input.ps, 2 => input.nintendo, _ => input.xbox };
    }

    static string PadLabels(int style, string tokens) =>
        string.Join(" / ", tokens.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Select(t => PadLabel(style, t)));

    // A 4-column table: label, button, label, button; six actions per column pair.
    static Grid ActionGrid() => new()
    {
        ColumnDefinitions = new ColumnDefinitions("Auto,Auto,Auto,Auto"), RowDefinitions = new RowDefinitions("Auto,Auto,Auto,Auto,Auto,Auto"),
        Margin = new Thickness(0, 4, 0, 4), HorizontalAlignment = HorizontalAlignment.Left,
    };

    static void Place(Grid grid, Control control, int column, int row)
    {
        Grid.SetColumn(control, column);
        Grid.SetRow(control, row);
        grid.Children.Add(control);
    }

    static TextBlock ActionLabel(string text) => new() { Text = text, FontWeight = FontWeight.Bold, Margin = new Thickness(0, 12, 12, 0) };

    Control BuildControlsPage()
    {
        var (card, body) = NewCard("Controls");
        var inputCombo = new ComboBox { Width = 460, Margin = new Thickness(3, 4, 3, 4) };
        var inputRow = Row(new TextBlock { Text = "Input", Width = 110, FontWeight = FontWeight.Bold, VerticalAlignment = VerticalAlignment.Center }, inputCombo);
        inputRow.Margin = new Thickness(0, 0, 0, 6);
        body.Children.Add(inputRow);
        var gamepad = new StackPanel();
        var keyboard = new StackPanel();
        body.Children.Add(gamepad);
        body.Children.Add(keyboard);

        // Gamepad: one button per action; click it, then press the controller input to bind (Escape or a click cancels).
        int style = 0;
        uint padId = 0;
        gamepad.Children.Add(Note("This controller controls Mario and is set up automatically; the left stick moves the Wii Remote's D-pad. " +
                                  "To change an action, click it and press the button on your controller (Escape cancels)."));
        var padTable = ActionGrid();
        gamepad.Children.Add(padTable);
        PaperButton? capturingPad = null;
        var heldAtStart = new HashSet<string>();
        var padTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(30) };
        void FillPadTable()
        {
            padTable.Children.Clear();
            for (int i = 0; i < Actions.Length; i++)
            {
                string key = PadStyles[style] + "_" + Actions[i].key;
                Place(padTable, ActionLabel(i < PadActionLabels.Length ? PadActionLabels[i] : Actions[i].label), (i / 6) * 2, i % 6);
                string? custom = config.Get("gamepad", key);
                var b = Btn(PadLabels(style, custom ?? PadDefaults[i][style]), null, custom == null ? Paper : Color.Parse("#FFF0B0"));
                b.Width = 240;
                b.Height = 40;
                b.Margin = new Thickness(0, 4, 18, 4);
                b.Tag = key;
                b.Click += () =>
                {
                    if (capturingPad != null) { EndPadCapture(null); return; }
                    capturingPad = b;
                    b.Text = "Press a button...";
                    b.Back = Yellow;
                    heldAtStart = Gamepads.Active(padId);
                    padTimer.Start();
                };
                Place(padTable, b, (i / 6) * 2 + 1, i % 6);
            }
        }
        void EndPadCapture(string? token)
        {
            padTimer.Stop();
            if (capturingPad == null) return;
            if (token != null) { config.SetString("gamepad", (string)capturingPad.Tag!, token); config.Save(); }
            capturingPad = null;
            FillPadTable();
        }
        padTimer.Tick += (_, _) =>
        {
            var held = Gamepads.Active(padId);
            heldAtStart.IntersectWith(held);  // a key held when the capture began counts once it is released and pressed again
            var pressed = held.FirstOrDefault(t => !heldAtStart.Contains(t));
            if (pressed != null) EndPadCapture(pressed);
        };
        cancelPadCapture = () => { if (capturingPad == null) return false; EndPadCapture(null); return true; };
        gamepad.Children.Add(Row(Btn("Reset to automatic", () =>
        {
            EndPadCapture(null);
            foreach (var (key, _, _) in Actions) config.Remove("gamepad", PadStyles[style] + "_" + key);
            config.Save();
            FillPadTable();
        }, Paper)));

        // Keyboard: a sideways Wii Remote.
        keyboard.Children.Add(Note("The keyboard works as a sideways Wii Remote. Click a key to change it, then press the new key (Escape cancels, Delete clears)."));
        var table = ActionGrid();
        var buttons = new List<PaperButton>();
        for (int i = 0; i < Actions.Length; i++)
        {
            var (key, label, defaults) = Actions[i];
            Place(table, ActionLabel(label), (i / 6) * 2, i % 6);
            var b = Btn(KeyText(config.Get("keyboard", key) ?? defaults), null, Paper);
            b.MinWidth = 170;
            b.Margin = new Thickness(0, 4, 24, 4);
            b.Tag = key;
            b.Click += () => BeginCapture(b);
            buttons.Add(b);
            Place(table, b, (i / 6) * 2 + 1, i % 6);
        }
        keyboard.Children.Add(table);
        keyboard.Children.Add(Row(Btn("Reset to defaults", () =>
        {
            foreach (var (key, _, defaults) in Actions) config.Set("keyboard", key, "\"" + defaults + "\"");
            config.Save();
            foreach (var b in buttons) b.Text = KeyText(Actions.First(a => a.key == (string)b.Tag!).defaults);
        }, Paper)));

        // The input list: connected pads first (the first one is selected: it is the main controller), then the keyboard.
        var shown = new List<(uint id, string name, int style)>();
        var names = new List<string>();
        bool filling = false;
        void ShowSelected()
        {
            int i = inputCombo.SelectedIndex;
            bool isPad = i >= 0 && i < shown.Count;
            gamepad.IsVisible = isPad;
            keyboard.IsVisible = !isPad;
            if (isPad) { EndPadCapture(null); style = shown[i].style; padId = shown[i].id; FillPadTable(); }
        }
        void RefreshInputs()
        {
            var pads = Gamepads.Connected();
            if (pads.Count == 0 && screenshotDir != null) pads.Add((0, "Test controller (screenshot mode)", 0));
            if (pads.SequenceEqual(shown) && names.Count > 0) return;
            uint? selectedId = inputCombo.SelectedIndex >= 0 && inputCombo.SelectedIndex < shown.Count ? shown[inputCombo.SelectedIndex].id : null;
            bool keyboardSelected = names.Count > 0 && inputCombo.SelectedIndex == shown.Count;
            shown.Clear();
            shown.AddRange(pads);
            names.Clear();
            foreach (var pad in pads) names.Add($"{pad.name}  ({new[] { "Xbox", "PlayStation", "Nintendo" }[pad.style]} layout)");
            names.Add(pads.Count == 0 ? "Keyboard  (no controller connected - plug one in and it appears here)" : "Keyboard");
            int keep = selectedId is uint id ? shown.FindIndex(p => p.id == id) : -1;
            filling = true;
            inputCombo.ItemsSource = names.ToList();
            inputCombo.SelectedIndex = keyboardSelected ? shown.Count : keep >= 0 ? keep : 0;
            filling = false;
            ShowSelected();
        }
        inputCombo.SelectionChanged += (_, _) => { if (!filling) ShowSelected(); };
        RefreshInputs();
        var poll = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        poll.Tick += (_, _) => { if (card.IsVisible) RefreshInputs(); };
        poll.Start();
        return card;
    }

    static string KeyText(string keys) => keys.Length == 0 ? "None" : keys.Replace(",", " / ");

    void BeginCapture(PaperButton button)
    {
        if (capturing != null) { capturing.Text = captureKey!; capturing.Back = Paper; }
        capturing = button;
        captureKey = button.Text;
        button.Text = "Press a key...";
        button.Back = Yellow;
    }

    void OnKey(object? sender, KeyEventArgs e)
    {
        if (capturing == null)
        {
            if (e.Key == Key.Escape && cancelPadCapture?.Invoke() == true) { e.Handled = true; return; }
            if (openPage >= 0 && e.Key == Key.Escape) { ClosePage(); e.Handled = true; return; }
            // The title screen's menu: arrow keys move, Enter or Space picks.
            if (openPage < 0 && e.Key is Key.Up or Key.Down) { view.Move(e.Key == Key.Down ? 1 : -1); e.Handled = true; return; }
            if (openPage < 0 && view.Hover >= 0 && e.Key is Key.Enter or Key.Space) { menu[view.Hover].act(); e.Handled = true; }
            return;
        }
        e.Handled = true;
        string? name = e.Key == Key.Escape ? null : e.Key == Key.Delete ? "" : SdlName(e.Key);
        if (name == null && e.Key != Key.Escape) return;  // a key SDL has no name for here: keep waiting
        if (name != null)
        {
            config.Set("keyboard", (string)capturing.Tag!, "\"" + name + "\"");
            config.Save();
            captureKey = KeyText(name);
        }
        capturing.Text = captureKey!;
        capturing.Back = Paper;
        capturing = null;
    }

    // SDL scancode names, as Keybinds::Parse (SDL_GetScancodeFromName) reads them from Config.toml.
    static string? SdlName(Key k) => k switch
    {
        >= Key.A and <= Key.Z => k.ToString(),
        >= Key.D0 and <= Key.D9 => ((char)('0' + (k - Key.D0))).ToString(),
        >= Key.NumPad0 and <= Key.NumPad9 => "Keypad " + (k - Key.NumPad0),
        >= Key.F1 and <= Key.F12 => k.ToString(),
        Key.Up => "Up", Key.Down => "Down", Key.Left => "Left", Key.Right => "Right",
        Key.Space => "Space", Key.Return => "Return", Key.Tab => "Tab", Key.Back => "Backspace", Key.Insert => "Insert",
        Key.Home => "Home", Key.End => "End", Key.PageUp => "PageUp", Key.PageDown => "PageDown", Key.CapsLock => "CapsLock",
        Key.LeftShift => "Left Shift", Key.RightShift => "Right Shift", Key.LeftCtrl => "Left Ctrl", Key.RightCtrl => "Right Ctrl",
        Key.LeftAlt => "Left Alt", Key.RightAlt => "Right Alt",
        Key.OemMinus => "-", Key.OemPlus => "=", Key.OemOpenBrackets => "[", Key.OemCloseBrackets => "]", Key.OemComma => ",",
        Key.OemPeriod => ".", Key.OemQuestion => "/", Key.OemSemicolon => ";", Key.OemQuotes => "'", Key.OemTilde => "`",
        _ => null,
    };

    // ----- Game disc (part of the Play page) -----
    // One button sets the disc up (or changes it later); extraction progress shows under it, and the dump
    // instructions only while no disc is set up.
    void AddDiscSetup(StackPanel body)
    {
        var progress = new ProgressBar { Width = 640, Height = 22, Margin = new Thickness(0, 6, 0, 2), IsVisible = false, HorizontalAlignment = HorizontalAlignment.Left };
        var progressText = Note("");
        progressText.IsVisible = false;
        void Say(string text) { progressText.Text = text; progressText.IsVisible = text.Length > 0; }  // no empty gap
        var choose = chooseDisc = Btn("Choose disc image (.iso, .ciso, .wbfs)...", null);
        choose.HorizontalAlignment = HorizontalAlignment.Left;
        choose.Click += async () =>
        {
            var file = await PickFile("Choose your Super Paper Mario disc image", "Wii disc images (*.iso, *.ciso, *.wbfs)",
                new[] { "*.iso", "*.ciso", "*.wbfs", "*.ISO", "*.CISO", "*.WBFS" }, null);
            if (file == null) return;
            WiiDisc disc;
            try { disc = WiiDisc.Open(file); }
            catch (Exception ex) { Say("Could not read that image: " + ex.Message); return; }
            using (disc)
            {
                if (disc.Problem() is { Length: > 0 } problem) { Say(problem); return; }
                choose.IsEnabled = false;
                progress.IsVisible = true;
                var staging = discDir + ".new";
                try
                {
                    if (Directory.Exists(staging)) Directory.Delete(staging, true);
                    var report = new Progress<(int done, int total, string name)>(p =>
                    {
                        progress.Maximum = p.total;
                        progress.Value = Math.Min(p.done, p.total);
                        Say($"Extracting {p.done}/{p.total}  {p.name}");
                    });
                    await Task.Run(() => disc.Extract(staging, (d, t, n) => ((IProgress<(int, int, string)>)report).Report((d, t, n))));
                    if (disc.Holes > 0) throw new InvalidDataException("the image is incomplete (blocks missing); dump the disc again.");
                    if (Directory.Exists(discDir)) Directory.Delete(discDir, true);
                    Directory.Move(staging, discDir);
                    config.SetString("paths", "dvd_root", "../../Disc/DATA");  // relative to engine/UserData, so the folder can move
                    config.Save();
                    Say("Done. The disc is extracted and ready.");
                }
                catch (Exception ex)
                {
                    Say("Extraction failed: " + ex.Message);
                }
                finally
                {
                    choose.IsEnabled = true;
                    progress.IsVisible = false;
                    RefreshStatus();
                }
            }
        };
        body.Children.Add(choose);
        body.Children.Add(progress);
        body.Children.Add(progressText);
        body.Children.Add(dumpHelp = Note(
            "This port runs your own copy of Super Paper Mario (USA R8PE01 or Europe R8PP01). It needs a disc image of the disc you own, " +
            "which the launcher decrypts and extracts once into its Disc folder (about 400 MB).\n\n" +
            "Making a legal image of your disc (Wii or Wii U consoles can read the disc; PC drives can't):\n" +
            "1. On a Wii with the Homebrew Channel, run CleanRip (or USB Loader GX's install option).\n" +
            "2. Insert Super Paper Mario and dump it to an SD card or USB drive. CleanRip writes a .iso " +
            "(it may be split into .part files: join them with  cat game.part* > game.iso ); USB Loader GX writes a .wbfs.\n" +
            "3. Copy the file to this PC and choose it with the button above. .iso, .ciso and .wbfs all work.\n" +
            "The USA release (revision 0) and the European release work; the European disc also offers French, German, Spanish and Italian. " +
            "A Dolphin-extracted folder also works: put it in Disc/DATA next to the launcher."));
    }

    (bool ok, string text, string path) DiscStatus()
    {
        string? configured = config.Get("paths", "dvd_root");
        string path = configured == null ? discDir : Path.GetFullPath(Path.Combine(userData, configured));
        if (!File.Exists(Path.Combine(path, "sys", "boot.bin")))
            return (false, "No game disc set up yet. Choose your Super Paper Mario disc image below; the launcher extracts it into its Disc folder.", path);
        var header = new byte[8];
        using (var f = File.OpenRead(Path.Combine(path, "sys", "boot.bin"))) f.ReadExactly(header, 0, 8);
        string id = System.Text.Encoding.ASCII.GetString(header, 0, 6);
        if (!(id == "R8PE01" && header[7] == 0) && id != "R8PP01")
            return (false, $"The disc folder holds {id} revision {header[7]}, not Super Paper Mario USA revision 0 or Europe: {path}", path);
        return (true, $"Super Paper Mario ({id}, {(id == "R8PP01" ? "Europe" : "USA")}) is ready: " + path, path);
    }

    void RefreshStatus()
    {
        var (ok, text, _) = DiscStatus();
        playStatus.Text = ok ? "Disc: ready.  Mods: " + ModNames() : text;
        playStatus.Foreground = new SolidColorBrush(ok ? Ink : Red);
        chooseDisc.Text = ok ? "Change disc..." : "Choose disc image (.iso, .ciso, .wbfs)...";
        chooseDisc.Back = ok ? Mint : Yellow;
        dumpHelp.IsVisible = !ok;
        gameNote.IsVisible = ok;
        view.InvalidateVisual();
    }

    string ModNames()
    {
        var names = Directory.Exists(modsDir) ? Directory.GetDirectories(modsDir).Where(d => !File.Exists(Path.Combine(d, "disabled"))).Select(Path.GetFileName).ToArray() : Array.Empty<string>();
        return names.Length == 0 ? "none" : string.Join(", ", names);
    }

    // ----- Mods -----
    Control BuildModsPage()
    {
        var (card, body) = NewCard("Mods");
        body.Children.Add(Note("Install a mod from its .zip or its folder. Ticked mods load when the game starts; untick one to turn it off. " +
                               "A mod holds a files folder (it replaces game files) and/or a textures folder."));
        body.Children.Add(Row(
            Btn("Install mod (.zip)...", async () => { if (await PickFile("Choose a mod (.zip)", "Mod archive (*.zip)", new[] { "*.zip" }, null) is { } zip) await InstallMod(zip); }),
            Btn("Install mod folder...", async () => { if (await PickFolder("Choose the mod's folder (the one holding files or textures)") is { } dir) await InstallMod(dir); }, Paper),
            Btn("Open mods folder", () => Open(Directory.CreateDirectory(modsDir).FullName), Paper)));
        modList = new StackPanel { Margin = new Thickness(8, 4, 8, 4) };
        var list = new StackPanel();
        list.Children.Add(new Border
        {
            Child = new TextBlock { Text = "Installed mods", Margin = new Thickness(8, 4, 8, 4) },
            BorderBrush = new SolidColorBrush(Color.Parse("#E0E0E0")), BorderThickness = new Thickness(0, 0, 0, 1),
        });
        list.Children.Add(new ScrollViewer { Content = modList, Height = 210 });
        body.Children.Add(new Border
        {
            Child = list, Width = 660, Background = Brushes.White, BorderBrush = new SolidColorBrush(Color.Parse("#828790")), BorderThickness = new Thickness(1),
            Margin = new Thickness(0, 6, 0, 6), HorizontalAlignment = HorizontalAlignment.Left,
        });
        LoadMods();
        return card;
    }

    async Task<string?> PickFolder(string title)
    {
        try
        {
            if (StorageProvider.CanPickFolder)
            {
                var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = title, AllowMultiple = false });
                return folders.Count > 0 ? folders[0].TryGetLocalPath() : null;
            }
        }
        catch (Exception) { }  // no portal or GTK after all: ask for the path instead
        var path = await AskPath(title);
        return string.IsNullOrWhiteSpace(path) ? null : path.Trim().Trim('"', '\'');
    }

    // Installs a mod from a .zip or a folder into mods/<name> (ModInstall.cs, shared with the Windows launcher).
    async Task InstallMod(string source)
    {
        string? temp = null;
        try
        {
            string from = source;
            if (File.Exists(source))
            {
                temp = Path.Combine(Path.GetTempPath(), "nspb-mod-" + Guid.NewGuid().ToString("N"));
                ModInstall.ExtractZip(source, temp);
                from = temp;
            }
            var modRoot = ModInstall.FindRoot(from);
            if (modRoot == null) { await Message("Install mod", "This doesn't look like a New Super Paper Bros. mod: it has no files or textures folder."); return; }
            string name = ModInstall.SafeName(modRoot == temp ? Path.GetFileNameWithoutExtension(source) : Path.GetFileName(modRoot.TrimEnd('/')));
            var dest = Path.Combine(modsDir, name);
            if (Directory.Exists(dest))
            {
                if (!await Message("Install mod", $"A mod called \"{name}\" is already installed. Replace it?", cancel: true)) return;
                Directory.Delete(dest, true);
            }
            ModInstall.CopyDir(modRoot, dest);
            LoadMods();
            RefreshStatus();
            await Message("Install mod", $"Installed \"{name}\". It loads the next time the game starts.");
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            await Message("Install mod", "The mod could not be installed:\n" + e.Message);
        }
        finally
        {
            if (temp != null) try { Directory.Delete(temp, true); } catch (Exception) { }
        }
    }

    void LoadMods()
    {
        modList.Children.Clear();
        if (!Directory.Exists(modsDir)) return;
        foreach (var dir in Directory.GetDirectories(modsDir).OrderBy(d => d, StringComparer.Ordinal))
        {
            var box = new CheckBox { Content = Path.GetFileName(dir), IsChecked = !File.Exists(Path.Combine(dir, "disabled")), Cursor = TippiHand, MinWidth = 480 };
            box.IsCheckedChanged += (_, _) =>
            {
                var marker = Path.Combine(dir, "disabled");
                if (box.IsChecked == true) File.Delete(marker); else File.WriteAllText(marker, "");
                RefreshStatus();
            };
            var remove = Btn("Remove", async () =>
            {
                if (!await Message("Remove mod", $"Remove \"{Path.GetFileName(dir)}\"? Its folder is deleted.", cancel: true)) return;
                try { Directory.Delete(dir, true); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { await Message("Remove mod", "The mod could not be removed:\n" + e.Message); }
                LoadMods();
                RefreshStatus();
            }, Paper);
            remove.Margin = new Thickness(0, 2, 0, 2);
            modList.Children.Add(Row(box, remove));
        }
    }
}

// The launcher's buttons (the Windows launcher's PillButton and TileButton): a bubbly pill with a light top and a darker
// lip underneath (Tomodachi Life), or, as a page tile, a coloured rounded block with white italic text (Smash 4).
sealed class PaperButton : Border
{
    readonly TextBlock label;
    Color back;
    bool hover, tile, selected;
    IBrush? textBrush;
    public event Action? Click;

    public PaperButton(string text, Color back, double fontSize)
    {
        label = new TextBlock { Text = text, FontWeight = FontWeight.Bold, FontSize = fontSize, VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center,
                                HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center };
        Child = label;
        BorderThickness = new Thickness(2);
        Padding = new Thickness(18, 6, 18, 6);
        Margin = new Thickness(0, 6, 10, 6);
        CornerRadius = new CornerRadius(999);
        Cursor = MainWindow.TippiHand;
        HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left;  // sized to the label, like the Windows AutoSize buttons
        Back = back;
        PointerEntered += (_, _) => { hover = true; Paint(); };
        PointerExited += (_, _) => { hover = false; Paint(); };
        PointerPressed += (_, e) =>
        {
            if (!IsEnabled || !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            e.Handled = true;
            Click?.Invoke();
        };
        PropertyChanged += (_, e) => { if (e.Property == IsEnabledProperty) Opacity = IsEnabled ? 1 : 0.55; };
    }

    public string Text { get => label.Text ?? ""; set => label.Text = value; }
    // White would vanish on the white cards: those buttons are mint.
    public Color Back { get => back; set { back = value == MainWindow.Paper ? MainWindow.Mint : value; Paint(); } }
    public IBrush? TextBrush { set { textBrush = value; Paint(); } }
    public bool Selected { set { selected = value; Paint(); } }

    // A page tile in the nav column: bold italic white text on the left; the selected one sticks out with a white ring.
    public void MakeTile()
    {
        tile = true;
        label.FontStyle = FontStyle.Italic;
        label.HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left;
        CornerRadius = new CornerRadius(14);
        Paint();
    }

    void Paint()
    {
        Color face;
        if (tile)
        {
            face = selected ? MainWindow.Mix(back, Colors.White, 0.12) : hover ? MainWindow.Mix(back, Colors.White, 0.25) : MainWindow.Mix(back, MainWindow.Cream, 0.35);
            BorderBrush = selected ? Brushes.White : Brushes.Transparent;
            BorderThickness = new Thickness(selected ? 4 : 0);
            Margin = new Thickness(selected ? 0 : 10, 0, 0, 12);
            BoxShadow = BoxShadows.Parse("3 5 0 0 #371E1E3C");
            label.Foreground = Brushes.White;
        }
        else
        {
            face = hover ? MainWindow.Mix(back, Colors.White, 0.18) : back;
            BorderBrush = new SolidColorBrush(MainWindow.Mix(back, MainWindow.Ink, 0.25));
            BoxShadow = new BoxShadows(new BoxShadow { OffsetY = 4, Color = MainWindow.Mix(back, MainWindow.Ink, 0.35) });
            bool light = back.R * 0.299 + back.G * 0.587 + back.B * 0.114 > 170;  // perceived brightness
            label.Foreground = textBrush ?? (light ? MainWindow.InkBrush : Brushes.White);
        }
        Background = new LinearGradientBrush
        {
            StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
            GradientStops = { new GradientStop(MainWindow.Mix(face, Colors.White, tile ? 0.18 : 0.35), 0), new GradientStop(face, 1) },
        };
    }
}
