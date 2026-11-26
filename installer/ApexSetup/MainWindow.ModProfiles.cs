using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using static ApexSetup.Strings;

namespace ApexSetup;

// The profiles at the top of the Mods screen's side pane: which mods are on, under a name (ModProfiles). The button shows
// the one in use; its flyout switches, saves, renames, copies, updates and deletes them. A switch turns mods on and off
// as the list's switches do, every one or none, and can be undone.
public sealed partial class MainWindow
{
    // what the profile button says (the profile in use is compared with the mods as they are, so each refresh asks once)
    string ProfileName() => ModProfiles.Active() ?? T("None chosen");

    void FillProfileButton()
    {
        var name = ProfileName();
        // (a hunt turns mods off and on by itself: nothing has changed for the player)
        var changed = ModHunt.Current() is null && ActiveProfile() is { } p && !ModProfiles.PlanFor(p, _mods, Docs).Same;
        var label = changed ? T("Profile") + " · " + T("changed") : T("Profile");
        var g = new Grid { ColumnSpacing = 12 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(Design.IconTile.Of("layers"));
        var text = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = label, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
        text.Children.Add(new TextBlock { Text = name, FontSize = 14, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis });
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        var arrow = Lucide.Icon("chevron-down", 14, Res("ApexMutedBrush"));
        arrow.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(arrow, 2);
        g.Children.Add(arrow);
        ModsProfile.Content = g;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(ModsProfile, $"{label}: {name}");
        ToolTipService.SetToolTip(ModsProfile, changed ? T("{0}. Some mods were turned on or off since.", name) : name);
    }

    static ModProfile? ActiveProfile() => ModProfiles.Active() is { } name ? ModProfiles.All().FirstOrDefault(p => p.Name == name) : null;

    void OnModsProfile(object sender, RoutedEventArgs e) => ShowProfiles(ModsProfile);

    // a list that opens from a button of this screen: rows of an icon, a title and a line of help, walked with the arrows too
    static (Flyout Flyout, StackPanel Rows) RowsFlyout(double width, FlyoutPlacementMode placement)
    {
        var rows = new StackPanel { Width = width, Spacing = 2, XYFocusKeyboardNavigation = XYFocusKeyboardNavigationMode.Enabled };
        return (new Flyout { Content = Design.Parts.PopCard(rows, 6), Placement = placement, FlyoutPresenterStyle = Design.Parts.Pop(width + 14) }, rows);
    }

    static Button FlyoutRow(FrameworkElement icon, string title, string? help, Action click, bool enabled = true)
    {
        var g = new Grid { ColumnSpacing = 14 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(18) });
        g.ColumnDefinitions.Add(new ColumnDefinition());
        icon.VerticalAlignment = VerticalAlignment.Center;
        icon.HorizontalAlignment = HorizontalAlignment.Center;
        g.Children.Add(icon);
        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        if (help is not null) text.Children.Add(new TextBlock { Text = help, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        var b = Dims(new Button
        {
            Content = g, MinHeight = help is null ? 44 : 52, Padding = new Thickness(10, 6, 10, 6), CornerRadius = new CornerRadius(6), IsEnabled = enabled,
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Background = Res("ApexClearBrush"), BorderThickness = new Thickness(0),
        });
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, title);
        if (help is not null) Microsoft.UI.Xaml.Automation.AutomationProperties.SetHelpText(b, help);
        b.Click += (_, _) => click();
        return b;
    }

    static TextBlock FlyoutNote(string text) => new() { Text = text, FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(10, 4, 10, 4) };
    static Border FlyoutLine() => new() { Height = 1, Background = Res("ApexCardBorderBrush"), Margin = new Thickness(4, 6, 4, 6) };

    void ShowProfiles(FrameworkElement anchor)
    {
        if (_game is null || !_modsLoaded) return;
        var (flyout, rows) = RowsFlyout(308, FlyoutPlacementMode.BottomEdgeAlignedLeft);
        var hunting = ModHunt.Current() is not null;
        var active = ModProfiles.Active();
        var list = new StackPanel { Spacing = 2 };
        foreach (var p in ModProfiles.All())
        {
            var name = p.Name;
            var plan = ModProfiles.PlanFor(p, _mods, Docs);
            var mine = name == active;
            var info = T("{0} on", plan.On) + (mine && !hunting && !plan.Same ? " · " + T("changed") : "");
            var row = new Grid { ColumnSpacing = 2 };
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var check = Lucide.Icon("check", 16, Res("ApexAccentBrush"), 2.6);
            if (!mine) check.Opacity = 0;
            // (the one in use too: it puts back what was changed since)
            var pick = FlyoutRow(check, name, info, () => { flyout.Hide(); Safe(SwitchProfileAsync(name)); }, !hunting);
            if (mine) pick.Background = Res("ApexPillBrush");
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(pick, mine ? T("{0}, in use", name) : name);
            row.Children.Add(pick);
            var more = IconButton("ellipsis", T("Options for the profile {0}", name));
            more.Width = more.Height = 44;
            var menu = new MenuFlyout();
            void Item(string text, string icon, Func<Task> run, bool enabled = true, bool danger = false)
            {
                var i = MenuItem(text, () => { flyout.Hide(); Safe(run()); }, icon, danger: danger);
                i.IsEnabled = enabled;
                menu.Items.Add(i);
            }
            Item(T("Rename"), "pencil", () => RenameProfileAsync(name));
            Item(T("Make a copy"), "copy", () => ProfileChange(() => ModProfiles.Duplicate(name), T("Profile copied")));
            Item(T("Update with the mods that are on now"), "power", () => UpdateProfileAsync(name), !hunting);
            menu.Items.Add(new MenuFlyoutSeparator());
            Item(T("Delete"), "trash-2", () => DeleteProfileAsync(name), danger: true);
            more.Click += (_, _) => menu.ShowAt(more);
            Grid.SetColumn(more, 1);
            row.Children.Add(more);
            list.Children.Add(row);
        }
        if (list.Children.Count > 0)
        {
            rows.Children.Add(new ScrollViewer { Content = list, MaxHeight = 360 });
            rows.Children.Add(FlyoutLine());
        }
        rows.Children.Add(FlyoutRow(Lucide.Icon("plus"), T("Save the mods that are on as a profile"), null, () => { flyout.Hide(); Safe(NewProfileAsync()); }, !hunting));
        rows.Children.Add(FlyoutNote(hunting ? T("Finish finding the problem mod first.") : T("Switching a profile only turns mods on and off. No file is deleted.")));
        if (!hunting && list.Children.Count > 0) rows.Children.Add(FlyoutNote(T("Mods added after a profile was saved stay as they are.")));
        flyout.ShowAt(anchor);
    }

    // a profile's name, refused while another profile has it; null when cancelled
    async Task<string?> AskProfileName(string title, string? text, string initial, string yes)
    {
        var body = new StackPanel { Spacing = 16, Width = Design.Dialogs.Small };
        if (text is not null) body.Children.Add(Body(text));
        var box = new TextBox { Header = T("Profile name"), Text = initial, MaxLength = ModProfiles.MaxName };
        box.Loaded += (_, _) => { box.Focus(FocusState.Programmatic); box.SelectAll(); };
        body.Children.Add(box);
        var taken = Design.Parts.FieldError(T("A profile with that name already exists."));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetLiveSetting(taken, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        body.Children.Add(taken);
        var d = new ContentDialog { Tag = "layers", Title = title, Content = body, PrimaryButtonText = yes, CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
        void Check()
        {
            var name = box.Text.Trim();
            var used = !name.Equals(initial, StringComparison.OrdinalIgnoreCase) && ModProfiles.Exists(name);
            taken.Visibility = used ? Visibility.Visible : Visibility.Collapsed;
            d.IsPrimaryButtonEnabled = name.Length > 0 && !used;
        }
        box.TextChanged += (_, _) => Check();
        Check();
        return await ShowDialog(d) == ContentDialogResult.Primary ? box.Text.Trim() : null;
    }

    async Task<List<ModFile>> ModsNow()
    {
        var (game, docs) = (_game!, Docs);
        return await Task.Run(() => Mods.List(game, docs, kinds: false));
    }

    async Task NewProfileAsync()
    {
        if (await AskProfileName(T("New profile"), T("Saves which mods are on now."), "", T("Create")) is not { } name) return;
        var mods = await ModsNow();
        await ProfileChange(() => ModProfiles.Create(name, mods), T("Profile {0} created", name));
    }

    async Task RenameProfileAsync(string name)
    {
        if (await AskProfileName(T("Rename profile"), null, name, T("Rename")) is not { } to || to == name) return;
        await ProfileChange(() => ModProfiles.Rename(name, to), T("Renamed to {0}", to), () => Safe(ProfileChange(() => ModProfiles.Rename(to, name), T("Renamed to {0}", name))));
    }

    async Task UpdateProfileAsync(string name)
    {
        var mods = await ModsNow();
        var was = ModProfiles.Active();
        Dictionary<string, bool>? before = null;
        await ProfileChange(() => before = ModProfiles.Update(name, mods), T("Profile {0} updated", name),
            () => Safe(ProfileChange(() => ModProfiles.Put(name, before!, was), T("Profile {0} is back as it was", name))));
    }

    async Task DeleteProfileAsync(string name)
    {
        if (!await Confirm(T("Delete the profile {0}?", name), T("Your mods stay as they are. Only the saved list is deleted."), T("Delete"), danger: true)) return;
        await ProfileChange(() => ModProfiles.Delete(name), T("Profile deleted"));
    }

    // a change to the saved profiles alone: no mod is touched
    Task ProfileChange(Action work, string done, Action? undo = null)
    {
        try { work(); ModsResult(done, undo); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        FillProfileButton();
        return Task.CompletedTask;
    }

    async Task SwitchProfileAsync(string name)
    {
        if (_game is null || _busy) return;
        var docs = Docs;
        ModProfiles.Plan plan;
        List<ModFile> mods;
        bool unsaved;
        try
        {
            ModProfiles.CheckCanSwitch();
            mods = await ModsNow();
            var all = ModProfiles.All();
            plan = ModProfiles.PlanFor(all.FirstOrDefault(p => p.Name == name) ?? throw new SetupException("That profile no longer exists."), mods, docs);
            // (what the switch changes is kept by no profile: it would be gone for good once Undo is)
            var change = plan.TurnOn.Concat(plan.TurnOff).ToHashSet();
            unsaved = change.Count > 0 && !all.Any(p => ModProfiles.PlanFor(p, mods, docs) is var now && now.Same && !now.Left.Any(change.Contains));
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("The profile was not switched"), ErrorText(e)); return; }

        // said first: what saves can lose, and what would no longer be kept anywhere
        var rows = _modRows.GroupBy(r => r.File.Path, StringComparer.OrdinalIgnoreCase).ToDictionary(x => x.Key, x => x.First(), StringComparer.OrdinalIgnoreCase);
        var lost = plan.TurnOff.Where(m => rows.TryGetValue(m.Path, out var r) && HasItems(r)).ToList();
        var keep = new CheckBox { IsChecked = true, Content = new TextBlock { Text = T("Save the mods that are on now as a profile first"), TextWrapping = TextWrapping.Wrap } };
        if (lost.Count > 0 || unsaved)
        {
            var body = new StackPanel { Spacing = 16, Width = Design.Dialogs.Small };
            body.Children.Add(Body(T("Mods to turn on: {0}. Mods to turn off: {1}.", plan.TurnOn.Count, plan.TurnOff.Count)));
            if (lost.Count > 0)
            {
                var names = string.Join("\n", lost.Take(5).Select(m => "•  " + Path.GetFileNameWithoutExtension(m.Name))) + (lost.Count > 5 ? "\n" + T("and {0} more", lost.Count - 5) : "");
                body.Children.Add(Warn(T("Saves can lose items"), T("These mods will be off. Lots and Sims that use their objects or outfits lose them the next time you save without them.") + "\n\n" + names));
            }
            if (unsaved) body.Children.Add(keep);
            var d = new ContentDialog { Tag = "layers", Title = T("Switch to {0}?", name), Content = body, PrimaryButtonText = T("Switch"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
            if (await ShowDialog(d) != ContentDialogResult.Primary) return;
        }
        if (_busy) return;
        var was = ModProfiles.Active();
        string? saved = null;
        List<ModFile> done;
        try
        {
            if (unsaved && keep.IsChecked == true) saved = ModProfiles.Create(T("Before {0}", name), mods, free: true, activate: false);
            SetInfo(ModsInfo, InfoBarSeverity.Informational, T("Switching to {0}…", name), "");
            done = await ModWork(() =>
            {
                var d = ModProfiles.Apply(plan.TurnOn, plan.TurnOff, docs);
                // (the mods are switched whatever becomes of the name on the button)
                try { ModProfiles.SetActive(name); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
                return d;
            });
        }
        catch (Exception e)
        {
            App.Log(e);
            SetInfo(ModsInfo, InfoBarSeverity.Error, T("The profile was not switched"), ErrorText(e));
            await ModsAgain();
            return;
        }
        var told = new List<string>();
        if (done.Count > 0) told.Add(T("Mods turned on: {0}. Mods turned off: {1}.", plan.TurnOn.Count, plan.TurnOff.Count));
        if (saved is not null) told.Add(T("What was on before is saved as {0}.", saved));
        if (plan.Gone.Count > 0)
            told.Add(T("No longer in your mods: {0}.", string.Join(", ", plan.Gone.Take(5).Select(x => Path.GetFileNameWithoutExtension(x))) + (plan.Gone.Count > 5 ? ", " + T("and {0} more", plan.Gone.Count - 5) : "")));
        if (plan.Left.Count > 0) told.Add(T("Left as they are, because the profile doesn't know them: {0}.", plan.Left.Count));
        ModsResult(T("Profile {0} is in use", name), done.Count == 0 ? null : () => Safe(UndoSwitchAsync(done, was)), string.Join(" ", told),
            plan.Gone.Count > 0 ? InfoBarSeverity.Warning : InfoBarSeverity.Success);
        await ModsAgain();
    }

    async Task UndoSwitchAsync(List<ModFile> done, string? was)
    {
        var docs = Docs;
        try
        {
            await ModWork(() =>
            {
                ModProfiles.Apply(done.Where(m => !m.Enabled).ToList(), done.Where(m => m.Enabled).ToList(), docs);
                try { ModProfiles.SetActive(was); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
                return 0;
            });
            ModsResult(T("Every mod is as it was before"), null);
        }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
        await ModsAgain();
    }
}
