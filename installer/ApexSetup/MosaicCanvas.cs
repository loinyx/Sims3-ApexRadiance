using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace ApexSetup;

// The memories' mosaic: only the cells near the part in sight exist, the rest is room (a save can hold many hundreds).
// make builds the cell at a place for its side in pixels.
public sealed class MosaicCanvas : Canvas
{
    const double Gap = 8;
    readonly int _count;
    readonly double _min;
    readonly Func<int, double, FrameworkElement> _make;
    readonly Dictionary<int, FrameworkElement> _made = [];
    List<(int Col, int Row, int Span)> _cells = [];
    int _cols;
    double _side;
    Rect _view;

    public MosaicCanvas(int count, double min, Func<int, double, FrameworkElement> make)
    {
        (_count, _min, _make) = (count, min, make);
        // (the cells are built as they come into sight: the order of the Tab key is theirs, not that of their making)
        TabFocusNavigation = KeyboardNavigationMode.Local;
        XYFocusKeyboardNavigation = XYFocusKeyboardNavigationMode.Enabled;
        SizeChanged += (_, e) => { if (e.NewSize.Width != e.PreviousSize.Width) Lay(e.NewSize.Width); };
        EffectiveViewportChanged += (_, e) => { _view = e.EffectiveViewport; Realise(); };
    }

    void Lay(double width)
    {
        if (width <= 0) return;
        var cols = CardGrid.Columns(width, _min, Gap);
        _side = (width - Gap * (cols - 1)) / cols;
        if (cols != _cols) (_cols, _cells) = (cols, SaveLayout.Mosaic(_count, cols));
        Height = _cells.Count == 0 ? 0 : _cells.Max(c => c.Row + c.Span) * (_side + Gap) - Gap;
        foreach (var (i, e) in _made) Put(i, e);
        Realise();
    }

    double Side(int i) => _cells[i].Span * (_side + Gap) - Gap;

    void Put(int i, FrameworkElement e)
    {
        e.Width = e.Height = Side(i);
        SetLeft(e, _cells[i].Col * (_side + Gap));
        SetTop(e, _cells[i].Row * (_side + Gap));
    }

    void Realise()
    {
        if (_cols == 0 || _view.Height <= 0) return;
        double top = _view.Y - _view.Height * 0.7, bottom = _view.Y + _view.Height * 1.7;
        var focus = XamlRoot is null ? null : FocusManager.GetFocusedElement(XamlRoot) as DependencyObject;
        while (focus is not null && VisualTreeHelper.GetParent(focus) is var up && up != this) focus = up;
        for (var i = 0; i < _count; i++)
        {
            var y = _cells[i].Row * (_side + Gap);
            var near = y + Side(i) >= top && y <= bottom;
            if (near == _made.TryGetValue(i, out var e)) continue;
            if (near)
            {
                _made[i] = e = _make(i, Side(i));
                Put(i, e);
                Children.Add(e);
            }
            else if (e != focus) // (the keyboard stays where it is)
            {
                Children.Remove(e);
                _made.Remove(i);
            }
        }
    }
}
