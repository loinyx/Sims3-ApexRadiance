namespace ApexSetup.Design;

// The design system's measures. A screen takes its distances, corners, text sizes and times from here; a number of its own in a
// screen is a mistake. docs/launcher-design.md says when each one is used.
public static class Space
{
    public const double X1 = 4, X2 = 8, X3 = 12, X4 = 16, X5 = 20, X6 = 24, X8 = 32, X10 = 40, X12 = 48, X16 = 64;

    public const double Page = X8;          // a page's own margin, on every side
    public const double TitleToSub = X1;    // a title and the line under it
    public const double HeadToBody = X6;    // a page's or a card's head and what it opens
    public const double Section = X8;       // between two sections
    public const double SectionToBody = X3; // a section's title and its content
    public const double Grid = X3;          // between cards side by side or stacked
    public const double CardPad = X5;       // a card's padding, the same on its four sides
    public const double RowPad = X5;        // a row's padding at its two ends
    public const double IconToText = X4;    // an icon and the words beside it
}

public static class Radius
{
    public const double Tag = 6, Control = 8, Tile = 10, Card = 12, Panel = 16;
}

public static class TextSize
{
    public const double Micro = 11, Caption = 12, Small = 13, Body = 14, Lead = 16, Title3 = 20, Title2 = 24, Title1 = 28, Hero = 40;
}

public static class Size
{
    public const double ControlSmall = 32, Control = 36, ControlLarge = 40, SidePanel = 368, Row = 68, Tile = 40, Icon = 20, IconSmall = 16, Page = 960, Dialog = 720;
}

public static class Motion
{
    public const int Fast = 120, Base = 200, Slow = 320;
}
