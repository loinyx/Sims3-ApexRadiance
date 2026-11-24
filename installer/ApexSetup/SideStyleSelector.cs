using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace ApexSetup;

// The Mods side pane: its group headings (Tag "-") get a plain container with no hover or selection; places get the list's
// own row
public sealed partial class SideStyleSelector : StyleSelector
{
    public Style? Item { get; set; }
    public Style? Header { get; set; }

    protected override Style? SelectStyleCore(object item, DependencyObject container) =>
        item is FrameworkElement { Tag: "-" } ? Header : Item;
}
