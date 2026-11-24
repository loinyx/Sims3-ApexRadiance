using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.System;
using static ApexSetup.Strings;

namespace ApexSetup;

// The merge dialog: every mod that goes in, in the order that counts, what conflicts and the player's choice for it, planned
// again at each change, so what it says is what runs. And the bar over the list when a merge still uses an older version of a mod.
public sealed partial class MainWindow
{
    // ---------- the merge ----------

    const int ScriptsOut = 0, ScriptsApart = 1, ScriptsIn = 2;

    async Task MergeAsync(List<ModRow> rows, string? name, string? folder)
    {
        if (_game is null) return;
        if (rows.Count == 0) { SetInfo(ModsInfo, InfoBarSeverity.Informational, T("These mods can't be merged"), T("Pick at least two packages that can be merged.")); return; }
        var mods = rows.Select(r => r.File).ToList();
        folder ??= rows.Select(r => DirOf(r.File)).GroupBy(d => d).OrderByDescending(g => g.Count()).First().Key;
        name ??= ModMerge.FreeName(Docs, folder, (Path.GetFileName(folder) is { Length: > 0 } f ? f : "Mods") + "-merged");
        if (await MergeDialog(Docs, mods, name, folder) is not { } jobs) return;
        var (done, error) = await MergeRunDialog(jobs);
        // (the game's caches point at the files as they were: cleared, a mod that looks missing in the game shows again)
        if (done.Count > 0)
        {
            var left = mods.Count - done.Sum(r => r.Mods);
            SetInfo(ModsInfo, error is null ? InfoBarSeverity.Success : InfoBarSeverity.Warning, T("{0} mods merged into {1}", done.Sum(r => r.Mods), string.Join(", ", done.Select(r => r.Merged.Name))),
                (error is null ? "" : T("The rest was not merged.") + " ") + (left == 0 ? "" : left == 1 ? T("1 mod was left out.") + " " : T("{0} mods were left out.", left) + " ")
                + T("Something missing in the game? Clear its caches."), T("Clear the game caches"), async () => await ClearCachesAfterMerge());
        }
        await ModsAgain();
    }

    // the merges to run, as the dialog showed them, or null
    async Task<List<MergeJob>?> MergeDialog(string docs, List<ModFile> mods, string name, string folder)
    {
        var choices = new MergeChoices();
        List<MergePlan> plans = [];
        MergePlan? scripts = null;
        string? trouble = null;
        int want = 1, got = 0; // (a change raises want; got follows once the plan made for it is on screen)
        bool planning = false, closed = false, parts = false;
        var scriptMods = 0;
        var views = new List<MergeRowView>();
        var timer = DispatcherQueue.CreateTimer();
        timer.IsRepeating = false;

        var d = new ContentDialog
        {
            Title = T("Merge {0} mods", mods.Count), PrimaryButtonText = T("Merge"), IsPrimaryButtonEnabled = false, CloseButtonText = T("Cancel"),
            DefaultButton = ContentDialogButton.Primary, XamlRoot = Content.XamlRoot,
        };
        d.Resources["ContentDialogMaxWidth"] = 680.0;
        var box = new TextBox { Header = T("Name of the merged package"), Text = name };
        var line = Muted(T("Checking the mods…"));
        AutomationProperties.SetLiveSetting(line, AutomationLiveSetting.Polite);
        var busy = new ProgressRing { Width = 14, Height = 14, IsActive = true, VerticalAlignment = VerticalAlignment.Center };
        var status = new Grid { ColumnSpacing = 10 };
        status.ColumnDefinitions.Add(new ColumnDefinition());
        status.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        status.Children.Add(line);
        Grid.SetColumn(busy, 1);
        status.Children.Add(busy);
        var notices = new StackPanel { Spacing = 8, Visibility = Visibility.Collapsed };
        var putAll = new HyperlinkButton { Content = T("Put all in"), Padding = new Thickness(0) };
        var leaveAll = new HyperlinkButton { Content = T("Leave all conflicts out"), Padding = new Thickness(0) };
        var quick = new WrapPanel { Spacing = 16 };
        quick.Children.Add(putAll);
        quick.Children.Add(leaveAll);
        var list = new ListView
        {
            ItemsSource = choices.Rows, SelectionMode = ListViewSelectionMode.Single, CanDragItems = true, CanReorderItems = true, AllowDrop = true, MaxHeight = 320,
            ItemTemplate = (DataTemplate)Microsoft.UI.Xaml.Markup.XamlReader.Load("<DataTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"><Grid/></DataTemplate>"),
            ItemContainerStyle = new Style(typeof(ListViewItem))
            {
                BasedOn = (Style)Application.Current.Resources["DefaultListViewItemStyle"],
                Setters =
                {
                    new Setter(Control.PaddingProperty, new Thickness(12, 6, 8, 6)), new Setter(Control.HorizontalContentAlignmentProperty, HorizontalAlignment.Stretch),
                    new Setter(FrameworkElement.MinHeightProperty, 52.0),
                },
            },
        };
        AutomationProperties.SetName(list, T("Order"));
        var order = new StackPanel { Spacing = 8, Visibility = Visibility.Collapsed };
        order.Children.Add(new TextBlock { Text = T("Order"), FontSize = 13, FontWeight = FontWeights.SemiBold });
        order.Children.Add(Muted(T("If two mods change the same thing, the top one counts. Drag to change the order, or use the arrows.")));
        order.Children.Add(quick);
        order.Children.Add(new Border { Child = list, CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1), BorderBrush = Res("ApexHairlineBrush"), Background = new SolidColorBrush(Pick(0x0AFFFFFF, 0x06000000)) });
        var leftOut = new StackPanel { Spacing = 4, Visibility = Visibility.Collapsed };
        var scriptBox = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch };
        var scriptHost = new StackPanel { Spacing = 6, Visibility = Visibility.Collapsed };
        scriptHost.Children.Add(scriptBox);
        scriptHost.Children.Add(Muted(T("Their makers usually ask not to merge them. Apart, they are easy to put back.")));
        var partsRow = Toggle(T("Split into parts of about 1 GB"), T("For very large merges."), false, on => { parts = on; Changed(0); });
        partsRow.Visibility = Visibility.Collapsed;

