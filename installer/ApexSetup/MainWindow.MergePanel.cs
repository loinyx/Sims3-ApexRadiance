using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;
using static ApexSetup.Strings;

namespace ApexSetup;

// A merge's details: its mods in their order, each with what the player wants of it (moved, updated, replaced, taken out),
// mods added and a new name. All of it waits in a draft (MergeDraft) until Save makes it in one go, or Discard forgets it.
// A merge with changes not saved stays open whatever the list shows, and leaving it asks. A mod inside opens on its own.
public sealed partial class MainWindow
{
    MergeDraft? _draft;                 // of the merge that is open
    MergeEditPlan? _draftPlan;          // what the draft would do, when it is known
    int _draftRun;
    string? _mergeInner;                // the mod inside whose details are open (its Rel)
    string? _mergeOpen;                 // the mod whose options show in the list (its Rel)
    Action? _mergePaint;                // the list that shows, painted again
    bool _leavingMerge, _pickedWaits;
    List<MergeUpdate> _mergeUpdates = [];
    (string Key, ModRow Row)? _inner;   // the open mod inside, as a row
    readonly Dictionary<string, string> _innerReplaces = new(StringComparer.OrdinalIgnoreCase); // what a kept mod replaces in the game, by its path; "" when nothing

    // ---------- newer versions ----------

    // which mods of the list are newer versions of mods inside merges; read behind the list, after the game's own keys
    async Task LoadMergeUpdatesAsync(Task after, List<ModRow> rows)
    {
        await after;
        if (!ReferenceEquals(rows, _modRows)) return; // (the list was read again meanwhile)
        var (docs, mods, game) = (Docs, _mods.ToList(), _gameIndex);
        List<MergeUpdate> found;
        try { found = await Task.Run(() => ModMerge.Updates(docs, mods, game)); }
        catch (Exception e) when (e is not OutOfMemoryException) { App.Log(e); return; }
        if (!ReferenceEquals(rows, _modRows)) return;
        _mergeUpdates = found.Where(u => !u.Same).ToList();
        (_mergeNews, _mergeNewsOf) = (_mergeUpdates.Where(u => !ModViewPrefs.MergeLater.Contains(ModViewPrefs.LaterKey(u.Newer), StringComparer.OrdinalIgnoreCase)).ToList(), docs);
        ShowMergeUpdates();
        foreach (var r in rows)
        {
            var n = _mergeUpdates.Count(u => u.Merged.Rel.Equals(r.File.Rel, StringComparison.OrdinalIgnoreCase));
            if (n == r.NewVersions) continue;
            r.NewVersions = n;
            r.Refresh();
        }
        _mergePaint?.Invoke();
    }

    // the mod of the list that is a newer version of this one, as it is now (on, and not going in elsewhere)
    ModFile? NewerOf(MergeDraft d, MergeDraft.Item i) => i.Changed ? null
        : _mergeUpdates.FirstOrDefault(u => u.Merge.Dir.Equals(d.Dir, StringComparison.OrdinalIgnoreCase) && u.Original.Rel.Equals(i.Rel, StringComparison.OrdinalIgnoreCase)) is { } u
            && !d.Items.Any(x => Same(x.With ?? x.Added, u.Newer)) ? _mods.FirstOrDefault(m => m.Enabled && Same(m, u.Newer)) : null;

    static bool Same(ModFile? a, ModFile b) => a is not null && a.Rel.Equals(b.Rel, StringComparison.OrdinalIgnoreCase);

    // ---------- the draft ----------

    // the open merge's draft: the one being made, or a new one when the merge is another (or was changed since)
    void DraftFor(ModRow r, MergeRecord merge)
    {
        if (_draft is { } d && d.Dir.Equals(merge.Dir, StringComparison.OrdinalIgnoreCase) && d.Sha256 == merge.Sha256) { d.Rel = r.File.Rel; return; }
        ModMerge.Fresh();
        (_draft, _draftPlan, _mergeOpen) = (new MergeDraft(merge), null, null);
        _draftRun++;
    }

    // the merge with changes not saved, in the list as it is now (its file may have been moved or renamed since)
    ModRow? DraftRow() => _draft is not { Changed: true } d ? null
        : _modRows.FirstOrDefault(r => r.File.Rel.Equals(d.Rel, StringComparison.OrdinalIgnoreCase) && ModMerge.Info(r.File) is { } i && i.Dir.Equals(d.Dir, StringComparison.OrdinalIgnoreCase))
            ?? _modRows.FirstOrDefault(r => Mine(r) && ModMerge.Info(r.File) is { } i && i.Dir.Equals(d.Dir, StringComparison.OrdinalIgnoreCase));

    // something in the draft changed: the list, the foot, and (a moment later, off the window's thread) what it would do
    void Staged()
    {
        _mergePaint?.Invoke();
        // (a mod inside that is open has a foot of its own)
        if (_mergeInner is null && _modRows.FirstOrDefault(x => x.File.Rel.Equals(_draft?.Rel, StringComparison.OrdinalIgnoreCase)) is { } r && ModMerge.Info(r.File) is { } merge)
        {
            ModDetailsFoot.Children.Clear();
            ModDetailsFoot.ColumnDefinitions.Clear();
            DetailsFoot(r, merge, Mine(r));
        }
        var run = ++_draftRun;
        if (_draft is not { Changed: true, OnlyName: false }) { _draftPlan = null; _mergePaint?.Invoke(); return; }
        After(400, () => { if (run == _draftRun) Safe(PlanDraftAsync(run)); });
    }

    async Task PlanDraftAsync(int run)
    {
        if (_busy || _draft is not { } d || DraftRow() is not { } r) return;
        var plan = d.Plan(Docs, r.File);
        MergeEditPlan? p = null;
        try { p = await Task.Run(plan); }
        catch (Exception e) when (e is not OutOfMemoryException) { App.Log(e); } // (Save plans again, and says it)
        if (run != _draftRun) return;
        _draftPlan = p;
        _mergePaint?.Invoke();
    }

    void RenameMerge(ModRow r, string name)
    {
        if (_draft is not { } d) return;
        d.Name = name == r.Name ? null : name;
        FillModDetails();
        Staged();
    }

