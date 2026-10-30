using System.Diagnostics;
using System.Drawing.Drawing2D;

namespace NewSuperPaperBros;

// The launcher window, laid out like a game's title screen (after Dusklight's launcher): an animated Super Paper Mario
// scene darkened from the left, the logo, a column of big menu words (Play, Game, Display, Controls, Mods, Quit), disc
// status bottom-left and version bottom-right. Game, Display, Controls and Mods open as a card over the dimmed scene,
// with Back (or Escape) to return. Everything it changes goes to <game folder>\UserData\Config.toml (engine\ in a release, Play\ in the dev tree), read by the game at start.
sealed class MainForm : Form
{
    static readonly Color Cream = ColorTranslator.FromHtml("#EEF0F6"), Paper = Color.White,
        Ink = ColorTranslator.FromHtml("#3B3B52"), Yellow = ColorTranslator.FromHtml("#FFC83D"),
        Red = ColorTranslator.FromHtml("#EC3A55"), Sky = ColorTranslator.FromHtml("#6FC3FF"), Green = ColorTranslator.FromHtml("#6FD36A"),
        Pink = ColorTranslator.FromHtml("#FFD6EA"), Mint = ColorTranslator.FromHtml("#D3F5EA");
    // Each page's colour: its card's border and heading.
    static readonly Color[] TileColors =
    {
        ColorTranslator.FromHtml("#EC3A55"), ColorTranslator.FromHtml("#3B8CFF"), ColorTranslator.FromHtml("#2EC27E"), ColorTranslator.FromHtml("#FF9A1F"),
    };
    static Color Mix(Color a, Color b, float f) => Color.FromArgb(Lerp(a.R, b.R, f), Lerp(a.G, b.G, f), Lerp(a.B, b.B, f));

    readonly bool selfTest;
    readonly string? screenshotDir;
    // Fonts are in points and grow with Windows display scaling; every pixel size goes through S() so it grows too.
    static float k = 1f;
    static int S(int v) => (int)Math.Round(v * k);
    static Size S(int w, int h) => new(S(w), S(h));
    static Padding S(int l, int t, int r, int b) => new(S(l), S(t), S(r), S(b));
    Button chooseDisc = null!;
    // play = the folder holding the game exe and its UserData: engine\ in a release, Play\ in the dev tree.
    readonly string root, play, gameExe, userData, configPath, modsDir, discDir;
    // The one version number: <Version> in NewSuperPaperBros.Launcher.csproj (package-spm.ps1 names the zip after it).
    static readonly string AppVersion = typeof(MainForm).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "?";
    readonly GameConfig config;
    readonly Dictionary<string, Image> sprites = new();
    readonly TitleView view;
    readonly List<(string name, Control page)> pages = new();
    readonly (string label, Action act)[] menu;
    int openPage = -1;  // the page shown over the title screen, -1 = none
    Button backButton = null!;
    Label playStatus = null!, dumpHelp = null!, gameNote = null!;
    ListView modList = null!;
    Button? capturing;
    Func<bool>? cancelPadCapture;  // the Controls page's gamepad capture: Escape ends it
    string? captureKey;

    static readonly (string key, string label, string defaults)[] Actions =
    {
        ("up", "Up", "Up"), ("down", "Down", "Down"), ("left", "Left", "Left"), ("right", "Right", "Right"),
        ("jump", "Jump (2)", "X,Space"), ("run", "Pixl / action (1)", "Z,Left Shift"), ("a", "Flip (A)", "A"),
        ("shake", "Shake (stylish)", "C,Left Ctrl"), ("tilt_left", "Tilt left", "Q"), ("tilt_right", "Tilt right", "E"),
        ("plus", "Menu (+)", "Return,Escape"), ("minus", "Minus (-)", "-,Tab"),
    };

    public MainForm(bool selfTest, string? screenshotDir = null)
    {
        this.selfTest = selfTest;
        this.screenshotDir = screenshotDir;
        k = DeviceDpi / 96f;
        (root, play, gameExe) = FindGame();
        // Zips drop the hidden attribute: keep the release's engine\ folder out of sight so only this exe shows.
        try
        {
            var engineDir = new DirectoryInfo(play);
            if (engineDir.Exists && engineDir.Name.Equals("engine", StringComparison.OrdinalIgnoreCase) && !engineDir.Attributes.HasFlag(FileAttributes.Hidden))
                engineDir.Attributes |= FileAttributes.Hidden;
        }
        catch (Exception) { }  // read-only media or no rights: cosmetic only
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
            if (File.Exists(file)) sprites[name] = Image.FromStream(new MemoryStream(File.ReadAllBytes(file)));
        }