        var body = new StackPanel { Spacing = 14, Width = 560 };
        foreach (var e in new UIElement[] { box, status, notices, order, leftOut, scriptHost, partsRow, Muted(T("The originals are kept in {0}. Split puts them back as they were.", ModMerge.ShownFolder)) }) body.Children.Add(e);
        d.Content = new ScrollViewer { Content = body, MaxHeight = Math.Clamp(Content.XamlRoot.Size.Height - 220, 300, 640), Padding = new Thickness(0, 0, 14, 0) };

        // ---- the plan, off the window's thread, a moment after the last change ----
        bool CanRun() => trouble is null && plans.Count > 0 && plans.All(p => p.CanRun) && scripts?.CanRun != false;
        bool Ready() => got == want && CanRun();
        string Named()
        {
            var n = box.Text.Trim();
            if (n.EndsWith(".package", StringComparison.OrdinalIgnoreCase)) n = n[..^8].TrimEnd();
            return n.Length > 0 ? n : name;
        }
        timer.Tick += (_, _) => Safe(Replan());
        void Changed(int wait)
        {
            want++;
            d.IsPrimaryButtonEnabled = false;
            busy.IsActive = true;
            Rows();
            timer.Stop();
            timer.Interval = TimeSpan.FromMilliseconds(Math.Max(1, wait));
            timer.Start();
        }
        void Decide(Func<MergeChoice, bool> which, bool put)
        {
            foreach (var r in choices.Rows.Where(which)) r.Pick = put;
            Changed(0);
        }
        async Task Replan()
        {
            if (planning) return;
            planning = true;
            try
            {
                while (got != want && !closed)
                {
                    var at = want;
                    var (n, how, split, o, left) = (Named(), scriptBox.SelectedItem is ComboBoxItem { Tag: int w } ? w : ScriptsOut, parts, choices.Order, choices.LeaveOut);
                    try
                    {
                        var (made, apart) = await Task.Run(() =>
                        {
                            var p = ModMerge.PlanParts(docs, mods, n, folder, how == ScriptsIn, o, left, split ? ModMerge.PartBytes : long.MaxValue);
                            return (p, how == ScriptsApart && p[0].Scripts.ToList() is { Count: > 1 } s ? ModMerge.Plan(docs, s, n + " scripts", folder, true, o, left) : null);
                        });
                        if (closed) return;
                        if (at != want) continue; // (changed again meanwhile)
                        (plans, scripts, trouble) = (made, apart, null);
                        // (a mod that conflicts now and was not shown so: planned again without it, until the player decides)
                        if (!choices.Take(plans, scripts, o, left)) { want++; continue; }
                    }
                    catch (Exception e)
                    {
                        App.Log(e);
                        if (at != want) continue;
                        trouble = ErrorText(e);
                    }
                    got = at;
                }
            }
            finally { planning = false; }
            if (!closed) Fill();
        }

