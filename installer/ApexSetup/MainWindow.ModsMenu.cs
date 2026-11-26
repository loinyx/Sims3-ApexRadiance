using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using static ApexSetup.Strings;

namespace ApexSetup;

// The Mods screen's Tools menu and its folders, opening a package in s3pe, and a mod's page on the web.
public sealed partial class MainWindow
{
    // ---------- Tools ----------

    void OnModsTools(object sender, RoutedEventArgs e)
    {
        if (_game is not { } g) return;
        var (flyout, rows) = RowsFlyout(318, FlyoutPlacementMode.BottomEdgeAlignedRight);
        void Tool(string icon, string title, string help, Action run, bool enabled = true) =>
            rows.Children.Add(FlyoutRow(Lucide.Icon(icon, 18), title, help, () => { flyout.Hide(); run(); }, enabled));
        var chosen = Picked().Where(Mine).ToList();
        Tool("merge", T("Merge mods"), chosen.Count > 1 ? T("The {0} selected, into one package", chosen.Count) : T("Select two or more mods first"), () => Safe(MergeAsync(chosen, null, null)), chosen.Count > 1 && !_busy);
        Tool("scan-search", T("Find custom content"), T("See which mods a file uses and what is missing"), () => Safe(FindContentAsync()));
        Tool("list-ordered", T("Load order"), T("Which mod the game reads first"), () => Safe(LoadOrderPageAsync()));
        Tool("bug", T("Find a problem mod"), T("Tests half of your mods at a time"), () => OnModsHunt(sender, e), _modsLoaded && !_busy && ModHunt.Current() is null);
        Tool("brush", T("Clear game caches"), T("For items that are missing or look wrong"), () => Safe(ChooseCachesAsync()));
        if (Picked() is [var one] && CanOpenInS3pe(one)) Tool("package", T("Open in s3pe"), one.Name, () => Safe(OpenInS3peAsync(one)));

        rows.Children.Add(FlyoutLine());
        rows.Children.Add(FlyoutNote(T("Open folder")));
        var grid = new Grid { ColumnSpacing = 4, RowSpacing = 4, Margin = new Thickness(4, 0, 4, 4) };
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        var reasons = new List<string>();
        void Folder(int at, string label, string path, string? why, bool wide = false)
        {
            if (at % 2 == 0) grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var b = Dims(new Button { Content = new TextBlock { Text = label, TextTrimming = TextTrimming.CharacterEllipsis }, MinHeight = 40, HorizontalAlignment = HorizontalAlignment.Stretch, CornerRadius = new CornerRadius(6), IsEnabled = why is null });
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, T("Open the folder {0}", label));
            if (why is not null) { Microsoft.UI.Xaml.Automation.AutomationProperties.SetHelpText(b, why); if (!reasons.Contains(why)) reasons.Add(why); }
            else ToolTipService.SetToolTip(b, path);
            b.Click += (_, _) => { flyout.Hide(); OpenModFolder(path); };
            Grid.SetRow(b, at / 2);
            Grid.SetColumn(b, at % 2);
            if (wide) Grid.SetColumnSpan(b, 2);
            grid.Children.Add(b);
        }
        var docs = Docs;
        var worlds = Path.Combine(g.Root, "GameData", "Shared", "NonPackaged", "Worlds");
        // (the game makes Saved Sims and Library itself in its Documents folder; Mods is the player's, Worlds the game's own)
        string? Own(string name) => Directory.Exists(docs) || Directory.Exists(Path.Combine(docs, name)) ? null : T("The game makes these folders the first time it starts");
        var mods = Path.Combine(docs, "Mods");
        Folder(0, "Mods", mods, Directory.Exists(mods) ? null : T("The Mods folder is made when you add a mod"));
        Folder(1, T("Saved Sims"), Path.Combine(docs, "SavedSims"), Own("SavedSims"));
        Folder(2, T("Library"), Path.Combine(docs, "Library"), Own("Library"));
        Folder(3, T("Worlds"), worlds, Directory.Exists(worlds) ? null : T("The game's Worlds folder was not found"));
        // the place on screen, when it is a folder of its own
        if (_modPlace is AsiMods or OffMods || IsFolder(_modPlace) && _modPlace.Length > 0)
        {
            var here = _modPlace switch { AsiMods => g.Bin, OffMods => Mods.Disabled(docs), _ => Path.Combine(mods, _modPlace) };
            if (Directory.Exists(here)) Folder(4, PlaceTitle(_modPlace).Title, here, null, wide: true);
        }
        rows.Children.Add(grid);
        foreach (var r in reasons) rows.Children.Add(FlyoutNote(r));
        flyout.ShowAt(ModsTools);
    }

    void OpenModFolder(string path)
    {
        try
        {
            if (!Directory.Exists(path)) { Engine.CheckWritable(); Directory.CreateDirectory(path); }
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true, Verb = "open" });
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The folder did not open"), ErrorText(e)); }
    }

    // ---------- s3pe ----------

    // the player's own packages only: a file the launcher installed no longer checks out once it is edited
    static bool CanOpenInS3pe(ModRow r) => Mine(r) && S3pe.Find() is not null;

    async Task OpenInS3peAsync(ModRow r)
    {
        if (S3pe.Find() is not { } exe) return;
        // (a mod inside a merge is never opened: its kept copy has to stay the same, byte for byte, to be put back)
        if (ModMerge.IsMerged(r.File) && !await Confirm(T("Open a merge in s3pe?"), T("Changes saved in this file are lost when the merge is edited or split. To change a mod inside it, take the mod out first."), T("Open"))) return;
        try { S3pe.Open(exe, r.File.Path); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("s3pe did not open"), ErrorText(e)); }
    }

    // ---------- a mod's page ----------

    UIElement LinkSection(ModRow r)
    {
        var link = ModNotes.LinkOf(r.File.Rel);
        var s = Section(T("Mod page"), link is null ? T("Add") : null, out var add);
        if (add is not null)
        {
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(add, T("Add the mod's page"));
            add.Click += (_, _) => Safe(EditLinkAsync(r));
        }
        if (link is null) return s;
        var g = new Grid { ColumnSpacing = 10 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var icon = Lucide.Icon("globe", 16, Res("ApexMutedBrush"));
        icon.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(icon);
        var u = new Uri(link);
        var text = new TextBlock { Text = (u.IsDefaultPort ? u.Host : u.Authority) + u.PathAndQuery.TrimEnd('/'), FontSize = 13, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis, Foreground = Res("ApexSoftTextBrush") };
        ToolTipService.SetToolTip(text, link);
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        var open = Button(T("Open"), () => OpenLink(link));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(open, T("Open the mod's page"));
        Grid.SetColumn(open, 2);
        g.Children.Add(open);
        var more = IconButton("ellipsis", T("Options for the mod's page"));
        var menu = new MenuFlyout();
        menu.Items.Add(MenuItem(T("Copy link"), () => CopyText(link), "copy"));
        menu.Items.Add(MenuItem(T("Edit link"), () => Safe(EditLinkAsync(r)), "pencil"));
        menu.Items.Add(MenuItem(T("Remove link"), () => SetLink(r, null, link), "trash-2"));
        more.Click += (_, _) => menu.ShowAt(more);
        Grid.SetColumn(more, 3);
        g.Children.Add(more);
        s.Children.Add(PaneCard(g, new Thickness(12, 6, 6, 6)));
        return s;
    }

    void OpenLink(string link)
    {
        if (ModNotes.WebLink(link) is not { } url) return;
        try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true })?.Dispose(); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The page did not open"), ErrorText(e)); }
    }

    async Task EditLinkAsync(ModRow r)
    {
        var was = ModNotes.LinkOf(r.File.Rel);
        var body = new StackPanel { Spacing = 8, Width = Design.Dialogs.Small };
        var box = new TextBox { Header = T("Address"), Text = was ?? "", PlaceholderText = "https://", MaxLength = 2000 };
        box.Loaded += (_, _) => { box.Focus(FocusState.Programmatic); box.SelectAll(); };
        body.Children.Add(box);
        var wrong = Design.Parts.FieldError(T("Use a web address, such as https://example.com."));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetLiveSetting(wrong, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        body.Children.Add(wrong);
        var d = new ContentDialog { Tag = "link", Title = was is null ? T("Add the mod's page") : T("Edit link"), Content = body, PrimaryButtonText = T("Save"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        void Check()
        {
            var ok = ModNotes.WebLink(box.Text) is not null;
            wrong.Visibility = ok || box.Text.Trim().Length == 0 ? Visibility.Collapsed : Visibility.Visible;
            d.IsPrimaryButtonEnabled = ok;
        }
        box.TextChanged += (_, _) => Check();
        Check();
        if (await ShowDialog(d) == ContentDialogResult.Primary) SetLink(r, box.Text, was);
    }

    void SetLink(ModRow r, string? link, string? was)
    {
        try
        {
            ModNotes.SetLink(r.File.Rel, link);
            if (link is null) ModsResult(T("Link removed"), () => SetLink(r, was, null));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        FillModDetails();
    }
}
