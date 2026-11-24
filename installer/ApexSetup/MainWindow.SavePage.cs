namespace ApexSetup;

// A save's own page. A placeholder until that page is merged: it opens the save's folder.
public sealed partial class MainWindow
{
    void OpenSave(Save s, string tab) => OpenFolder(s.Dir);
}