        // ---- what is on screen ----
        void Fill()
        {
            var inside = plans.Sum(p => p.Inputs.Count) + (scripts?.Inputs.Count ?? 0);
            var resources = plans.Sum(p => p.Resources) + (scripts?.Resources ?? 0);
            var bytes = plans.Sum(p => p.OutputBytes) + (scripts?.OutputBytes ?? 0);
            var made = plans.Count + (scripts is null ? 0 : 1);
            busy.IsActive = false;
            d.Title = inside == mods.Count ? T("Merge {0} mods", inside) : T("Merge {0} of the {1} mods chosen", inside, mods.Count);
            line.Text = trouble is not null || inside < 2 ? "" : made == 1 ? // (one mod: no merge yet, and the reason is said below)
                T("Into {0} · {1} mods · {2} resources · {3}", PlaceTitle(folder).Title, inside, resources, ModRow.SizeText(bytes))
                : T("Into {0} · {1} packages · {2} mods · {3} resources · {4}", PlaceTitle(folder).Title, made, inside, resources, ModRow.SizeText(bytes));
            d.PrimaryButtonText = CanRun() ? T("Merge") : T("Check again");
            d.IsPrimaryButtonEnabled = true;

            notices.Children.Clear();
            if (trouble is not null) notices.Children.Add(Warn(T("These mods can't be merged"), trouble));
            foreach (var r in plans.SelectMany(p => p.Refusals).DistinctBy(r => r.Text)) notices.Children.Add(Warn(T(r.Key, r.Args), null));
            foreach (var r in scripts?.Refusals ?? [])
                if (!plans.Any(p => p.Refusals.Any(x => x.Text == r.Text))) notices.Children.Add(Warn(T("The package of script mods can't be made"), T(r.Key, r.Args)));
            var open = choices.Rows.Count(r => r.Undecided);
            if (open > 0) notices.Children.Add(Warn(open == 1 ? T("1 mod conflicts and stays out until you choose") : T("{0} mods conflict and stay out until you choose", open), T("Put in the ones you want, or leave them out.")));
            notices.Visibility = notices.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;

            order.Visibility = choices.Rows.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
            Rows();

            leftOut.Children.Clear();
            var gone = choices.Others(plans, scripts);
            if (gone.Count > 0) leftOut.Children.Add(new TextBlock { Text = T("Left out"), FontSize = 13, FontWeight = FontWeights.SemiBold });
            foreach (var s in gone.Take(30))
                leftOut.Children.Add(new TextBlock { Text = $"{Path.GetFileNameWithoutExtension(s.Mod.Name)}: {T(s.Reason.Key, s.Reason.Args)}", FontSize = 12, Foreground = Res("ApexMutedBrush"), TextWrapping = TextWrapping.Wrap });
            if (gone.Count > 30) leftOut.Children.Add(Muted(T("and {0} more", gone.Count - 30)));
            leftOut.Visibility = gone.Count > 0 ? Visibility.Visible : Visibility.Collapsed;

            // (the first plan sets the script mods aside: how many there are does not change after it)
            if (scriptMods == 0 && plans.Count > 0 && plans[0].Scripts.Count() is > 0 and var n)
            {
                scriptMods = n;
                scriptBox.Header = n == 1 ? T("1 script mod") : T("{0} script mods", n);
                void Way(string text, int how) => scriptBox.Items.Add(new ComboBoxItem { Content = text, Tag = how });
                Way(n == 1 ? T("Leave it out") : T("Leave them out"), ScriptsOut);
                if (n > 1 && n < mods.Count) Way(T("Merge the script mods in a separate package"), ScriptsApart);
                Way(n == 1 ? T("Include it in this merge") : T("Include them in this merge"), ScriptsIn);
                scriptBox.SelectedIndex = 0;
                scriptBox.SelectionChanged += (_, _) => Changed(0);
                scriptHost.Visibility = Visibility.Visible;
            }
            partsRow.Visibility = parts || bytes > ModMerge.PartBytes ? Visibility.Visible : Visibility.Collapsed;
        }