        Text = "New Super Paper Bros.  v" + AppVersion;
        tippiCursor = MakeTippiCursor(clickable: false);
        tippiHand = MakeTippiCursor(clickable: true);
        Cursor = tippiCursor;  // child controls without their own cursor inherit it
        Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!);  // Pixl, embedded from pixl.ico
        var area = Screen.PrimaryScreen?.WorkingArea.Size ?? new Size(S(1100), S(760));
        // 16:9, the shape of the title artwork, so all of it shows.
        int cw = Math.Min(S(1200), Math.Min(area.Width - 40, (area.Height - 60) * 16 / 9));
        ClientSize = new Size(cw, cw * 9 / 16);
        MinimumSize = new Size(Math.Min(S(960), area.Width), Math.Min(S(580), area.Height));
        BackColor = Cream;
        Font = new Font("Segoe UI", 10.5f);
        KeyPreview = true;
        StartPosition = FormStartPosition.CenterScreen;
        if (screenshotDir != null) { StartPosition = FormStartPosition.Manual; Location = new Point(-6000, -6000); ShowInTaskbar = false; }  // off-screen
        DoubleBuffered = true;

        // One full-window title screen; the pages open as cards on top of it.
        view = new TitleView { Dock = DockStyle.Fill };
        view.Draw = PaintTitle;
        view.MouseMove += (_, e) =>
        {
            if (openPage >= 0) return;
            hover = menuRects.FindIndex(r => r.Contains(e.Location));
            view.Cursor = hover >= 0 || discRect.Contains(e.Location) ? tippiHand : tippiCursor;
        };
        view.MouseLeave += (_, _) => hover = -1;
        // A click on the disc status opens Game.
        view.MouseClick += (_, e) =>
        {
            if (openPage >= 0 || e.Button != MouseButtons.Left) return;
            int i = menuRects.FindIndex(r => r.Contains(e.Location));
            if (i >= 0) menu[i].act();
            else if (discRect.Contains(e.Location)) OpenPage(0);
        };
        view.Resize += (_, _) => { LayoutPage(); if (openPage >= 0) FreezeScene(); };
        backButton = Btn("◀  Back", (_, _) => ClosePage(), Paper);
        backButton.Visible = false;
        view.Controls.Add(backButton);
        Controls.Add(view);
        var frame = new System.Windows.Forms.Timer { Interval = 33 };
        frame.Tick += (_, _) => { if (animate && openPage < 0 && WindowState != FormWindowState.Minimized) view.Invalidate(); };
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
        Shown += (_, _) =>
        {
            introAt = T;
            if (screenshotDir != null)
            {
                // Headless visual check: the title screen and every page drawn to <dir>\<name>.png.
                Directory.CreateDirectory(screenshotDir);
                introAt = -10;  // intro animation already done
                void Shot(string name)
                {
                    Application.DoEvents();
                    using var bmp = new Bitmap(Width, Height);
                    DrawToBitmap(bmp, new Rectangle(Point.Empty, Size));
                    bmp.Save(Path.Combine(screenshotDir, name + ".png"));
                }
                hover = 0;
                Shot("Title");
                hover = -1;
                for (int i = 0; i < pages.Count; i++) { OpenPage(i); Shot(pages[i].name); }
                ClosePage();
                using (var cur = new Bitmap(128, 64))
                using (var g = Graphics.FromImage(cur)) { g.Clear(Sky); for (int i = 0; i < cursorBitmaps.Count; i++) g.DrawImageUnscaled(cursorBitmaps[i], i * 64, 0); cur.Save(Path.Combine(screenshotDir, "cursor.png")); }
                Close();
            }
            else if (selfTest)
            {
                for (int i = 0; i < pages.Count; i++) OpenPage(i);
                ClosePage();
                Environment.ExitCode = !File.Exists(gameExe) ? 6 : !ModInstall.SelfCheck() ? 7 : !Gamepads.Ready ? 4 : Gamepads.Connected().All(p => Gamepads.Opens(p.id)) ? 0 : 5;
                Close();
            }
        };
    }

    // The release keeps the game in engine\ (renamed so nobody double-clicks it); the dev tree keeps it in Play\.
    // Search up from the launcher's own folder, so launcher\publish\ works in the dev tree too.
    // The engine exe loads engine_usa.dll or engine_pal.dll beside it by the extracted disc's id (engine_loader.cpp).
    public const string EngineExe = "nspb-engine.exe";
    static (string root, string dir, string exe) FindGame()
    {
        var start = new DirectoryInfo(AppContext.BaseDirectory);
        for (var d = start; d != null; d = d.Parent)
        {
            foreach (var (dir, exe) in new[] { ("engine", EngineExe), ("Play", "SuperPaperMario.exe") })
                if (File.Exists(Path.Combine(d.FullName, dir, exe))) return (d.FullName, Path.Combine(d.FullName, dir), Path.Combine(d.FullName, dir, exe));
        }
        return (start.FullName, Path.Combine(start.FullName, "engine"), Path.Combine(start.FullName, "engine", EngineExe));
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
        if (page is Panel card)
        {
            card.Tag = accent;  // the card's border and heading take the page's colour
            card.BackColor = Color.Transparent;  // its rounded corners show the scene
            if (card.Controls.Count > 0 && card.Controls[0].Controls.Count > 0) card.Controls[0].Controls[0].ForeColor = Mix(accent, Ink, 0.25f);
        }
        page.Visible = false;
        view.Controls.Add(page);
        pages.Add((name, page));
    }

    // A page opens as a card over the dimmed scene, with Back (or Escape) to return to the title screen.
    void OpenPage(int index)
    {
        openPage = index;
        FreezeScene();
        for (int i = 0; i < pages.Count; i++) pages[i].page.Visible = i == index;
        backButton.Visible = true;
        LayoutPage();
        RefreshStatus();
    }

    void ClosePage()
    {
        if (openPage < 0) return;
        cancelPadCapture?.Invoke();
        foreach (var (_, page) in pages) page.Visible = false;
        backButton.Visible = false;
        openPage = -1;
        view.BackgroundImage = null;
        frozen?.Dispose();
        frozen = null;
        RefreshStatus();
        view.Invalidate();
    }

    void LayoutPage()
    {
        if (openPage < 0) return;
        backButton.Location = new Point(S(24), S(16));
        int top = backButton.Bottom + S(8);
        pages[openPage].page.Bounds = new Rectangle(S(20), top, view.ClientSize.Width - S(40), view.ClientSize.Height - top - S(16));
    }

    // ----- The fun bits: the animated scene and its characters -----
    // Everything moves on one clock; with Windows animations turned off the scene stands still.
    readonly bool animate = SystemInformation.UIEffectsEnabled;
    readonly Stopwatch clock = Stopwatch.StartNew();
    double T => animate ? clock.Elapsed.TotalSeconds : 1.0;
    readonly Dictionary<(string, int, int, bool), Bitmap> scaled = new();

    static int Lerp(int a, int b, float f) => (int)(a + (b - a) * f);

    // The pointer (Tippi) and the one over anything clickable (Tippi with a sparkle). Static so Btn() can use it.
    static Cursor tippiCursor = Cursors.Default, tippiHand = Cursors.Hand;
    readonly List<Bitmap> cursorBitmaps = new();  // the pointer images, for the screenshot check

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    struct IconInfo { public bool fIcon; public int xHotspot, yHotspot; public IntPtr hbmMask, hbmColor; }
    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern IntPtr CreateIconIndirect(ref IconInfo info);
    [System.Runtime.InteropServices.DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);

    // Tippi as the mouse pointer, sized for the display scaling, clicking with her top-left wing tip. Over clickable
    // things she is a little smaller with a sparkle at her side, so it still shows what can be clicked.
    Cursor MakeTippiCursor(bool clickable)
    {
        if (!sprites.TryGetValue("tippi", out var img)) return clickable ? Cursors.Hand : Cursors.Default;
        int size = S(32);
        using var bmp = new Bitmap(size, size);
        using (var g = Graphics.FromImage(bmp))
        {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            var b = Opaque((Bitmap)img);  // her drawn area, without the sprite's empty margin
            float fit = (clickable ? 0.78f : 1f) * size / Math.Max(b.Width, b.Height);  // whole Tippi inside the square
            g.DrawImage(img, new Rectangle(0, 0, (int)(b.Width * fit), (int)(b.Height * fit)), b, GraphicsUnit.Pixel);
            if (clickable)
            {
                // A bold four-point sparkle in the bottom-right corner.
                g.SmoothingMode = SmoothingMode.AntiAlias;
                float cx = size * 0.76f, cy = size * 0.76f, r = size * 0.23f, n = r * 0.32f;
                using var star = new GraphicsPath();
                star.AddPolygon(new[] { new PointF(cx, cy - r), new PointF(cx + n, cy - n), new PointF(cx + r, cy), new PointF(cx + n, cy + n),
                                        new PointF(cx, cy + r), new PointF(cx - n, cy + n), new PointF(cx - r, cy), new PointF(cx - n, cy - n) });
                using (var ink = new Pen(Ink, Math.Max(1.5f, size / 20f)) { LineJoin = LineJoin.Round }) g.DrawPath(ink, star);
                using (var fill = new SolidBrush(Yellow)) g.FillPath(fill, star);
            }
        }
        int hx = 0, hy = 0;
        for (int y = 0, found = 0; y < size && found == 0; y++)
            for (int x = 0; x < size / 2; x++)
                if (bmp.GetPixel(x, y).A > 128) { hx = x; hy = y; found = 1; break; }
        var info = new IconInfo { fIcon = false, xHotspot = hx, yHotspot = hy, hbmColor = bmp.GetHbitmap(Color.FromArgb(0)), hbmMask = new Bitmap(size, size).GetHbitmap() };
        cursorBitmaps.Add((Bitmap)bmp.Clone());
        var handle = CreateIconIndirect(ref info);
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        return handle == IntPtr.Zero ? Cursors.Default : new Cursor(handle);
    }

    // Bounding box of a sprite's visible pixels.
    static Rectangle Opaque(Bitmap b)
    {
        int l = b.Width, t = b.Height, r = -1, bo = -1;
        for (int y = 0; y < b.Height; y += 2)
            for (int x = 0; x < b.Width; x += 2)
                if (b.GetPixel(x, y).A > 24) { l = Math.Min(l, x); t = Math.Min(t, y); r = Math.Max(r, x); bo = Math.Max(bo, y); }
        return r < 0 ? new Rectangle(0, 0, b.Width, b.Height) : Rectangle.FromLTRB(l, t, r + 1, bo + 1);
    }

    // A sprite resized once and kept (drawing a pre-sized bitmap every frame is cheap).
    Bitmap Sprite(string name, int w, int h, bool flip = false)
    {
        if (scaled.TryGetValue((name, w, h, flip), out var b)) return b;
        b = new Bitmap(Math.Max(1, w), Math.Max(1, h));
        using (var g = Graphics.FromImage(b))
        {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            if (sprites.TryGetValue(name, out var img)) g.DrawImage(img, new Rectangle(0, 0, w, h));
        }
        if (flip) b.RotateFlip(RotateFlipType.RotateNoneFlipX);
        return scaled[(name, w, h, flip)] = b;
    }


    // ----- The title screen -----
    // Like Dusklight's launcher: artwork fills the window (startup	itle-*.jpg: the Super Paper Mario wallpaper by
    // FawfulTheGreat64, see CREDITS.txt) and darkens toward the left, where the logo and a column of big menu words
    // sit; disc status in the bottom-left corner, version in the top-right (the art has its own logo bottom-right).
    static readonly Color Accent = ColorTranslator.FromHtml("#FEE685"), ReadyGreen = ColorTranslator.FromHtml("#D8F999"),
        ErrorPink = ColorTranslator.FromHtml("#FFC9C9"), Muted = ColorTranslator.FromHtml("#C9C3BE"), Night = ColorTranslator.FromHtml("#16122B");
    Bitmap? frozen;  // the dimmed scene behind an open page (child cards are transparent over BackgroundImage)

    Image? titleArt;  // loaded in BuildScene the first time
    Bitmap? sceneSky;

    // One of the title scenes beside the game (engine\startup	itle-*.jpg), or none (a plain night sky is drawn).
    Image? LoadTitleArt()
    {
        var files = Directory.Exists(Path.Combine(play, "startup")) ? Directory.GetFiles(Path.Combine(play, "startup"), "title-*.jpg") : Array.Empty<string>();
        if (files.Length == 0) return null;
        try { return Image.FromStream(new MemoryStream(File.ReadAllBytes(files[Random.Shared.Next(files.Length)]))); }
        catch (Exception e) when (e is IOException or ArgumentException) { return null; }
    }

    // The artwork scaled to cover the window, centred.
    void BuildScene(Size size)
    {
        titleArt ??= LoadTitleArt();
        sceneSky?.Dispose();
        sceneSky = new Bitmap(size.Width, size.Height);
        using var g = Graphics.FromImage(sceneSky);
        if (titleArt == null)
        {
            using var sky = new LinearGradientBrush(new Rectangle(Point.Empty, size), ColorTranslator.FromHtml("#2B2257"), Night, LinearGradientMode.Vertical);
            g.FillRectangle(sky, 0, 0, size.Width, size.Height);
            return;
        }
        float scale = Math.Max((float)size.Width / titleArt.Width, (float)size.Height / titleArt.Height);
        int w = (int)(titleArt.Width * scale), h = (int)(titleArt.Height * scale);
        var cover = new Rectangle((size.Width - w) / 2, (size.Height - h) / 2, w, h);
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        g.DrawImage(titleArt, cover);
    }

    // The backdrop, fading in from black over the first second.
    void PaintWorld(Graphics g, Size size)
    {
        if (sceneSky == null || sceneSky.Size != size) BuildScene(size);
        g.DrawImageUnscaled(sceneSky!, 0, 0);
        float a = (float)Math.Clamp((T - introAt) / 1.0, 0, 1);
        if (a < 1) using (var black = new SolidBrush(Color.FromArgb((int)(255 * (1 - a)), Color.Black))) g.FillRectangle(black, 0, 0, size.Width, size.Height);
    }

    // Text with Dusklight's soft dark glow behind it, so it reads over the scene.
    static void GlowText(Graphics g, string text, Font font, Point at, Color color, int alpha = 255, TextFormatFlags flags = TextFormatFlags.NoPadding)
    {
        TextRenderer.DrawText(g, text, font, new Point(at.X + S(1), at.Y + S(2)), Color.FromArgb(alpha * 200 / 255, 0, 0, 0), flags);
        TextRenderer.DrawText(g, text, font, at, Color.FromArgb(alpha, color), flags);
    }

    readonly Font menuFont = new("Segoe UI", 22f, FontStyle.Bold), cornerFont = new("Segoe UI", 12f, FontStyle.Bold),
        smallFont = new("Segoe UI", 10.5f), eyebrowFont = new("Segoe UI", 14f);
    readonly List<Rectangle> menuRects = new();
    Rectangle discRect;
    int hover = -1;          // the menu item under the mouse or chosen with the arrow keys
    double introAt = 1e9;    // when the window first showed: items fade and rise in one after another

    // How far item `i` is into its fade-in (0..1).
    float Intro(int i) => (float)Math.Clamp((T - introAt - 0.1 * i) / 0.35, 0, 1);

    void PaintTitle(Graphics g)
    {
        var size = view.ClientSize;
        if (size.Width <= 0 || size.Height <= 0 || openPage >= 0) return;
        PaintWorld(g, size);
        // Dusklight's darkening: deep night from the left edge to clear past the middle, plus a little along the bottom.
        using (var left = new LinearGradientBrush(new Rectangle(0, 0, (int)(size.Width * 0.55) + 1, size.Height), Color.FromArgb(215, Night), Color.FromArgb(0, Night), LinearGradientMode.Horizontal))
            g.FillRectangle(left, 0, 0, (int)(size.Width * 0.55), size.Height);
        using (var bottom = new LinearGradientBrush(new Rectangle(0, size.Height - S(150) - 2, size.Width, S(150) + 4), Color.FromArgb(0, Night), Color.FromArgb(170, Night), LinearGradientMode.Vertical))
            g.FillRectangle(bottom, 0, size.Height - S(150), size.Width, S(150));
        g.SmoothingMode = SmoothingMode.AntiAlias;

        int x = S(64), y = Math.Max(S(28), size.Height / 2 - S(300));
        // The eyebrow and the logo.
        float a0 = Intro(0);
        int rise = (int)(S(10) * (1 - a0));
        GlowText(g, "JosiahG presents", eyebrowFont, new Point(x + S(4), y + rise), Color.White, (int)(255 * a0));
        y += S(34);
        float em = Math.Min(S(58), (size.Width * 0.45f) / 6.2f);
        using (var path = new GraphicsPath())
        {
            var family = new FontFamily("Segoe UI Black") is { Name: "Segoe UI Black" } black ? black : new FontFamily("Segoe UI");
            path.AddString("NEW SUPER", family, (int)FontStyle.Italic, em, new Point(x, y + rise), StringFormat.GenericDefault);
            path.AddString("PAPER BROS.", family, (int)FontStyle.Italic, em, new Point(x, y + rise + (int)(em * 0.98f)), StringFormat.GenericDefault);
            int alpha = (int)(255 * a0);
            using (var m = new Matrix()) { m.Translate(S(4), S(6)); using var shadowPath = (GraphicsPath)path.Clone(); shadowPath.Transform(m);
                using var drop = new Pen(Color.FromArgb(alpha * 140 / 255, 0, 0, 0), S(12)) { LineJoin = LineJoin.Round }; g.DrawPath(drop, shadowPath); }
            using (var rim = new Pen(Color.FromArgb(alpha, Color.White), S(10)) { LineJoin = LineJoin.Round }) g.DrawPath(rim, path);
            var b = path.GetBounds();
            using (var fill = new LinearGradientBrush(new RectangleF(b.X, b.Y - 1, b.Width, b.Height / 2 + 2), Color.FromArgb(alpha, Red), Color.FromArgb(alpha, Yellow), LinearGradientMode.Vertical)
                   { WrapMode = WrapMode.TileFlipY })
                g.FillPath(fill, path);
            y = (int)b.Bottom + S(28);
        }

        // The menu: big words that light up with a yellow fade on hover, like Dusklight's.
        menuRects.Clear();
        int rowH = S(46), rowW = S(380);
        for (int i = 0; i < menu.Length; i++)
        {
            float a = Intro(i + 1);
            var row = new Rectangle(x - S(16), y + (int)(S(10) * (1 - a)), rowW, rowH);
            menuRects.Add(row);
            bool on = i == hover;
            if (on)
                using (var path = Rounded(row, S(10)))
                using (var glow = new LinearGradientBrush(row, Accent, Color.FromArgb(0, Accent), LinearGradientMode.Horizontal))
                    g.FillPath(glow, path);
            var label = menu[i].label.ToUpperInvariant();
            var at = new Point(row.X + S(16), row.Y + (rowH - TextRenderer.MeasureText(label, menuFont).Height) / 2);
            if (on) TextRenderer.DrawText(g, label, menuFont, at, Night, TextFormatFlags.NoPadding);
            else GlowText(g, label, menuFont, at, Color.White, (int)(255 * a));
            y += rowH + S(4);
        }

        // Bottom-left: is a disc set up (click it to open Game).
        var (ok, _, _) = DiscStatus();
        float ac = Intro(menu.Length + 1);
        int cy = size.Height - S(84);
        string id = ok ? DiscGameId() : "";
        string status = ok ? $"✔  Super Paper Mario ({(id == "R8PP01" ? "Europe" : "USA")}) is ready" : "✖  No game disc yet: choose Game to set it up";
        GlowText(g, status, cornerFont, new Point(x, cy), ok ? ReadyGreen : ErrorPink, (int)(255 * ac));
        var statusSize = TextRenderer.MeasureText(status, cornerFont);
        discRect = new Rectangle(x, cy, statusSize.Width, statusSize.Height);
        GlowText(g, "Mods: " + ModNames(), smallFont, new Point(x, cy + S(30)), Muted, (int)(255 * ac));

        // Top-right: the version, over a soft dark band so it reads on the art.
        int right = size.Width - S(28), ty = S(16);
        using (var band = new LinearGradientBrush(new Rectangle(0, -2, size.Width, S(70) + 4), Color.FromArgb((int)(150 * ac), Night), Color.FromArgb(0, Night), LinearGradientMode.Vertical))
            g.FillRectangle(band, 0, 0, size.Width, S(70));
        string ver = "v" + AppVersion + "   made by JosiahG";
        var verSize = TextRenderer.MeasureText(ver, smallFont);
        GlowText(g, ver, smallFont, new Point(right - verSize.Width, ty), Muted, (int)(255 * ac));
    }

    // The scene behind an open page: frozen and dimmed (the cards over it are transparent at their corners).
    void FreezeScene()
    {
        var size = view.ClientSize;
        if (size.Width <= 0 || size.Height <= 0) return;
        var bmp = new Bitmap(size.Width, size.Height);
        using (var g = Graphics.FromImage(bmp))
        {
            PaintWorld(g, size);
            using var dim = new SolidBrush(Color.FromArgb(150, Night));
            g.FillRectangle(dim, 0, 0, size.Width, size.Height);
        }
        view.BackgroundImage = bmp;
        frozen?.Dispose();
        frozen = bmp;
    }

    // The title screen's drawing surface: all painting comes from MainForm.PaintTitle.
    sealed class TitleView : Panel
    {
        public Action<Graphics>? Draw;
        public TitleView()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            BackgroundImageLayout = ImageLayout.None;
            BackColor = Night;
        }
        protected override void OnPaint(PaintEventArgs e) { base.OnPaint(e); Draw?.Invoke(e.Graphics); }
    }

    // A white paper card holding one page's controls, top to bottom.
    static (Panel card, FlowLayoutPanel body) NewCard(string heading)
    {
        var card = new DoubleBufferedPanel { Padding = S(22, 16, 22, 16) };
        card.Paint += (s, e) =>
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var c = (Control)s!;
            var r = c.ClientRectangle;
            r.Width -= S(8); r.Height -= S(8);
            r.Offset(S(2), S(2));
            var tile = c.Tag is Color t ? t : Sky;
            using var path = Rounded(r, S(22));
            using var shadow = Rounded(new Rectangle(r.X + S(4), r.Y + S(6), r.Width, r.Height), S(22));
            g.FillPath(new SolidBrush(Color.FromArgb(40, 30, 30, 70)), shadow);
            g.FillPath(new SolidBrush(Paper), path);
            g.DrawPath(new Pen(Mix(tile, Color.White, 0.35f), S(5)), path);
        };
        // The body sizes to its content and the card scrolls only when that is taller than the card.
        var body = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Paper };
        card.AutoScroll = true;
        body.Controls.Add(new Label { Text = heading, Font = new Font("Segoe UI", 18f, FontStyle.Bold), ForeColor = Ink, AutoSize = true, Margin = S(0, 0, 0, 10) });
        card.Controls.Add(body);
        return (card, body);
    }

    static GraphicsPath Rounded(Rectangle r, int radius)
    {
        var p = new GraphicsPath();
        int d = radius * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    static Label Note(string text, int width = 700) => new()
    {
        Text = text, AutoSize = true, MaximumSize = new Size(S(width), 0), ForeColor = Ink, Margin = S(0, 6, 0, 6),
    };

    static Button Btn(string text, EventHandler onClick, Color? back = null)
    {
        var b = new PillButton
        {
            Text = text, AutoSize = true, BackColor = back == Paper ? Mint : back ?? Yellow, ForeColor = Ink,
            Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), Padding = S(10, 4, 10, 4), Margin = S(0, 6, 10, 6), Cursor = tippiHand,
        };
        b.Click += onClick;
        return b;
    }

    // ----- Play -----
    Control BuildPlayPage()
    {
        var (card, body) = NewCard("Game");
        playStatus = Note("");
        playStatus.Font = new Font("Segoe UI", 10.5f, FontStyle.Bold);
        body.Controls.Add(playStatus);
        AddDiscSetup(body);
        // The Wii system language the game sees (runtime SCGetLanguage, Config.toml [game] language). The European
        // disc ships English, French, German, Spanish and Italian text; the USA disc is English only.
        body.Controls.Add(Choice("Language (European disc)", "language", new[] { ("English", "\"en\""), ("Français", "\"fr\""), ("Deutsch", "\"de\""), ("Español", "\"es\""), ("Italiano", "\"it\"") }, "game"));
        body.Controls.Add(gameNote = Note("The game starts borderless at 1080p. " +
                               "In the game, F10 opens the settings menu (resolution, aspect, controllers, audio) and F11 switches between fullscreen and a window."));
        var row = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        row.Controls.Add(Btn("Open game folder", (_, _) => Process.Start("explorer.exe", play), Paper));
        row.Controls.Add(Btn("Open saves folder", (_, _) => Process.Start("explorer.exe", Directory.CreateDirectory(SaveDataDir()).FullName), Paper));
        row.Controls.Add(Btn("Import Wii / Dolphin save...", (_, _) => ImportSave(), Paper));
        body.Controls.Add(row);
        return card;
    }

    // A Wii save (data.bin from SD: Wii Menu > Data Management > Save Data > Copy) or a Dolphin save folder.
    void ImportSave()
    {
        const string saveDir = @"Wii\title\00010000\52385045\data";
        var dolphin = new[] { Environment.SpecialFolder.ApplicationData, Environment.SpecialFolder.MyDocuments }
            .Select(f => Path.Combine(Environment.GetFolderPath(f), "Dolphin Emulator", saveDir)).FirstOrDefault(Directory.Exists);
        using var dialog = new OpenFileDialog
        {
            Title = "Choose a Super Paper Mario save: data.bin from a Wii's SD card, or wiimario00 in a Dolphin save folder",
            Filter = "Wii or Dolphin save (data.bin, wiimario00)|data.bin;wiimario0*|All files|*.*",
            InitialDirectory = dolphin ?? "",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        var dataDir = SaveDataDir();
        if (Directory.Exists(dataDir) && Directory.EnumerateFiles(dataDir).Any() &&
            MessageBox.Show(this, "This replaces your current save files. They are kept as a backup next to them (data.backup-<date>).\n\nImport the save?",
                "Import save", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
        try
        {
            var files = WiiSave.Import(dialog.FileName, dataDir);
            MessageBox.Show(this, "Imported " + string.Join(", ", files) + ".", "Import save", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or System.Security.Cryptography.CryptographicException)
        {
            MessageBox.Show(this, "The save could not be imported:\n" + e.Message, "Import save", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    // "--play" (tests): what PLAY does, without the window or message boxes. 0 = game started, 3 = no disc, 4 = no engine.
    public int PlayHeadless()
    {
        if (!DiscStatus().Item1) return 3;
        if (!File.Exists(gameExe)) return 4;
        return Process.Start(new ProcessStartInfo(gameExe) { WorkingDirectory = play, UseShellExecute = false }) != null ? 0 : 5;
    }

    void Launch()
    {
        var (ok, text, _) = DiscStatus();
        if (!ok)
        {
            MessageBox.Show(this, text + "\n\nChoose your disc image on the Game page first.", "New Super Paper Bros.", MessageBoxButtons.OK, MessageBoxIcon.Information);
            OpenPage(0);
            return;
        }
        var exe = gameExe;
        if (!File.Exists(exe))
        {
            MessageBox.Show(this, "The game files (the engine folder) are missing. Unzip the whole download again, keeping the engine folder next to this launcher.", "New Super Paper Bros.", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        Gamepads.Release();
        var game = Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = play, UseShellExecute = false });
        if (game != null)
        {
            game.EnableRaisingEvents = true;
            game.Exited += (_, _) => BeginInvoke(Gamepads.Resume);
        }
        else Gamepads.Resume();
        WindowState = FormWindowState.Minimized;
    }

    // ----- Display -----
    Control BuildDisplayPage()
    {
        var (card, body) = NewCard("Display");
        body.Controls.Add(Choice("Display mode", "display_mode", new[] { ("Borderless fullscreen", "\"borderless\""), ("Windowed", "\"windowed\""), ("Exclusive fullscreen", "\"exclusive\"") }));
        body.Controls.Add(Choice("Resolution", "resolution_multiplier", new[] { ("1080p", "2.25"), ("4K (2160p)", "4.5"), ("Match the screen", "0"), ("1440p", "3"), ("720p", "1.5"), ("480p (original)", "1") }));
        body.Controls.Add(Choice("Aspect ratio", "aspect_mode", new[] { ("Cinematic 2.39:1", "3"), ("Fill the screen", "2"), ("16:9", "1"), ("Original 4:3", "0") }));
        body.Controls.Add(Choice("Frame interpolation", "frame_interpolation_fps", new[] { ("120 FPS", "120"), ("Off (60 FPS)", "0") }));
        body.Controls.Add(Choice("Vsync", "vsync", new[] { ("On (no screen tearing)", "true"), ("Off", "false") }));
        body.Controls.Add(Note("Changes are saved at once and used the next time the game starts. The same settings are in the game's F10 menu. " +
                               "Cinematic and Fill widen the game's camera (more of the level is visible); 16:9 keeps the Wii picture with bars on wider screens. " +
                               "Frame interpolation inserts frames between the game's 60 fps; turn it off if you see artifacts."));
        return card;
    }

    Control Choice(string label, string key, (string label, string value)[] items, string section = "video")
    {
        var row = new FlowLayoutPanel { AutoSize = true, Margin = S(0, 4, 0, 4) };
        row.Controls.Add(new Label { Text = label, AutoSize = false, Size = S(200, 32), ForeColor = Ink, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft });
        var combo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = S(260), FlatStyle = FlatStyle.Flat, Font = new Font("Segoe UI", 10.5f), Margin = S(3, 4, 3, 4) };
        foreach (var item in items) combo.Items.Add(item.label);
        string current = config.Get(section, key) ?? "";
        int index = Array.FindIndex(items, i => i.value.Trim('"') == current || (double.TryParse(i.value, out var a) && double.TryParse(current, out var b) && a == b));
        combo.SelectedIndex = index < 0 ? 0 : index;
        combo.SelectedIndexChanged += (_, _) => { config.Set(section, key, items[combo.SelectedIndex].value); config.Save(); };
        row.Controls.Add(combo);
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

    Control BuildControlsPage()
    {
        var (card, body) = NewCard("Controls");
        var inputRow = new FlowLayoutPanel { AutoSize = true, Margin = S(0, 0, 0, 6) };
        inputRow.Controls.Add(new Label { Text = "Input", AutoSize = false, Size = S(110, 32), ForeColor = Ink, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft });
        var inputCombo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = S(460), FlatStyle = FlatStyle.Flat, Margin = S(3, 4, 3, 4) };
        inputRow.Controls.Add(inputCombo);
        body.Controls.Add(inputRow);
        var gamepad = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = Padding.Empty };
        var keyboard = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = Padding.Empty };
        body.Controls.Add(gamepad);
        body.Controls.Add(keyboard);

        // Gamepad: one button per action; click it, then press the controller input to bind (Escape or a click cancels).
        int style = 0;
        uint padId = 0;
        var padNote = Note("This controller controls Mario and is set up automatically; the left stick moves the Wii Remote's D-pad. " +
                           "To change an action, click it and press the button on your controller (Escape cancels).");
        gamepad.Controls.Add(padNote);
        var padTable = new TableLayoutPanel { AutoSize = true, ColumnCount = 4, Margin = S(0, 4, 0, 4) };
        gamepad.Controls.Add(padTable);
        Button? capturingPad = null;
        var heldAtStart = new HashSet<string>();
        var padTimer = new System.Windows.Forms.Timer { Interval = 30 };
        void FillPadTable()
        {
            padTable.SuspendLayout();
            padTable.Controls.Clear();
            for (int i = 0; i < Actions.Length; i++)
            {
                string key = PadStyles[style] + "_" + Actions[i].key;
                string label = i < PadActionLabels.Length ? PadActionLabels[i] : Actions[i].label;
                padTable.Controls.Add(new Label { Text = label, AutoSize = true, ForeColor = Ink, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), Margin = S(0, 12, 12, 0) }, (i / 6) * 2, i % 6);
                string? custom = config.Get("gamepad", key);
                var b = Btn(PadLabels(style, custom ?? PadDefaults[i][style]), null!, custom == null ? Paper : ColorTranslator.FromHtml("#FFF0B0"));
                b.AutoSize = false;
                b.Size = S(240, 40);
                b.Margin = S(0, 4, 18, 4);
                b.Tag = key;
                b.Click += (_, _) =>
                {
                    if (capturingPad != null) { EndPadCapture(null); return; }
                    capturingPad = b;
                    b.Text = "Press a button...";
                    b.BackColor = Yellow;
                    heldAtStart = Gamepads.Active(padId);
                    padTimer.Start();
                };
                padTable.Controls.Add(b, (i / 6) * 2 + 1, i % 6);
            }
            padTable.ResumeLayout();
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
        gamepad.Controls.Add(Btn("Reset to automatic", (_, _) =>
        {
            EndPadCapture(null);
            foreach (var (key, _, _) in Actions) config.Remove("gamepad", PadStyles[style] + "_" + key);
            config.Save();
            FillPadTable();
        }, Paper));

        // Keyboard: a sideways Wii Remote.
        keyboard.Controls.Add(Note("The keyboard works as a sideways Wii Remote. Click a key to change it, then press the new key (Escape cancels, Delete clears)."));
        var table = new TableLayoutPanel { AutoSize = true, ColumnCount = 4, Margin = S(0, 4, 0, 4) };
        var buttons = new List<Button>();
        for (int i = 0; i < Actions.Length; i++)
        {
            var (key, label, defaults) = Actions[i];
            table.Controls.Add(new Label { Text = label, AutoSize = true, ForeColor = Ink, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), Margin = S(0, 12, 12, 0) }, (i / 6) * 2, i % 6);
            var b = Btn(KeyText(config.Get("keyboard", key) ?? defaults), null!, Paper);
            b.MinimumSize = S(170, 0);
            b.Margin = S(0, 4, 24, 4);
            b.Tag = key;
            b.Click += (s, _) => BeginCapture((Button)s!);
            buttons.Add(b);
            table.Controls.Add(b, (i / 6) * 2 + 1, i % 6);
        }
        keyboard.Controls.Add(table);
        keyboard.Controls.Add(Btn("Reset to defaults", (_, _) =>
        {
            foreach (var (key, _, defaults) in Actions) config.Set("keyboard", key, "\"" + defaults + "\"");
            config.Save();
            foreach (var b in buttons) b.Text = KeyText(Actions.First(a => a.key == (string)b.Tag!).defaults);
        }, Paper));

        // The input list: connected pads first (the first one is selected: it is the main controller), then the keyboard.
        var shown = new List<(uint id, string name, int style)>();
        void ShowSelected()
        {
            int i = inputCombo.SelectedIndex;
            bool isPad = i >= 0 && i < shown.Count;
            gamepad.Visible = isPad;
            keyboard.Visible = !isPad;
            if (isPad) { EndPadCapture(null); style = shown[i].style; padId = shown[i].id; FillPadTable(); }
        }
        void RefreshInputs()
        {
            var pads = Gamepads.Connected();
            if (pads.Count == 0 && screenshotDir != null) pads.Add((0, "Test controller (screenshot mode)", 0));
            if (pads.SequenceEqual(shown) && inputCombo.Items.Count > 0) return;
            uint? selectedId = inputCombo.SelectedIndex >= 0 && inputCombo.SelectedIndex < shown.Count ? shown[inputCombo.SelectedIndex].id : null;
            bool keyboardSelected = inputCombo.Items.Count > 0 && inputCombo.SelectedIndex == shown.Count;
            shown.Clear();
            shown.AddRange(pads);
            inputCombo.Items.Clear();
            foreach (var pad in pads) inputCombo.Items.Add($"{pad.name}  ({new[] { "Xbox", "PlayStation", "Nintendo" }[pad.style]} layout)");
            inputCombo.Items.Add(pads.Count == 0 ? "Keyboard  (no controller connected - plug one in and it appears here)" : "Keyboard");
            int keep = selectedId is uint id ? shown.FindIndex(p => p.id == id) : -1;
            inputCombo.SelectedIndex = keyboardSelected ? shown.Count : keep >= 0 ? keep : 0;
            ShowSelected();
        }
        inputCombo.SelectedIndexChanged += (_, _) => ShowSelected();
        RefreshInputs();
        var poll = new System.Windows.Forms.Timer { Interval = 1000 };
        poll.Tick += (_, _) => { if (card.Visible) RefreshInputs(); };
        poll.Start();
        return card;
    }

    static string KeyText(string keys) => keys.Length == 0 ? "None" : keys.Replace(",", " / ");

    void BeginCapture(Button button)
    {
        if (capturing != null) capturing.Text = captureKey!;
        capturing = button;
        captureKey = button.Text;
        button.Text = "Press a key...";
        button.BackColor = Yellow;
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (capturing == null)
        {
            if (e.KeyCode == Keys.Escape && cancelPadCapture?.Invoke() == true) { e.Handled = true; return; }
            if (openPage >= 0 && (e.KeyCode == Keys.Escape || e.KeyCode == Keys.Back) && ActiveControl is not TextBox) { ClosePage(); e.Handled = true; return; }
            // The title screen's menu: arrow keys move, Enter or Space picks.
            if (openPage < 0 && e.KeyCode is Keys.Up or Keys.Down)
            {
                hover = hover < 0 ? 0 : (hover + (e.KeyCode == Keys.Down ? 1 : menu.Length - 1)) % menu.Length;
                view.Invalidate();
                e.Handled = true;
                return;
            }
            if (openPage < 0 && hover >= 0 && e.KeyCode is Keys.Enter or Keys.Space) { menu[hover].act(); e.Handled = true; return; }
            base.OnKeyDown(e);
            return;
        }
        e.SuppressKeyPress = true;
        e.Handled = true;
        string? name = e.KeyCode == Keys.Escape ? null : e.KeyCode == Keys.Delete ? "" : SdlName(e.KeyCode);
        if (name == null && e.KeyCode != Keys.Escape) return;  // a key SDL has no name for here: keep waiting
        if (name != null)
        {
            config.Set("keyboard", (string)capturing.Tag!, "\"" + name + "\"");
            config.Save();
            captureKey = KeyText(name);
        }
        capturing.Text = captureKey!;
        capturing.BackColor = Mint;
        capturing = null;
    }

    // SDL scancode names, as Keybinds::Parse (SDL_GetScancodeFromName) reads them from Config.toml.
    static string? SdlName(Keys k) => k switch
    {
        >= Keys.A and <= Keys.Z => k.ToString(),
        >= Keys.D0 and <= Keys.D9 => ((char)('0' + (k - Keys.D0))).ToString(),
        >= Keys.NumPad0 and <= Keys.NumPad9 => "Keypad " + (k - Keys.NumPad0),
        >= Keys.F1 and <= Keys.F12 => k.ToString(),
        Keys.Up => "Up", Keys.Down => "Down", Keys.Left => "Left", Keys.Right => "Right",
        Keys.Space => "Space", Keys.Return => "Return", Keys.Tab => "Tab", Keys.Back => "Backspace", Keys.Insert => "Insert",
        Keys.Home => "Home", Keys.End => "End", Keys.PageUp => "PageUp", Keys.PageDown => "PageDown", Keys.CapsLock => "CapsLock",
        Keys.ShiftKey => "Left Shift", Keys.ControlKey => "Left Ctrl", Keys.Menu => "Left Alt",
        Keys.OemMinus => "-", Keys.Oemplus => "=", Keys.OemOpenBrackets => "[", Keys.OemCloseBrackets => "]", Keys.Oemcomma => ",",
        Keys.OemPeriod => ".", Keys.OemQuestion => "/", Keys.OemSemicolon => ";", Keys.OemQuotes => "'", Keys.Oemtilde => "`",
        _ => null,
    };

    // ----- Game disc (part of the Play page) -----
    // One button sets the disc up (or changes it later); extraction progress shows under it, and the dump
    // instructions only while no disc is set up.
    void AddDiscSetup(FlowLayoutPanel body)
    {
        var progress = new ProgressBar { Size = S(640, 22), Margin = S(0, 6, 0, 2), Visible = false };
        var progressText = Note("");
        progressText.Visible = false;
        progressText.TextChanged += (_, _) => progressText.Visible = progressText.Text.Length > 0;  // no empty gap
        var choose = chooseDisc = Btn("Choose disc image (.iso, .ciso, .wbfs)...", null!);
        choose.AutoSizeMode = AutoSizeMode.GrowAndShrink;  // "Change disc..." is shorter
        choose.Click += async (_, _) =>
        {
            using var dialog = new OpenFileDialog { Title = "Choose your Super Paper Mario disc image", Filter = "Wii disc images (*.iso;*.ciso;*.wbfs)|*.iso;*.ciso;*.wbfs|All files|*.*" };
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            WiiDisc disc;
            try { disc = WiiDisc.Open(dialog.FileName); }
            catch (Exception ex) { progressText.Text = "Could not read that image: " + ex.Message; return; }
            using (disc)
            {
                if (disc.Problem() is { Length: > 0 } problem) { progressText.Text = problem; return; }
                choose.Enabled = false;
                progress.Visible = true;
                var staging = discDir + ".new";
                try
                {
                    if (Directory.Exists(staging)) Directory.Delete(staging, true);
                    var report = new Progress<(int done, int total, string name)>(p =>
                    {
                        progress.Maximum = p.total;
                        progress.Value = Math.Min(p.done, p.total);
                        progressText.Text = $"Extracting {p.done}/{p.total}  {p.name}";
                    });
                    await Task.Run(() => disc.Extract(staging, (d, t, n) => ((IProgress<(int, int, string)>)report).Report((d, t, n))));
                    if (disc.Holes > 0) throw new InvalidDataException("the image is incomplete (blocks missing); dump the disc again.");
                    if (Directory.Exists(discDir)) Directory.Delete(discDir, true);
                    Directory.Move(staging, discDir);
                    config.SetString("paths", "dvd_root", "../../Disc/DATA");  // relative to <game folder>\UserData (one level below the root in both layouts), so the folder can move
                    config.Save();
                    progressText.Text = "Done. The disc is extracted and ready.";
                }
                catch (Exception ex)
                {
                    progressText.Text = "Extraction failed: " + ex.Message;
                }
                finally
                {
                    choose.Enabled = true;
                    progress.Visible = false;
                    RefreshStatus();
                }
            }
        };
        body.Controls.Add(choose);
        body.Controls.Add(progress);
        body.Controls.Add(progressText);
        body.Controls.Add(dumpHelp = Note(
            "This port runs your own copy of Super Paper Mario (USA R8PE01 or Europe R8PP01). It needs a disc image of the disc you own, " +
            "which the launcher decrypts and extracts once into its Disc folder (about 400 MB).\n\n" +
            "Making a legal image of your disc (Wii or Wii U consoles can read the disc; PC drives can't):\n" +
            "1. On a Wii with the Homebrew Channel, run CleanRip (or USB Loader GX's install option).\n" +
            "2. Insert Super Paper Mario and dump it to an SD card or USB drive. CleanRip writes a .iso " +
            "(it may be split into .part files: join them with  copy /b  on the PC); USB Loader GX writes a .wbfs.\n" +
            "3. Copy the file to this PC and choose it with the button above. .iso, .ciso and .wbfs all work.\n" +
            "The USA release (revision 0) and the European release work; the European disc also offers French, German, Spanish and Italian. " +
            "A Dolphin-extracted folder also works: put it in Disc\\DATA next to the launcher."));
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
        playStatus.ForeColor = ok ? Ink : Red;
        chooseDisc.Text = ok ? "Change disc..." : "Choose disc image (.iso, .ciso, .wbfs)...";
        chooseDisc.BackColor = ok ? Mint : Yellow;
        dumpHelp.Visible = !ok;
        gameNote.Visible = ok;
        view.Invalidate();
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
        body.Controls.Add(Note("Install a mod from its .zip or its folder. Ticked mods load when the game starts; untick one to turn it off. " +
                               "A mod holds a files folder (it replaces game files) and/or a textures folder."));
        var row = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        row.Controls.Add(Btn("Install mod (.zip)...", (_, _) =>
        {
            using var dialog = new OpenFileDialog { Title = "Choose a mod (.zip)", Filter = "Mod archive (*.zip)|*.zip" };
            if (dialog.ShowDialog(this) == DialogResult.OK) InstallMod(dialog.FileName);
        }));
        row.Controls.Add(Btn("Install mod folder...", (_, _) =>
        {
            using var dialog = new FolderBrowserDialog { Description = "Choose the mod's folder (the one holding files or textures)", UseDescriptionForTitle = true };
            if (dialog.ShowDialog(this) == DialogResult.OK) InstallMod(dialog.SelectedPath);
        }, Paper));
        row.Controls.Add(Btn("Remove selected", (_, _) => RemoveSelectedMod(), Paper));
        row.Controls.Add(Btn("Open mods folder", (_, _) => Process.Start("explorer.exe", Directory.CreateDirectory(modsDir).FullName), Paper));
        body.Controls.Add(row);
        modList = new ListView { View = View.Details, CheckBoxes = true, Size = S(660, 240), FullRowSelect = true, Font = new Font("Segoe UI", 10.5f), Margin = S(0, 6, 0, 6), HeaderStyle = ColumnHeaderStyle.Nonclickable };
        modList.Columns.Add("Installed mods", S(630));
        modList.ItemCheck += (_, e) =>
        {
            var dir = (string)modList.Items[e.Index].Tag!;
            var marker = Path.Combine(dir, "disabled");
            if (e.NewValue == CheckState.Checked) File.Delete(marker); else File.WriteAllText(marker, "");
            BeginInvoke(RefreshStatus);
        };
        body.Controls.Add(modList);
        LoadMods();
        return card;
    }

    // Installs a mod from a .zip or a folder into mods\<name>. The mod's root is the folder holding files\ or textures\
    // (zips often wrap it in one more folder). Zip entries that would land outside the target are refused.
    void InstallMod(string source)
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
            if (modRoot == null)
            {
                MessageBox.Show(this, "This doesn't look like a New Super Paper Bros. mod: it has no files or textures folder.", "Install mod", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            string name = ModInstall.SafeName(modRoot == temp ? Path.GetFileNameWithoutExtension(source) : Path.GetFileName(modRoot));
            var dest = Path.Combine(modsDir, name);
            if (Directory.Exists(dest))
            {
                if (MessageBox.Show(this, $"A mod called \"{name}\" is already installed. Replace it?", "Install mod", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
                Directory.Delete(dest, true);
            }
            ModInstall.CopyDir(modRoot, dest);
            LoadMods();
            RefreshStatus();
            MessageBox.Show(this, $"Installed \"{name}\". It loads the next time the game starts.", "Install mod", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            MessageBox.Show(this, "The mod could not be installed:\n" + e.Message, "Install mod", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        finally
        {
            if (temp != null) try { Directory.Delete(temp, true); } catch (Exception) { }
        }
    }

    void RemoveSelectedMod()
    {
        if (modList.SelectedItems.Count == 0) { MessageBox.Show(this, "Click a mod in the list first.", "Remove mod", MessageBoxButtons.OK, MessageBoxIcon.Information); return; }
        var item = modList.SelectedItems[0];
        if (MessageBox.Show(this, $"Remove \"{item.Text}\"? Its folder is deleted.", "Remove mod", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
        try { Directory.Delete((string)item.Tag!, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { MessageBox.Show(this, "The mod could not be removed:\n" + e.Message, "Remove mod", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
        LoadMods();
        RefreshStatus();
    }

    void LoadMods()
    {
        modList.Items.Clear();
        if (!Directory.Exists(modsDir)) return;
        foreach (var dir in Directory.GetDirectories(modsDir).OrderBy(d => d, StringComparer.Ordinal))
            modList.Items.Add(new ListViewItem(Path.GetFileName(dir)) { Tag = dir, Checked = !File.Exists(Path.Combine(dir, "disabled")) });
    }

    // Tomodachi Life's buttons: a bubbly pill with a light top, a darker lip underneath and dark text on light colours.
    sealed class PillButton : Button
    {
        bool hover, down;
        public PillButton()
        {
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.SupportsTransparentBackColor, true);
            MouseEnter += (_, _) => { hover = true; Invalidate(); };
            MouseLeave += (_, _) => { hover = down = false; Invalidate(); };
            MouseDown += (_, _) => { down = true; Invalidate(); };
            MouseUp += (_, _) => { down = false; Invalidate(); };
        }
        public override Size GetPreferredSize(Size proposed)
        {
            var text = TextRenderer.MeasureText(Text, Font);
            return new Size(text.Width + Padding.Horizontal + S(28), text.Height + Padding.Vertical + S(16));
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(Parent?.BackColor ?? Paper);
            if (Parent?.BackgroundImage is { } scene) g.DrawImageUnscaled(scene, -Left, -Top);  // over the title screen's scene
            g.SmoothingMode = SmoothingMode.AntiAlias;
            int lip = S(4), press = down ? lip - S(1) : 0;
            var r = new Rectangle(S(1), S(1) + press, Width - S(3), Height - lip - S(3));
            var face = hover ? Mix(BackColor, Color.White, 0.18f) : BackColor;
            using (var under = Rounded(new Rectangle(r.X, r.Y + lip - press, r.Width, r.Height), r.Height / 2)) g.FillPath(new SolidBrush(Mix(BackColor, Ink, 0.35f)), under);
            using var path = Rounded(r, r.Height / 2);
            using (var brush = new LinearGradientBrush(r, Mix(face, Color.White, 0.35f), face, LinearGradientMode.Vertical)) g.FillPath(brush, path);
            using (var edge = new Pen(Mix(BackColor, Ink, 0.25f), S(2))) g.DrawPath(edge, path);
            bool light = BackColor.R * 0.299f + BackColor.G * 0.587f + BackColor.B * 0.114f > 170;  // perceived brightness
            TextRenderer.DrawText(g, Text, Font, r, light ? Ink : Color.White, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }
    }

    sealed class DoubleBufferedPanel : Panel
    {
        public DoubleBufferedPanel() { DoubleBuffered = true; ResizeRedraw = true; }
    }
}