    void Discard()
    {
        (_draft, _draftPlan, _mergeOpen) = (null, null, null);
        _draftRun++;
        FillModDetails();
    }

    // why a planned change can't be made, in the player's words
    static List<string> Reasons(MergeEditPlan p) => p.Refusals.Concat(p.Plan?.Refusals ?? []).Select(r => T(r.Key, TArgs(r.Args)))
        .Concat(p.Plan?.Skipped.Select(s => T("{0} can't go in. {1}", Path.GetFileNameWithoutExtension(s.Mod.Name), T(s.Reason.Key, TArgs(s.Reason.Args)))) ?? []).ToList();

    // ---------- leaving with changes not saved ----------

    // true: there are changes not saved, so it asks first. go: once they are saved or discarded; stay: the player stays
    bool AskLeaveMerge(Action go, Action? stay = null)
    {
        if (_leavingMerge) return true;
        if (DraftRow() is not { } r) return false;
        _leavingMerge = true;
        DispatcherQueue.TryEnqueue(() => Safe(LeaveMergeAsync(r, go, stay)));
        return true;
    }

    async Task LeaveMergeAsync(ModRow r, Action go, Action? stay)
    {
        var leave = false;
        try
        {
            var d = new ContentDialog { Title = T("Save the changes to {0}?", r.Name), Content = T("They are not saved yet."), PrimaryButtonText = T("Save"), SecondaryButtonText = T("Discard"), CloseButtonText = T("Cancel"),
                DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot };
            var answer = await ShowDialog(d);
            leave = answer == ContentDialogResult.Secondary || answer == ContentDialogResult.Primary && await SaveMergeAsync();
            if (answer == ContentDialogResult.Secondary) Discard();
            if (!leave) stay?.Invoke();
        }
        finally { _leavingMerge = false; }
        if (leave) go();
        else FillModDetails();
    }

    // another mod picked while a merge has changes not saved: asked once the selection has settled (code clears it and adds to
    // it in steps). A selection that only went, as going to another folder does, leaves the merge open.
    bool AskPickedElsewhere()
    {
        if (_leavingMerge) return true;
        if (DraftRow() is null) return false;
        if (_pickedWaits) return true;
        _pickedWaits = true;
        DispatcherQueue.TryEnqueue(() =>
        {
            _pickedWaits = false;
            var want = Picked().Select(x => x.File.Rel).ToList();
            if (DraftRow() is { } r && want.Any(x => !x.Equals(r.File.Rel, StringComparison.OrdinalIgnoreCase))) { AskLeaveMerge(() => SelectMods(want), () => SelectMods([r.File.Rel])); return; }
            FillModDetails();
            FillFooter();
        });
        return true;
    }

    void SelectMods(IReadOnlyCollection<string> rels)
    {
        var list = _modsGrid ? (ListViewBase)ModGrid : ModList;
        list.SelectedItems.Clear();
        foreach (var r in _modShown.Where(r => rels.Contains(r.File.Rel, StringComparer.OrdinalIgnoreCase)).ToList()) list.SelectedItems.Add(r);
        FillModDetails();
        FillFooter();
    }

    // ---------- the list of its mods ----------