        void Rows()
        {
            putAll.Visibility = choices.Rows.Any(r => r.Undecided) ? Visibility.Visible : Visibility.Collapsed;
            leaveAll.Visibility = choices.Rows.Any(r => r.Meets is not null && r.Pick != false) ? Visibility.Visible : Visibility.Collapsed;
            quick.Visibility = putAll.Visibility == Visibility.Visible || leaveAll.Visibility == Visibility.Visible ? Visibility.Visible : Visibility.Collapsed;
            foreach (var v in views.Where(v => v.Row is not null)) Show(v);
        }

        // ---- a row ----
        (string Text, string Brush) Info(MergeChoice r)
        {
            static string Plain(string file) => Path.GetFileNameWithoutExtension(file);
            if (r.Why is { } why) return (T(why.Key, why.Args), "ApexWarningBrush");
            if (r.Meets is not { } meets) return (r.Pick == false ? T("Stays out of the merge") : r.Scripts ? T("In the package of script mods") : T("No conflict"), "ApexMutedBrush");
            var several = meets.With.Count + meets.Outside.Count > 1;
            var others = meets.With.OrderByDescending(w => w.Value).Select(w => several ? $"{Plain(w.Key)} ({w.Value})" : Plain(w.Key)).Concat(meets.Outside).ToList();
            var who = string.Join(", ", others.Take(3)) + (others.Count > 3 ? " " + T("and {0} more", others.Count - 3) : "");
            var with = meets.Resources == 1 ? T("Conflicts with {0} in 1 resource.", who) : T("Conflicts with {0} in {1} resources.", who, meets.Resources);
            if (r.Pick == false) return (T("Stays out of the merge") + ". " + with, "ApexMutedBrush");
            if (r.Undecided) return (with + " " + T("Out until you choose."), "ApexWarningBrush");
            var share = r.Share is not { } s || s is { Kept: 0, Replaced: 0 } ? "" : " " + (s.Replaced == 0 ? T("Its copy counts.") : s.Kept == 0 ? T("Another copy counts. The rest of it goes in.") : T("Its copy counts in {0}, another one in {1}.", s.Kept, s.Replaced));
            // (one that stays out of the merge is outside it: which copy the game uses is then up to the load order)
            var beside = meets.With.Keys.Where(k => choices.Rows.FirstOrDefault(x => x.Mod.Rel.Equals(k, StringComparison.OrdinalIgnoreCase)) is not { In: true, Why: null }).Select(Plain).Concat(meets.Outside).Take(3).ToList();
            return (with + share + (beside.Count == 0 ? "" : " " + T("Outside the merge, it also meets {0}: which of them the game uses could change.", string.Join(", ", beside))),
                r.Share is { Replaced: > 0 } || beside.Count > 0 ? "ApexWarningBrush" : "ApexSuccessBrush");
        }

        void Show(MergeRowView v)
        {
            var r = v.Row!;
            var at = choices.Rows.IndexOf(r);
            if (at < 0) return; // (on its way to another place)
            var name = Path.GetFileNameWithoutExtension(r.Mod.Name);
            var (info, brush) = Info(r);
            v.Number.Text = (at + 1).ToString();
            v.Name.Text = name;
            v.Info.Text = info;
            v.Info.Foreground = Res(brush);
            v.Root.Opacity = r.Pick == false || r.Why is not null ? 0.7 : 1;
            v.PutIn.Content = r.Pick == false ? T("Put back") : T("Put in");
            v.PutIn.Visibility = r.In ? Visibility.Collapsed : Visibility.Visible;
            v.LeaveOut.Visibility = r.Meets is not null && r.Pick != false ? Visibility.Visible : Visibility.Collapsed;
            v.Acts.Visibility = v.PutIn.Visibility == Visibility.Visible || v.LeaveOut.Visibility == Visibility.Visible ? Visibility.Visible : Visibility.Collapsed;
            v.Up.IsEnabled = at > 0;
            v.Down.IsEnabled = at < choices.Rows.Count - 1;
            AutomationProperties.SetName(v.PutIn, $"{v.PutIn.Content}: {name}");
            AutomationProperties.SetName(v.LeaveOut, $"{T("Leave out")}: {name}");
            AutomationProperties.SetName(v.Up, T("Move {0} up", name));
            AutomationProperties.SetName(v.Down, T("Move {0} down", name));
            if (v.Item is { } item) AutomationProperties.SetName(item, $"{at + 1}. {name}. {info}");
        }

