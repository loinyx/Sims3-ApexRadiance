using Windows.UI;

namespace ApexSetup.Design;

// The scrapbook's materials: papers, metal, film, stamps and the inks written on them. They are things on a board, not theme
// colours, so they look the same in every theme.
public static class Scrap
{
    static Color C(uint argb) => Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb);

    public static readonly Color
        PaperTop = C(0xFFFDFCF8), PaperBottom = C(0xFFEDE8DE), Print = C(0xFFFAF8F3), White = C(0xFFFFFFFF),
        CreamTop = C(0xFFF4EAD3), CreamBottom = C(0xFFE2D2AE), AgeSpot = C(0xFFA9864A), Sepia = C(0x4D8A5A20), Vignette = C(0x732A1A08), SepiaInk = C(0xFF5E4C30),
        KraftTop = C(0xFFC79C6C), KraftBottom = C(0xFFA97F52), KraftFibre = C(0xFF6E4A27), KraftFleck = C(0xFFF1DAB8), KraftLine = C(0x597A5531), KraftInk = C(0xFF3D2B1A),
        Ink = C(0xFF2B2A26), PhotoEdge = C(0x38000000), Sheen = C(0x42FFFFFF), SheenFaint = C(0x0DFFFFFF), Clear = C(0x00FFFFFF),
        WireShadow = C(0x40000000), WireLight = C(0x8CFFFFFF), SteelDark = C(0xFF7E848D), SteelLight = C(0xFFF4F6F8), SteelMid = C(0xFFC3C8CF), SteelEdge = C(0xFF8A9099),
        StampPaper = C(0xFFFBF8F1),
        FilmEdge = C(0xFF211D19), FilmBase = C(0xFF14110F), FilmPrint = C(0xFFE8A04A),
        IconShadowNear = C(0x2E000000), IconShadowFar = C(0x1A000000), IconSky = C(0xFF7FA9D6), IconHorizon = C(0xFFE9C9A4), IconField = C(0xFF6E8F6A), IconGround = C(0xFF3C5B43),
        MatWarm = C(0xFFF6F3EC), MatCool = C(0xFFF7F5F0), BevelTop = C(0xFFF2EEE6), BevelLeft = C(0xFFFBFAF6), BevelRight = C(0xFFD8D1C3), BevelBottom = C(0xFFCFC7B7),
        LipShadow = C(0x66000000), SideLight = C(0x1AFFFFFF), SideFaint = C(0x05FFFFFF), SideShade = C(0x38000000), SideDeep = C(0x52000000), Seam = C(0x59000000), WalnutGrain = C(0x591A0F07);

    // the profiles of the wooden mouldings, outer edge (0) to inner lip (1): light and shade across a rounded, carved side
    public static readonly (double At, Color Color)[]
        Walnut = [(0, C(0xFF1E1209)), (0.08, C(0xFF5A3820)), (0.2, C(0xFF8B5B36)), (0.32, C(0xFFA06A40)), (0.46, C(0xFF6E4426)), (0.6, C(0xFF82522E)), (0.78, C(0xFF5A361E)), (0.9, C(0xFF3A2212)), (1, C(0xFF1A0F07))],
        Ebony = [(0, C(0xFF030303)), (0.12, C(0xFF1E1E1E)), (0.25, C(0xFF3A3A3A)), (0.36, C(0xFF121212)), (0.7, C(0xFF1A1A1A)), (0.82, C(0xFF0A0A0A)), (0.86, C(0xFFB08A3E)), (0.9, C(0xFFF4DC98)), (0.94, C(0xFF8A6420)), (1, C(0xFF2A1C06))];

    // the two colours of an airmail border, one pair per stamp
    public static readonly (Color A, Color B)[] Airmail =
    [
        (C(0xFFC8383D), C(0xFF2F5FA5)), (C(0xFF2E7D5B), C(0xFFC8383D)), (C(0xFF2B3A67), C(0xFFD9A441)), (C(0xFFE06C9F), C(0xFF2F8F8A)),
    ];
}