    UIElement MergeTab(ModRow r, MergeRecord merge, bool mine)
    {
        var d = _draft!;
        var s = new StackPanel { Spacing = 12 };
        var news = new StackPanel { Spacing = 8 };
        s.Children.Add(news);
        if (mine) s.Children.Add(new TextBlock { Text = T("If two mods change the same thing, the one higher up counts. Drag to change the order."), FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });

        var list = new ListView
        {
            ItemTemplate = EmptyCell(), SelectionMode = ListViewSelectionMode.None, IsItemClickEnabled = true, Padding = new Thickness(0),
            CanReorderItems = mine, CanDragItems = mine, AllowDrop = mine,
            Background = Res("ApexRaisedBrush"), BorderBrush = Res("ApexCardBorderBrush"), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(12),
        };
        AutomationProperties.SetName(list, T("Mods in this merge"));
        if (mine) AutomationProperties.SetHelpText(list, T("Alt with the up or down arrow moves a mod."));
        list.ItemClick += (_, e) => { if (e.ClickedItem is MergeDraft.Item { Added: null } i) OpenInner(i.Rel); };
        // (the list takes a drop of its own rows only: the side pane's folders must not take them as mods)
        list.DragItemsStarting += (_, _) => (_dragRows, _dragFolder) = (null, null);
        list.DragItemsCompleted += (_, _) => { DragEnded?.Invoke(); Staged(); };
        list.PreviewKeyDown += (_, e) =>
        {
            if (!mine || e.Key is not (VirtualKey.Up or VirtualKey.Down)) return;
            if (!Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Menu).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down)) return;
            if (FocusManager.GetFocusedElement(Content.XamlRoot) is not ListViewItem c || list.ItemFromContainer(c) is not MergeDraft.Item i) return;
            e.Handled = true;
            Move(i, e.Key == VirtualKey.Up ? -1 : 1, "");
        };
        list.ContainerContentChanging += (_, a) =>
        {
            if (a.InRecycleQueue || a.Item is not MergeDraft.Item i || a.ItemContainer.ContentTemplateRoot is not Grid g) return;
            a.ItemContainer.HorizontalContentAlignment = HorizontalAlignment.Stretch;
            (a.ItemContainer.Padding, a.ItemContainer.MinHeight) = (new Thickness(0), 0);
            Paint(a.ItemContainer, g, i);
            a.Handled = true;
        };

        // what a row's buttons do
        void Move(MergeDraft.Item i, int by, string focus)
        {
            var at = d.Items.IndexOf(i);
            if (at < 0 || at + by < 0 || at + by >= d.Items.Count) return;
            d.Items.Move(at, at + by);
            Staged();
            // (the row was made again: the key that moved it works again)
            DispatcherQueue.TryEnqueue(() =>
            {
                list.ScrollIntoView(i);
                list.UpdateLayout();
                if (list.ContainerFromItem(i) is ListViewItem c) FocusRow(c, focus, FocusState.Keyboard);
            });
        }
        void Open(MergeDraft.Item i) { _mergeOpen = _mergeOpen == i.Rel ? null : i.Rel; PaintAll(); }

        // a row: the grip, the mod and what it replaces or what will become of it, its one action, its options
        void Paint(Microsoft.UI.Xaml.Controls.Primitives.SelectorItem c, Grid g, MergeDraft.Item i)
        {
            var had = FocusManager.GetFocusedElement(Content.XamlRoot) as Control;
            var focus = had is not null && Within(had, g) ? had.Tag as string ?? "" : null;
            var state = had?.FocusState ?? FocusState.Programmatic;
            g.Children.Clear();
            g.ColumnDefinitions.Clear();
            g.RowDefinitions.Clear();
            (g.MinHeight, g.Padding, g.BorderBrush, g.BorderThickness) = (54, new Thickness(mine ? 8 : 14, 4, 4, 4), Res("ApexHairlineBrush"), new Thickness(0, 0, 0, 1));
            foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            void Put(FrameworkElement e, int col) { Grid.SetColumn(e, col); e.VerticalAlignment = VerticalAlignment.Center; g.Children.Add(e); }

            if (mine)
            {
                var grip = Lucide.Icon("grip-vertical", 14, Res("ApexMutedBrush"));
                grip.Margin = new Thickness(0, 0, 8, 0);
                Put(grip, 0);
            }
            var newer = NewerOf(d, i);
            var text = new StackPanel { Spacing = 2, Opacity = i.Out is null ? 1 : 0.55, Margin = new Thickness(0, 5, 6, 5) };
            text.Children.Add(new TextBlock { Text = i.Name, FontSize = 13, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
            var what = new TextBlock { FontSize = 12, TextWrapping = TextWrapping.Wrap, MaxLines = 3, TextTrimming = TextTrimming.CharacterEllipsis };
            text.Children.Add(what);
            Put(text, 1);
            void Say(string line, Brush brush)
            {
                (what.Text, what.Foreground) = (line, brush);
                ToolTipService.SetToolTip(text, i.Name + "\n" + line);
                AutomationProperties.SetName(c, i.Name + ", " + line);
            }
            var share = _draftPlan?.Plan?.Shares.GetValueOrDefault(i.NewRel);
            string With(string line) => string.Join(" · ", new[] { line, share is null ? null : ShareText(share) }.Where(x => !string.IsNullOrEmpty(x))); // (no share to tell: no dot left hanging)
            if (i.Changed) Say(With(ChangeText(i)), Res(i.Out is null ? "ApexSuccessBrush" : "ApexSoftTextBrush"));
            else if (newer is not null) Say(With(T("New version: {0}", Path.GetFileNameWithoutExtension(newer.Name))), Res("ApexWarningBrush"));
            else
            {
                Say(With(ModRow.SizeText(i.Size)), Res("ApexMutedBrush"));
                WhatItReplaces(merge, i.Rel, summary =>
                {
                    if (!g.Children.Contains(text)) return; // (the row was painted again)
                    Say(With(summary + " · " + ModRow.SizeText(i.Size)), new SolidColorBrush(Pick(0xFF9CC7EE, 0xFF2F6FA8)));
                });
            }

            if (mine && (i.Changed || newer is not null))
            {
                var label = i.Out is not null ? T("Keep") : i.Changed ? T("Undo") : T("Update");
                var act = new HyperlinkButton { Content = label, Tag = "act", Padding = new Thickness(8, 5, 8, 6), FontSize = 13, FontWeight = FontWeights.SemiBold };
                AutomationProperties.SetName(act, label + ", " + i.Name);
                act.Click += (_, _) =>
                {
                    if (i.Added is not null) d.Items.Remove(i);
                    else if (i.Changed) (i.Out, i.With, i.Newer) = (null, null, false);
                    else (i.With, i.Newer) = (newer, true);
                    Staged();
                };
                Put(act, 2);
            }
            var open = _mergeOpen == i.Rel && i.Out is null;
            if (mine && i.Out is null)
            {
                var more = IconButton(open ? "chevron-up" : "chevron-down", T("Options for {0}", i.Name));
                more.Tag = "more";
                more.Click += (_, _) => Open(i);
                Put(more, 3);
            }
            if (mine && open)
            {
                var options = new Grid { ColumnSpacing = 6, RowSpacing = 6, Margin = new Thickness(0, 2, 6, 8) };
                options.ColumnDefinitions.Add(new ColumnDefinition());
                options.ColumnDefinitions.Add(new ColumnDefinition());
                var at = d.Items.IndexOf(i);
                Button Option(string tag, string label, Action click, bool enabled = true)
                {
                    var b = Dims(new Button { Content = new TextBlock { Text = label, FontSize = 12, TextTrimming = TextTrimming.CharacterEllipsis }, Tag = tag, HorizontalAlignment = HorizontalAlignment.Stretch, Height = 34, Padding = new Thickness(8, 0, 8, 0), IsEnabled = enabled });
                    AutomationProperties.SetName(b, label + ", " + i.Name);
                    ToolTipService.SetToolTip(b, label);
                    b.Click += (_, _) => click();
                    if (options.Children.Count % 2 == 0) options.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                    Grid.SetRow(b, options.Children.Count / 2);
                    Grid.SetColumn(b, options.Children.Count % 2);
                    options.Children.Add(b);
                    return b;
                }
                Option("up", T("Move up"), () => Move(i, -1, "up"), at > 0);
                Option("down", T("Move down"), () => Move(i, 1, "down"), at < d.Items.Count - 1);
                if (i.Added is null)
                {
                    Option("file", T("Replace file"), () => Safe(ReplaceInsideAsync(r, i.Rel)));
                    Button? take = null;
                    take = Option("out", T("Take out"), () =>
                    {
                        // (the row is made again while the menu has the keyboard)
                        var menu = TakeOutMenu(r, i.Rel);
                        menu.Closed += (_, _) => DispatcherQueue.TryEnqueue(() => FocusRow(c, "out", FocusState.Programmatic));
                        menu.ShowAt(take!);
                    });
                }
                Grid.SetRow(options, 1);
                Grid.SetColumn(options, 1);
                Grid.SetColumnSpan(options, 3);
                g.Children.Add(options);
            }
            if (focus is not null) DispatcherQueue.TryEnqueue(() => FocusRow(c, focus, state));
        }

        void PaintAll()
        {
            news.Children.Clear();
            var found = mine ? d.Items.Where(i => NewerOf(d, i) is not null).ToList() : [];
            if (found.Count > 0)
            {
                var all = Link(T("Update all"), () =>
                {
                    foreach (var i in found)
                        if (NewerOf(d, i) is { } n) (i.With, i.Newer) = (n, true);
                    Staged();
                });
                news.Children.Add(Banner("sparkles", found.Count == 1 ? T("1 mod has a new version") : T("{0} mods have a new version", found.Count), all));
            }
            if (d.Changed && _draftPlan is { CanRun: false } no)
                foreach (var why in Reasons(no)) news.Children.Add(Warn(why, null));
            if (d.Left < 2) news.Children.Add(Warn(T("After saving, the merge is removed"), T("A merge needs at least 2 mods. The ones that stay are separate mods again.")));
            news.Visibility = news.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            foreach (var i in d.Items)
                if (list.ContainerFromItem(i) is ListViewItem { ContentTemplateRoot: Grid g } c) Paint(c, g, i);
        }

        if (mine)
        {
            var add = WideButton("plus", T("Add mods"), () => Safe(AddToMergeAsync(r)));
            (add.Height, add.Padding, add.CornerRadius, add.HorizontalContentAlignment, add.BorderThickness, add.Background) = (44, new Thickness(8, 0, 12, 0), new CornerRadius(8), HorizontalAlignment.Left, new Thickness(0), Res("ApexClearBrush"));
            list.Footer = add;
        }
        list.ItemsSource = d.Items;
        PaintAll();
        s.Children.Add(list);
        FitInPane(list);
        _mergePaint = PaintAll;
        return s;
    }

    static bool Within(DependencyObject? e, DependencyObject root)
    {
        for (; e is not null; e = VisualTreeHelper.GetParent(e)) if (ReferenceEquals(e, root)) return true;
        return false;
    }

    // the row's button of that name, else its options, else the row
    static void FocusRow(Control row, string tag, FocusState state)
    {
        static IEnumerable<Control> Buttons(DependencyObject e)
        {
            for (var n = 0; n < VisualTreeHelper.GetChildrenCount(e); n++)
            {
                var k = VisualTreeHelper.GetChild(e, n);
                if (k is Microsoft.UI.Xaml.Controls.Primitives.ButtonBase button) yield return button;
                else foreach (var b in Buttons(k)) yield return b;
            }
        }
        var all = Buttons(row).Where(b => b.IsEnabled).ToList();
        ((tag.Length == 0 ? null : all.FirstOrDefault(b => (b.Tag as string) == tag) ?? all.FirstOrDefault(b => b.Tag as string is "more" or "act")) ?? row).Focus(state == FocusState.Unfocused ? FocusState.Programmatic : state);
    }

    static Border Banner(string icon, string text, HyperlinkButton action)
    {
        var g = new Grid { ColumnSpacing = 10 };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var i = Lucide.Icon(icon, 15, Res("ApexAccentBrush"));
        i.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(i);
        var t = new TextBlock { Text = text, FontSize = 13, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center };
        AutomationProperties.SetLiveSetting(t, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        Grid.SetColumn(t, 1);
        g.Children.Add(t);
        action.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(action, 2);
        g.Children.Add(action);
        return new Border { Child = g, Padding = new Thickness(12, 10, 12, 10), CornerRadius = new CornerRadius(10), Background = Res("ApexAccentSoftBrush"), BorderBrush = Res("ApexAccentLineBrush"), BorderThickness = new Thickness(1) };
    }

    // what becomes of a mod when the change is saved
    static string ChangeText(MergeDraft.Item i) => i switch
    {
        { Added: not null } => T("Goes in when you save"),
        { Out: TakeOutTo.On } => T("Comes out when you save, on"),
        { Out: TakeOutTo.Off } => T("Comes out when you save, off"),
        { Out: TakeOutTo.Bin } => T("Goes to the Recycle Bin when you save"),
        { Newer: true, With: { } w } => T("Updated when you save: {0}", Path.GetFileNameWithoutExtension(w.Name)),
        { With: { } w } => T("Replaced when you save: {0}", Path.GetFileNameWithoutExtension(w.Name)),
        _ => "",
    };

    static string ShareText(MergeShare s) => string.Join(" · ", new[]
    {
        s.Kept == 0 ? null : s.Kept == 1 ? T("Has priority in 1 item") : T("Has priority in {0} items", s.Kept),
        s.Replaced == 0 ? null : s.Replaced == 1 ? T("1 item replaced, the rest goes in") : T("{0} items replaced, the rest goes in", s.Replaced),
    }.OfType<string>());

    // what a mod of the merge replaces in the game, read from the copy kept at the merge
    void WhatItReplaces(MergeRecord merge, string rel, Action<string> say)
    {
        if (_gameIndex is not { } gi) return;
        var kept = Path.Combine(merge.Dir, Path.GetRelativePath("Mods", rel));
        if (_innerReplaces.TryGetValue(kept, out var known)) { if (known.Length > 0) say(known); return; }
        if (!File.Exists(kept)) return;
        _ = Task.Run(() => DefaultReplacements.Analyze(gi, kept)).ContinueWith(t =>
        {
            if (!t.IsCompletedSuccessfully) return;
            _innerReplaces[kept] = known = t.Result.Replaced > 0 ? ReplacesSummary(t.Result) : "";
            if (known.Length > 0) say(known);
        }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    // ---------- what the player does to a mod ----------

    MergeDraft.Item? DraftItem(string rel) => _draft?.Items.FirstOrDefault(i => i.Added is null && i.Rel.Equals(rel, StringComparison.OrdinalIgnoreCase));

    // the change is seen in the list: from the mod's own details, back to it
    void ToMergeList()
    {
        if (_mergeInner is not null)
        {
            (_mergeInner, _detailsTab) = (null, "inside");
            FillModDetails();
        }
        Staged();
    }

    void TakeOut(string rel, TakeOutTo to)
    {
        if (DraftItem(rel) is not { } i) return;
        (i.Out, i.With, i.Newer) = (to, null, false);
        ToMergeList();
    }

    MenuFlyout TakeOutMenu(ModRow r, string rel)
    {
        var menu = new MenuFlyout();
        menu.Items.Add(MenuItem(r.File.Enabled ? T("Stays on, as a separate mod") : T("Turned on, as a separate mod"), () => TakeOut(rel, TakeOutTo.On), "power"));
        menu.Items.Add(MenuItem(T("Turned off: the game's own items come back"), () => TakeOut(rel, TakeOutTo.Off), "power-off"));
        menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(MenuItem(T("Goes to the Recycle Bin"), () => TakeOut(rel, TakeOutTo.Bin), "trash-2", danger: true));
        return menu;
    }

    async Task ReplaceInsideAsync(ModRow r, string rel)
    {
        var draft = _draft;
        var picker = new Microsoft.Windows.Storage.Pickers.FileOpenPicker(AppWindow.Id);
        picker.FileTypeFilter.Add(".package");
        if (await picker.PickSingleFileAsync() is not { } file) return;
        if (!ReferenceEquals(draft, _draft) || DraftItem(rel) is not { } i) return; // (the merge changed meanwhile)
        try { (i.With, i.Newer, i.Out) = (_mods.FirstOrDefault(m => m.Path.Equals(file.Path, StringComparison.OrdinalIgnoreCase)) ?? ModMerge.Outside(file.Path), false, null); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); return; }
        ToMergeList();
    }

    // other mods that can go in: packages of the player's that are on, found by name or folder
    async Task AddToMergeAsync(ModRow r)
    {
        if (_draft is not { } draft) return;
        var all = _modRows.Where(x => Mine(x) && x.File.Enabled && !ReferenceEquals(x, r) && !draft.Items.Any(i => Same(i.With ?? i.Added, x.File)))
            .OrderBy(x => x.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        var chosen = new HashSet<ModRow>();
        var search = new TextBox { PlaceholderText = T("Search mods") };
        AutomationProperties.SetName(search, T("Search mods"));
        var list = new ListView { ItemTemplate = EmptyCell(), SelectionMode = ListViewSelectionMode.Multiple, Height = 340, Padding = new Thickness(0) };
        AutomationProperties.SetName(list, T("Add mods to this merge"));
        list.ContainerContentChanging += (_, a) =>
        {
            if (a.InRecycleQueue || a.Item is not ModRow x || a.ItemContainer.ContentTemplateRoot is not Grid g) return;
            a.ItemContainer.HorizontalContentAlignment = HorizontalAlignment.Stretch;
            g.Children.Clear();
            g.ColumnSpacing = 10;
            if (g.ColumnDefinitions.Count == 0) foreach (var w in new[] { new GridLength(1, GridUnitType.Star), GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
            var words = new StackPanel { Spacing = 1, Padding = new Thickness(0, 6, 0, 6) };
            words.Children.Add(new TextBlock { Text = x.Name, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis });
            words.Children.Add(new TextBlock { Text = ModRow.Place(x.File), FontSize = 12, Foreground = Res("ApexMutedBrush"), TextTrimming = TextTrimming.CharacterEllipsis });
            g.Children.Add(words);
            var size = new TextBlock { Text = x.Size, FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(size, 1);
            g.Children.Add(size);
            AutomationProperties.SetName(a.ItemContainer, $"{x.Name}, {ModRow.Place(x.File)}, {x.Size}");
            a.Handled = true;
        };
        var none = Muted(T("No mod here can go in."));
        var body = new StackPanel { Spacing = 12, Width = 440 };
        body.Children.Add(search);
        body.Children.Add(list);
        body.Children.Add(none);
        var dlg = new ContentDialog { Title = T("Add mods to this merge"), Content = body, PrimaryButtonText = T("Add"), CloseButtonText = T("Cancel"), DefaultButton = ContentDialogButton.Primary, IsPrimaryButtonEnabled = false, XamlRoot = Content.XamlRoot };
        List<ModRow> shown = [];
        list.SelectionChanged += (_, e) =>
        {
            foreach (var x in e.AddedItems.OfType<ModRow>()) chosen.Add(x);
            foreach (var x in e.RemovedItems.OfType<ModRow>().Where(shown.Contains)) chosen.Remove(x); // (one a search took away stays chosen)
            dlg.IsPrimaryButtonEnabled = chosen.Count > 0;
        };
        void Fill()
        {
            var q = search.Text.Trim();
            shown = all.Where(x => q.Length == 0 || (x.Name + " " + ModRow.Place(x.File)).Contains(q, StringComparison.CurrentCultureIgnoreCase)).ToList();
            var keep = shown.Where(chosen.Contains).ToList();
            list.ItemsSource = shown;
            foreach (var x in keep) list.SelectedItems.Add(x);
            none.Visibility = shown.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }
        search.TextChanged += (_, _) => Fill();
        Fill();
        if (await ShowDialog(dlg) != ContentDialogResult.Primary || !ReferenceEquals(draft, _draft)) return;
        foreach (var x in all.Where(chosen.Contains)) draft.Items.Add(new MergeDraft.Item { Rel = x.File.Rel, Size = x.File.Size, Added = x.File });
        Staged();
    }

    // ---------- save ----------

    void DraftFoot(ModRow r)
    {
        var f = ModDetailsFoot;
        f.Visibility = Visibility.Visible;
        f.ColumnDefinitions.Add(new ColumnDefinition());
        var s = new StackPanel { Spacing = 10 };
        var t = new TextBlock { Text = T("Changes not saved"), FontSize = 13, FontWeight = FontWeights.SemiBold, TextWrapping = TextWrapping.Wrap };
        AutomationProperties.SetLiveSetting(t, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        s.Children.Add(t);
        var g = new Grid { ColumnSpacing = 8 };
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition());
        var no = new Button { Content = T("Discard"), HorizontalAlignment = HorizontalAlignment.Stretch, Height = 36 };
        no.Click += (_, _) => Discard();
        g.Children.Add(no);
        var yes = MainButton(T("Save"), () => Safe(SaveMergeAsync()));
        Grid.SetColumn(yes, 1);
        g.Children.Add(yes);
        s.Children.Add(g);
        f.Children.Add(s);
    }

    // The draft made, all of it or nothing: planned again, shown while it runs (it can be stopped until files move), and what
    // happened. True: saved.
    async Task<bool> SaveMergeAsync()
    {
        if (_busy || _draft is not { } d || DraftRow() is not { } r) return false;
        if (d.OnlyName)
        {
            var was = r.File.Rel;
            RenameMod(r, d.Name!);
            if (r.File.Rel == was) return false; // (it said why)
            _detailsFor = r.File.Rel;
            Discard();
            return true;
        }
        var plan = d.Plan(Docs, r.File);
        _draftRun++;
        var bar = new ProgressBar { Maximum = 1 };
        AutomationProperties.SetName(bar, T("Saving {0}", r.Name));
        var step = new TextBlock { Text = T("Check the files"), FontSize = 13, TextWrapping = TextWrapping.Wrap };
        AutomationProperties.SetLiveSetting(step, Microsoft.UI.Xaml.Automation.Peers.AutomationLiveSetting.Polite);
        var count = new TextBlock { FontSize = 12, Foreground = Res("ApexMutedBrush") };
        var body = new StackPanel { Spacing = 12, Width = 400 };
        foreach (var x in new FrameworkElement[] { bar, step, count }) body.Children.Add(x);
        var cts = new CancellationTokenSource();
        var running = true;
        var dlg = new ContentDialog { Title = T("Saving {0}", r.Name), Content = body, CloseButtonText = T("Cancel"), XamlRoot = Content.XamlRoot };
        dlg.CloseButtonClick += (_, e) => { if (running) { e.Cancel = true; cts.Cancel(); dlg.CloseButtonText = T("Stopping…"); } };
        var steps = new[] { (MergeStep.Check, T("Check the files")), (MergeStep.Write, T("Write the merged package")), (MergeStep.Verify, T("Check every resource against the originals")), (MergeStep.Move, T("Put the files in place")), (MergeStep.Finish, T("Put the files in place")) };
        var progress = new Progress<MergeProgress>(p =>
        {
            if (!running) return;
            var at = Math.Max(0, Array.FindIndex(steps, x => x.Item1 == p.Step));
            step.Text = steps[at].Item2;
            count.Text = p.Total <= 0 || p.Step >= MergeStep.Move ? "" : $"{ModRow.SizeText(p.Done)} / {ModRow.SizeText(p.Total)}";
            bar.Value = (at + (p.Total > 0 ? (double)p.Done / p.Total : 0)) / steps.Length;
        });
        var shown = ShowDialog(dlg);
        MergeEditPlan? planned = null;
        ModMerge.TakeOutResult? res = null;
        Exception? error = null;
        try
        {
            res = await ModWork(() =>
            {
                planned = plan();
                cts.Token.ThrowIfCancellationRequested();
                return planned.CanRun ? ModMerge.Run(planned, progress, cts.Token) : null;
            });
        }
        catch (OperationCanceledException e) { error = e; }
        catch (Exception e) { App.Log(e); error = e; }
        running = false;

        if (res is null)
        {
            var stopped = error is OperationCanceledException;
            // (a SetupException says itself what is out of place, when something is)
            var why = stopped ? T("Nothing was changed.") : error is SetupException ? ErrorText(error) : error is not null ? ErrorText(error) + " " + T("Your mods are as they were.")
                : string.Join("\n", Reasons(planned!)) + "\n" + T("Nothing was changed.");
            dlg.Title = stopped ? T("Not saved") : T("The merge was not saved");
            dlg.Content = new TextBlock { Text = why, TextWrapping = TextWrapping.Wrap, MaxWidth = 440 };
            dlg.CloseButtonText = T("Close");
            SetInfo(ModsInfo, stopped ? InfoBarSeverity.Informational : InfoBarSeverity.Error, (string)dlg.Title, why); // (the dialog may be gone: another one takes its place)
            await shown;
            _draftPlan = planned;
            if (error is not null && !stopped) await ModsAgain();
            _mergePaint?.Invoke();
            return false;
        }

        dlg.Hide();
        await shown;
        // (a file the Recycle Bin could not take is in the list instead, turned off: said, so it is no surprise)
        var asked = d.Items.Where(i => i.Added is null && i.Out != TakeOutTo.Bin && (i.Out is not null || res.Remerged is null)).Select(i => i.Rel).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var left = res.Back.Where(m => !asked.Contains(m.Rel)).Select(m => Path.GetFileNameWithoutExtension(m.Name)).ToList();
        var detail = left.Count == 0 ? "" : T("Windows could not move these to the Recycle Bin, so they are in your mods, turned off: {0}", string.Join(", ", left));
        (_draft, _draftPlan, _mergeOpen, _mergeInner) = (null, null, null, null);
        ModsResult(res.Remerged is { } m ? T("{0} saved and checked. It holds {1} mods.", Path.GetFileNameWithoutExtension(m.Merged.Name), m.Mods) : T("Every mod is separate again"), null, detail, left.Count == 0 ? InfoBarSeverity.Success : InfoBarSeverity.Warning);
        await ModsAgain();
        if (res.Remerged is { } made)
        {
            (_detailsFor, _detailsTab) = (made.Merged.Rel, "inside");
            SelectMods([made.Merged.Rel]);
        }
        return true;
    }

    // ---------- a mod inside ----------

    void OpenInner(string rel)
    {
        (_mergeInner, _detailsTab) = (rel, "overview");
        FillModDetails();
    }

    void CloseInner()
    {
        (_mergeInner, _detailsTab) = (null, "inside");
        FillModDetails();
    }

    // the kept copy as a row, its kinds, picture and what it replaces read behind it
    ModRow InnerRow(MergeRecord merge, MergeOriginal o)
    {
        var key = merge.Dir + "|" + o.Rel + "|" + o.Sha256 + "|" + (_gameIndex is null); // (read again once the game's own keys are)
        if (_inner is not { } known || known.Key != key)
        {
            var row = new ModRow(ModMerge.Inside(merge, o), []);
            row.DetectedKinds = row.Kinds;
            _inner = (key, row);
            Safe(Read(row));
        }
        var inner = _inner!.Value.Row;
        inner.Kinds = o.Note?.Kinds is { Count: > 0 } k ? k : inner.DetectedKinds;
        return inner;

        async Task Read(ModRow row)
        {
            var (path, gi, own) = (row.File.Path, _gameIndex, o.Note?.Picture is { } p ? Path.Combine(ModNotes.PictureDir, Path.GetFileName(p)) : null);
            try
            {
                var (kinds, replaces, picture) = await Task.Run(() => (ModKinds.Read(path).Kinds.Select(x => x.ToString()).ToList(), gi is null ? Replacements.None : DefaultReplacements.Analyze(gi, path),
                    own is not null && File.Exists(own) ? File.ReadAllBytes(own) : Mods.Thumbnail(row.File)));
                if (kinds.Count > 0) row.DetectedKinds = kinds;
                row.Defaults = replaces;
                row.Image = await SaveCard.ImageOf(picture);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { App.Log(e); return; }
            // (not under the player's hands: what they type or open is theirs)
            if (_inner?.Row == row && _mergeInner is not null && _mergeInner.Equals(o.Rel, StringComparison.OrdinalIgnoreCase) && FocusManager.GetFocusedElement(Content.XamlRoot) is not TextBox) FillModDetails();
        }
    }

    void FillInnerDetails(ModRow r, MergeRecord merge, MergeOriginal o, bool mine)
    {
        var back = new Button { Padding = new Thickness(2, 4, 10, 4), Margin = new Thickness(-4, 0, 0, 0), BorderThickness = new Thickness(0), Background = Res("ApexClearBrush") };
        var label = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        label.Children.Add(Lucide.Icon("chevron-left", 16, Res("ApexSoftTextBrush")));
        label.Children.Add(new TextBlock { Text = _draft?.Name ?? r.Name, FontSize = 13, Foreground = Res("ApexSoftTextBrush"), MaxWidth = 250, TextTrimming = TextTrimming.CharacterEllipsis });
        back.Content = label;
        AutomationProperties.SetName(back, T("Back to {0}", r.Name));
        back.Click += (_, _) => CloseInner();
        ModRow row;
        try { row = InnerRow(merge, o); }
        catch (Exception e) when (e is SetupException or IOException or UnauthorizedAccessException or InvalidDataException)
        {
            App.Log(e);
            ModDetailsHead.Children.Add(new Border { Child = back, Padding = new Thickness(20, 14, 12, 10) });
            ModDetails.Children.Add(Warn(T("An original of this merge is missing or changed: {0}", Path.GetFileName(o.Rel)), null));
            return;
        }

        var tabs = new List<(string Key, string Label)> { ("overview", T("Overview")), ("content", T("Content")) };
        if (r.File.Enabled) tabs.Add(("conflicts", T("Conflicts")));
        if (row.Defaults.IsDefault && _gameIndex is not null) tabs.Add(("replaces", T("Replaces")));
        if (mine) tabs.Add(("notes", T("Notes")));
        if (!tabs.Any(t => t.Key == _detailsTab)) _detailsTab = "overview";

        var badges = new List<Border>();
        if (row.Defaults.IsDefault) badges.Add(StripBadge(T("Default replacement"), "replace", "#9CC7EE"));
        ModDetailsHead.Children.Add(HeadPanel(back, PictureTile(row, KindIcon(row.Kinds[0]), o.Note?.Picture is not null), null, NameText(row.Name, row.File.Name), T("Inside the merge {0}", r.Name), badges));
        DetailsTabs(tabs, null);

        // the player's own words about it, kept with the merge
        void Write(Action<ModNote> change) => ModMerge.EditNote(Docs, r.File, o.Rel, change);
        void Change(Action<ModNote> change)
        {
            try { Write(change); }
            catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); }
            FillModDetails();
        }
        switch (_detailsTab)
        {
            case "content": ModDetails.Children.Add(ContentTab(row.File.Path)); break;
            case "conflicts": ModDetails.Children.Add(ConflictsTab(r.File, row.File.Path)); break;
            case "replaces": ModDetails.Children.Add(ReplacesTab(row)); break;
            case "notes":
                var notes = o.Note?.Notes ?? "";
                ModDetails.Children.Add(NotesBox(notes, 200, text => { if (text.Trim() != notes) { Write(n => n.Notes = text); notes = text.Trim(); } }));
                break;
            default: InnerOverview(r, row, merge, o, mine, Write, Change); break;
        }
        if (mine) InnerFoot(r, o);
    }

    void InnerOverview(ModRow r, ModRow row, MergeRecord merge, MergeOriginal o, bool mine, Action<Action<ModNote>> write, Action<Action<ModNote>> change)
    {
        var host = ModDetails;
        var d = _draft!;
        var item = DraftItem(o.Rel);
        if (mine && item is { Changed: true })
            host.Children.Add(Banner("info", ChangeText(item), Link(item.Out is not null ? T("Keep") : T("Undo"), () => { (item.Out, item.With, item.Newer) = (null, null, false); FillModDetails(); Staged(); })));
        else if (mine && item is not null && NewerOf(d, item) is { } newer)
            host.Children.Add(Banner("sparkles", T("New version: {0}", Path.GetFileNameWithoutExtension(newer.Name)), Link(T("Update"), () => { (item.With, item.Newer) = (newer, true); ToMergeList(); })));

        var (who, guessed) = o.Note?.Creator is { } set ? (set, false) : (ModCreators.Guess(_mods.Append(row.File)).GetValueOrDefault(row.File.Path), true);
        var written = guessed ? "" : who ?? "";
        host.Children.Add(Facts((T("Size"), FactText(row.Size)), (T("Added"), FactText(Day(Added(row.File)))),
            (T("Creator"), CreatorBox(who, guessed, mine, name => { if (name.Trim() != written) { write(n => n.Creator = name); written = name.Trim(); } }))));
        if (row.Defaults.Replaced > 0) host.Children.Add(Note("replace", "ApexAccentBrush", ReplacesSummary(row.Defaults)));

        var what = Section(T("What it is"), mine ? T("Change") : null, out var pick);
        if (pick is not null) pick.Click += (_, _) => PickKinds(row, pick, kinds => change(n => n.Kinds = kinds));
        what.Children.Add(KindChips(row, o.Note?.Kinds is { Count: > 0 }));
        host.Children.Add(what);

        if (mine)
        {
            var tags = Section(T("Tags"), null, out _);
            tags.Children.Add(InnerTags(o, change));
            host.Children.Add(tags);
        }

        var where = Section(T("Where"), null, out _);
        where.Children.Add(new TextBlock { Text = string.Join(" › ", new[] { ModMerge.FolderName }.Concat(Path.GetRelativePath(Path.GetDirectoryName(merge.Dir)!, row.File.Path).Split(Path.DirectorySeparatorChar))), FontSize = 13, TextWrapping = TextWrapping.Wrap, Foreground = Res("ApexSoftTextBrush") });
        where.Children.Add(Muted(T("Taken out of the merge, it goes back to {0}.", ModRow.Place(row.File))));
        host.Children.Add(where);
        host.Children.Add(Actions(ActionRow("folder-search", T("Show file"), null, () => ShowFile(row.File.Path))));
    }

    UIElement InnerTags(MergeOriginal o, Action<Action<ModNote>> change)
    {
        var wrap = new WrapPanel { Spacing = 8 };
        var have = o.Note?.Tags ?? [];
        void Tag(string name, bool on) => change(n =>
        {
            n.Tags = (n.Tags ?? []).Where(t => !t.Equals(name, StringComparison.OrdinalIgnoreCase)).ToList();
            if (on) n.Tags.Add(name);
        });
        foreach (var name in have)
        {
            var color = ModNotes.ColorOf(name);
            var chip = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            chip.Children.Add(new TextBlock { Text = name, FontSize = 12, Foreground = TagDot.BrushOf(color), VerticalAlignment = VerticalAlignment.Center });
            var x = new Button { Content = Lucide.Icon("x", 10, TagDot.BrushOf(color)), Padding = new Thickness(3), MinWidth = 0, MinHeight = 0, Background = Res("ApexClearBrush"), BorderThickness = new Thickness(0) };
            AutomationProperties.SetName(x, T("Remove tag {0}", name));
            x.Click += (_, _) => Tag(name, false);
            chip.Children.Add(x);
            wrap.Children.Add(new Border { Child = chip, Height = 32, Padding = new Thickness(12, 0, 6, 0), CornerRadius = new CornerRadius(8), Background = TagDot.BrushOf(color, 0x26) });
        }
        var label = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        label.Children.Add(Lucide.Icon("tag", 12));
        label.Children.Add(new TextBlock { Text = T("Add tag"), FontSize = 12 });
        var add = new Button { Content = label, Height = 32, Padding = new Thickness(12, 0, 14, 0), CornerRadius = new CornerRadius(8) };
        AutomationProperties.SetName(add, T("Add tag"));
        add.Click += (_, _) =>
        {
            var menu = new MenuFlyout();
            foreach (var t in ModNotes.Tags)
            {
                var one = new ToggleMenuFlyoutItem { Text = t.Name, IsChecked = have.Contains(t.Name, StringComparer.OrdinalIgnoreCase), Icon = MenuIcon("tag", TagDot.BrushOf(t.Color)) };
                one.Click += (_, _) => Tag(t.Name, one.IsChecked);
                menu.Items.Add(one);
            }
            if (ModNotes.Tags.Count > 0) menu.Items.Add(new MenuFlyoutSeparator());
            menu.Items.Add(MenuItem(T("New tag…"), () => Safe(NewInnerTagAsync(Tag)), "plus"));
            menu.ShowAt(add);
        };
        wrap.Children.Add(add);
        return wrap;
    }

    async Task NewInnerTagAsync(Action<string, bool> tag)
    {
        if (await AskTag(T("New tag"), "", ModNotes.Colors[ModNotes.Tags.Count % ModNotes.Colors.Length], T("Create")) is not { } t) return;
        try { ModNotes.CreateTag(t.Name, t.Color); }
        catch (Exception e) { App.Log(e); SetInfo(ModsInfo, InfoBarSeverity.Error, T("Nothing was changed"), ErrorText(e)); return; }
        _folderSig = "";
        tag(t.Name.Trim(), true);
        FillMods();
    }

    void InnerFoot(ModRow r, MergeOriginal o)
    {
        var f = ModDetailsFoot;
        f.Visibility = Visibility.Visible;
        foreach (var w in new[] { new GridLength(1, GridUnitType.Star), new GridLength(1, GridUnitType.Star), GridLength.Auto }) f.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        var swap = new Button { Content = T("Replace file"), HorizontalAlignment = HorizontalAlignment.Stretch, Height = 36, Padding = new Thickness(8, 0, 8, 0) };
        swap.Click += (_, _) => Safe(ReplaceInsideAsync(r, o.Rel));
        f.Children.Add(swap);
        var take = new Button { Content = T("Take out"), HorizontalAlignment = HorizontalAlignment.Stretch, Height = 36, Padding = new Thickness(8, 0, 8, 0) };
        take.Click += (_, _) => TakeOutMenu(r, o.Rel).ShowAt(take);
        Grid.SetColumn(take, 1);
        f.Children.Add(take);
        var bin = IconButton("trash-2", T("Take it out and move it to the Recycle Bin"), "ApexErrorBrush");
        (bin.Width, bin.Height, bin.BorderThickness, bin.BorderBrush) = (36, 36, new Thickness(1), Res("ApexCardBorderBrush"));
        bin.Click += (_, _) => TakeOut(o.Rel, TakeOutTo.Bin);
        Grid.SetColumn(bin, 2);
        f.Children.Add(bin);
    }
}