        MergeRowView Make(Grid root)
        {
            var v = new MergeRowView(root);
            // (the button pressed leaves the row: the keyboard stays on the one that took its place)
            void Set(bool? pick)
            {
                if (v.Row is not { } r) return;
                r.Pick = pick == true && r.Meets is null ? null : pick;
                Changed(0);
                (v.LeaveOut.Visibility == Visibility.Visible ? v.LeaveOut : v.PutIn).Focus(FocusState.Programmatic);
            }
            v.PutIn.Click += (_, _) => Set(true);
            v.LeaveOut.Click += (_, _) => Set(false);
            v.Up.Click += (_, _) => Move(v.Row, -1, x => x.Up);
            v.Down.Click += (_, _) => Move(v.Row, 1, x => x.Down);
            views.Add(v);
            return v;
        }

        void Move(MergeChoice? r, int by, Func<MergeRowView, Button>? press)
        {
            var i = r is null ? -1 : choices.Rows.IndexOf(r);
            if (i < 0 || i + by < 0 || i + by >= choices.Rows.Count) return;
            choices.Rows.Move(i, i + by);
            list.SelectedItem = r;
            Changed(300);
            list.ScrollIntoView(r);
            FrameworkElementAutomationPeer.FromElement(list)?.RaiseNotificationEvent(AutomationNotificationKind.ActionCompleted, AutomationNotificationProcessing.MostRecent,
                T("{0} is now number {1}", Path.GetFileNameWithoutExtension(r!.Mod.Name), i + by + 1), "merge-order");
            // (the row is in another container now: the keyboard follows it)
            DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
            {
                if (closed || list.ContainerFromItem(r) is not ListViewItem item) return;
                if (press is not null && views.FirstOrDefault(v => ReferenceEquals(v.Row, r)) is { } v)
                    (press(v).IsEnabled ? press(v) : v.Up.IsEnabled ? v.Up : v.Down).Focus(FocusState.Programmatic);
                else item.Focus(FocusState.Keyboard);
            });
        }

        list.ContainerContentChanging += (_, e) =>
        {
            if (e.ItemContainer?.ContentTemplateRoot is not Grid root) return;
            var v = views.FirstOrDefault(x => x.Root == root);
            if (e.InRecycleQueue) { if (v is not null) v.Row = null; return; }
            v ??= Make(root);
            (v.Row, v.Item) = ((MergeChoice)e.Item, e.ItemContainer);
            Show(v);
            e.Handled = true;
        };
        list.DragItemsCompleted += (_, _) => Changed(100);
        // Alt+Up and Alt+Down move the row; Enter on a row is not the dialog's Merge
        list.PreviewKeyDown += (_, e) =>
        {
            var alt = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Menu).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
            if (!alt || e.Key is not (VirtualKey.Up or VirtualKey.Down) || list.SelectedItem is not MergeChoice r) return;
            e.Handled = true;
            Move(r, e.Key == VirtualKey.Up ? -1 : 1, null);
        };
        list.KeyDown += (_, e) => { if (e.Key == VirtualKey.Enter && e.OriginalSource is ListViewItem) e.Handled = true; };
        putAll.Click += (_, _) => Decide(r => r.Undecided, true);
        leaveAll.Click += (_, _) => Decide(r => r.Meets is not null, false);
        box.TextChanged += (_, _) => Changed(400);
        // (never a merge that is not the one on screen)
        d.PrimaryButtonClick += (_, e) => { if (Ready()) return; e.Cancel = true; Changed(0); };

        Safe(Replan());
        var answer = await ShowDialog(d);
        closed = true;
        timer.Stop();
        if (answer != ContentDialogResult.Primary || !Ready()) return null;
        var jobs = plans.Select(p => new MergeJob(Path.GetFileName(p.TargetRel), (progress, ct) => ModMerge.Run(p, progress, ct), () => p.LogPath)).ToList();
        if (scripts is { } own)
        {
            // (planned again, from the mods shown, once the first merge is in place: only then is their order against it
            // known, and a mod that conflicts with it and was not put in stays out)
            var (from, put) = (own.Inputs.ToList(), choices.Rows.Where(r => r.Pick == true).Select(r => r.Mod.Rel).ToList());
            MergePlan? late = null;
            jobs.Add(new MergeJob(Path.GetFileName(own.TargetRel), (progress, ct) =>
            {
                late = ModMerge.Plan(docs, from, own.Name, folder, true, put);
                if (late.Refusals.FirstOrDefault() is { } no) throw new SetupException(no.Key, no.Args);
                return ModMerge.Run(late, progress, ct);
            }, () => late?.LogPath));
        }
        return jobs;
    }

    // a row of the merge dialog: its parts, made once for each container and filled for the mod it shows
    sealed class MergeRowView
    {
        public Grid Root { get; }
        public TextBlock Number { get; } = new() { FontSize = 12, Foreground = Res("ApexMutedBrush"), VerticalAlignment = VerticalAlignment.Center, TextAlignment = TextAlignment.Right };
        public TextBlock Name { get; } = new() { FontSize = 13, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
        public TextBlock Info { get; } = new() { FontSize = 12, TextWrapping = TextWrapping.Wrap };
        public StackPanel Acts { get; } = new() { Orientation = Orientation.Horizontal, Spacing = 16 };
        public HyperlinkButton PutIn { get; } = new() { Padding = new Thickness(0, 3, 0, 3), FontSize = 12 };
        public HyperlinkButton LeaveOut { get; } = new() { Content = T("Leave out"), Padding = new Thickness(0, 3, 0, 3), FontSize = 12 };
        public Button Up { get; } = IconButton("chevron-down", T("Move up"));
        public Button Down { get; } = IconButton("chevron-down", T("Move down"));
        public MergeChoice? Row { get; set; }
        public FrameworkElement? Item { get; set; }

        public MergeRowView(Grid root)
        {
            Root = root;
            root.ColumnSpacing = 10;
            foreach (var w in new[] { GridLength.Auto, new GridLength(26), new GridLength(1, GridUnitType.Star), GridLength.Auto, GridLength.Auto }) root.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
            var up = (FrameworkElement)Up.Content;
            up.RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5);
            up.RenderTransform = new RotateTransform { Angle = 180 };
            Acts.Children.Add(PutIn);
            Acts.Children.Add(LeaveOut);
            var text = new StackPanel { Spacing = 1, VerticalAlignment = VerticalAlignment.Center };
            foreach (var e in new UIElement[] { Name, Info, Acts }) text.Children.Add(e);
            var grip = Lucide.Icon("grip-vertical", 14, Res("ApexIconMutedBrush"));
            grip.VerticalAlignment = VerticalAlignment.Center;
            var col = 0;
            foreach (var e in new[] { grip, Number, text, Up, Down }) { Grid.SetColumn(e, col++); root.Children.Add(e); }
        }
    }

    // ---------- newer versions of merged mods ----------

    List<MergeUpdate> _mergeNews = []; // the ones not put off with Later
    string _mergeNewsOf = "";
    (List<MergeUpdate> Updates, string Lang, bool Light)? _mergeBarShows;
    bool _mergeBarHooked;

    // The bar over the list: which mods have a newer file and which merge still uses the old one. A message on the Mods bar
    // or a hunt comes first: it shows when they are gone.
    void ShowMergeUpdates()
    {
        if (!_mergeBarHooked)
        {
            _mergeBarHooked = true;
            ModsInfo.RegisterPropertyChangedCallback(InfoBar.IsOpenProperty, (_, _) => ShowMergeUpdates());
        }
        var ups = _mergeNewsOf == Docs ? _mergeNews : [];
        var show = ups.Count > 0 && !ModsInfo.IsOpen && ModHunt.Current() is null;
        ModMergeBar.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        // (said already: a button in it may have the keyboard)
        if (!show || _mergeBarShows is { } was && ReferenceEquals(was.Updates, ups) && was.Lang == Lang && was.Light == _light) return;
        _mergeBarShows = (ups, Lang, _light);

        static string Plain(string file) => Path.GetFileNameWithoutExtension(file);
        var merges = ups.GroupBy(u => u.Merged.Rel, StringComparer.OrdinalIgnoreCase).Select(g => Plain(g.First().Merged.Name)).ToList();
        var names = string.Join(", ", merges.Take(3)) + (merges.Count > 3 ? " " + T("and {0} more", merges.Count - 3) : "");
        var title = ups.Count == 1 ? T("New version of {0}.", Plain(ups[0].Original.Rel)) : T("New versions of {0} mods.", ups.Count);
        var text = merges.Count > 1 ? T("The merges {0} still use the old ones.", names) : ups.Count == 1 ? T("The merge {0} still uses the old one.", names) : T("The merge {0} still uses the old ones.", names);

        var say = new TextBlock { TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center, FontSize = 13 };
        say.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = title + " ", FontWeight = FontWeights.SemiBold });
        say.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = text, Foreground = Res("ApexSoftTextBrush") });
        if (ups.Count > 1) ToolTipService.SetToolTip(say, string.Join("\n", ups.Take(20).Select(u => $"{Plain(u.Newer.Name)} ({Plain(u.Merged.Name)})")));
        var dot = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = Res("ApexWarningBrush"), VerticalAlignment = VerticalAlignment.Center };
        var acts = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        acts.Children.Add(Button(merges.Count == 1 ? T("Update merge") : T("Update merges"), () => Safe(UpdateMergesAsync(ups)), accent: true));
        acts.Children.Add(Button(T("Later"), () =>
        {
            ModViewPrefs.MergeLater = [.. ModViewPrefs.MergeLater, .. ups.Select(u => ModViewPrefs.LaterKey(u.Newer))];
            _mergeNews = [];
            ShowMergeUpdates();
        }));
        var g = new Grid { ColumnSpacing = 12 };
        foreach (var w in new[] { GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto }) g.ColumnDefinitions.Add(new ColumnDefinition { Width = w });
        foreach (var h in new[] { GridLength.Auto, GridLength.Auto }) g.RowDefinitions.Add(new RowDefinition { Height = h });
        Grid.SetColumn(say, 1);
        foreach (var e in new FrameworkElement[] { dot, say, acts }) g.Children.Add(e);
        // the buttons beside the text, or under it in a narrow list
        void Place(double width)
        {
            var narrow = width < 560;
            g.RowSpacing = narrow ? 10 : 0; // (one line: the empty second row adds no gap under it)
            Grid.SetRow(acts, narrow ? 1 : 0);
            Grid.SetColumn(acts, narrow ? 1 : 2);
            Grid.SetColumnSpan(acts, narrow ? 2 : 1);
            Grid.SetColumnSpan(say, narrow ? 2 : 1);
        }
        Place(ModMergeBar.ActualWidth);
        g.SizeChanged += (_, e) => Place(e.NewSize.Width);
        ModMergeBar.Child = g;
    }

    // each merge with its newer mods, as one checked change per merge; the first that fails stops the rest
    async Task UpdateMergesAsync(List<MergeUpdate> updates)
    {
        var docs = Docs;
        if (_mergeNewsOf != docs || AskLeaveMerge(() => Safe(UpdateMergesAsync(updates)))) return;
        var jobs = updates.GroupBy(u => u.Merged.Rel, StringComparer.OrdinalIgnoreCase).Select(g =>
        {
            MergeEditPlan? plan = null;
            return new MergeJob(g.First().Merged.Name, (progress, ct) =>
            {
                var edit = new MergeEdit();
                foreach (var u in g) edit.Replace[u.Original.Rel] = u.Newer;
                plan = ModMerge.PlanEdit(docs, g.First().Merged, edit);
                if (plan.Plan is null) throw new SetupException("There is nothing to update."); // (a merge of one mod would be undone, not updated)
                return ModMerge.Run(plan, progress, ct).Remerged!;
            }, () => plan?.Plan?.LogPath);
        }).ToList();
        string Title(List<MergeResult> r) => r.Count == 1 ? T("Merge {0} updated", Path.GetFileNameWithoutExtension(r[0].Merged.Name)) : T("{0} merges updated", r.Count);
        var (done, error) = await MergeRunDialog(jobs, Title);
        if (done.Count > 0)
            SetInfo(ModsInfo, error is null ? InfoBarSeverity.Success : InfoBarSeverity.Warning, Title(done),
                (error is null ? "" : T("The other merges were not updated.") + " ") + T("The old versions went to the Recycle Bin.") + " " + T("Something missing in the game? Clear its caches."),
                T("Clear the game caches"), async () => await ClearCachesAfterMerge());
        await ModsAgain();
    }
}
