using System.Runtime.CompilerServices;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace ApexSetup;

// The hand over what can be clicked: WinUI shows the arrow everywhere. Set the first time the pointer is over a control, so
// rows built later have it too.
public static class HandCursor
{
    static readonly ConditionalWeakTable<UIElement, object> Done = new();

    // the window's content and each dialog (a dialog is a popup outside it)
    public static void Watch(UIElement root) => root.AddHandler(UIElement.PointerMovedEvent, new PointerEventHandler(OnMoved), true);

    // something clickable that is no control, such as a picture
    public static void On(UIElement e)
    {
        if (Done.TryAdd(e, Done)) SetCursor(e, InputSystemCursor.Create(InputSystemCursorShape.Hand));
    }

    static void OnMoved(object sender, PointerRoutedEventArgs args)
    {
        for (var d = args.OriginalSource as DependencyObject; d is not null && !ReferenceEquals(d, sender); d = VisualTreeHelper.GetParent(d))
        {
            // (a scroll bar's parts are buttons too; a text box keeps its own cursor)
            if (d is ScrollBar or TextBox) return;
            if (d is ButtonBase or ToggleSwitch or ComboBox or NavigationViewItem or SelectorItem or SelectorBarItem) { On((UIElement)d); return; }
        }
    }

    // UIElement.ProtectedCursor is protected, and every control here would need a class of its own to reach it
    [UnsafeAccessor(UnsafeAccessorKind.Method, Name = "set_ProtectedCursor")]
    static extern void SetCursor(UIElement e, InputCursor cursor);
}
