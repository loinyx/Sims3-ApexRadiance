using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace ApexSetup;

// English is the key, as in the mod's menu (ui/i18n.h): T("English") returns the Portuguese text when Portuguese is on,
// and English for a text without a pair. Static XAML texts are translated in place by Localize.
public static class Strings
{
    public static bool Pt { get; set; }

    public static string T(string en, params object[] args)
    {
        var s = Pt && PtBr.TryGetValue(en, out var t) ? t : en;
        return args.Length == 0 ? s : string.Format(s, args);
    }

    public static void Localize(object? e)
    {
        switch (e)
        {
            case FrameworkElement { Tag: "dyn" }:
                return;
            case TextBlock tb:
                tb.Text = T(Remember(tb, tb.Text));
                break;
            case NavigationView nv:
                foreach (var i in nv.MenuItems.Concat(nv.FooterMenuItems)) Localize(i);
                Localize(nv.Content);
                break;
            case ScrollViewer sv:
                Localize(sv.Content);
                break;
            case ContentControl cc when cc.Content is string s:
                cc.Content = T(Remember(cc, s));
                break;
            case ContentControl cc:
                Localize(cc.Content);
                break;
            case Panel p:
                foreach (var c in p.Children) Localize(c);
                break;
            case Border b:
                Localize(b.Child);
                break;
        }
    }

    // the English key of a static text shown in either language (Tag="dyn" elements are set from code instead)
    static Dictionary<string, string>? _english;
    static string Remember(object o, string current)
    {
        _english ??= PtBr.GroupBy(p => p.Value).ToDictionary(g => g.Key, g => g.First().Key);
        return _english.TryGetValue(current, out var en) ? en : current;
    }

    static readonly Dictionary<string, string> PtBr = new()
    {
        // navigation and footer
        ["Welcome"] = "Início",
        ["Game"] = "Jogo",
        ["Components"] = "Componentes",
        ["Install"] = "Instalar",
        ["Done"] = "Pronto",
        ["Repair or remove"] = "Reparar ou remover",
        ["About and licences"] = "Sobre e licenças",
        ["Back"] = "Voltar",
        ["Next"] = "Avançar",
        ["Get started"] = "Começar",
        ["Finish"] = "Concluir",
        ["Close The Sims 3 to continue."] = "Feche The Sims 3 para continuar.",
        ["Setup needs the internet to download the files."] = "O instalador precisa da internet para baixar os arquivos.",
        ["Nothing changes until you click Install."] = "Nada muda até você clicar em Instalar.",

        // welcome
        ["Lighting, picture and performance for The Sims 3."] = "Iluminação, imagem e desempenho para The Sims 3.",
        ["What Setup does"] = "O que o instalador faz",
        ["Finds your game: Steam 1.67.2 or EA app 1.69."] = "Encontra o seu jogo: Steam 1.67.2 ou EA app 1.69.",
        ["Installs or updates Apex Radiance and, if you want them, the RTX Remix bridge, Sims3SettingsSetter and Sims 3 Modernization, from their official GitHub releases."] =
            "Instala ou atualiza o Apex Radiance e, se você quiser, o RTX Remix bridge, o Sims3SettingsSetter e o Sims 3 Modernization, a partir das releases oficiais no GitHub.",
        ["Keeps a copy of every file it replaces, so Remove puts your game back as it was."] =
            "Guarda uma cópia de cada arquivo que substitui, para Remover deixar o jogo como estava.",
        ["Your settings in Documents\\Electronic Arts\\The Sims 3\\Apex Radiance stay untouched."] =
            "As suas configurações em Documentos\\Electronic Arts\\The Sims 3\\Apex Radiance não são tocadas.",
        ["Setup can't reach GitHub"] = "O instalador não consegue acessar o GitHub",
        ["Check your internet connection, then try again."] = "Verifique a sua conexão e tente de novo.",
        ["Try again"] = "Tentar de novo",
        ["Update available"] = "Atualização disponível",
        ["Apex Radiance {0} is installed; {1} is out."] = "O Apex Radiance {0} está instalado; a {1} já saiu.",

        // game
        ["Choose your game"] = "Escolha o seu jogo",
        ["Setup found these installs. Pick the one you play."] = "O instalador encontrou estas instalações. Escolha a que você joga.",
        ["Setup found no install. Browse for the game folder."] = "Nenhuma instalação encontrada. Procure a pasta do jogo.",
        ["Browse for another folder"] = "Procurar outra pasta",
        ["No game in this folder"] = "Nenhum jogo nesta pasta",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Escolha a pasta do The Sims 3, ou a pasta Game\\Bin dele (a que tem o TS3W.exe ou o TS3.exe).",
        ["The game is closed"] = "O jogo está fechado",
        ["The game is running: close it"] = "O jogo está aberto: feche-o",
        ["No ASI loader yet"] = "Ainda sem ASI loader",
        ["ASI loader: {0}"] = "ASI loader: {0}",
        ["Graphics: {0}"] = "Gráficos: {0}",
        ["Installed in this game"] = "Instalado neste jogo",
        ["Not installed"] = "Não instalado",
        ["Installed"] = "Instalado",
        ["{0} · update to {1}"] = "{0} · atualizar para {1}",
        ["the game's own Direct3D 9"] = "o Direct3D 9 do próprio jogo",
        ["another d3d9.dll"] = "outra d3d9.dll",
        ["An older combined build ({0}) is in Game\\Bin. Setup removes it."] = "Há uma versão combinada antiga ({0}) em Game\\Bin. O instalador a remove.",
        ["A second ApexRadiance.asi is in Game\\Bin\\{0}. Setup removes it."] = "Há uma segunda ApexRadiance.asi em Game\\Bin\\{0}. O instalador a remove.",
        ["Two ASI loaders ({0}) would load every mod twice. Keep one."] = "Dois ASI loaders ({0}) carregariam cada mod duas vezes. Deixe só um.",
        ["ts3native.log says the installed Sims 3 Modernization is the build for the other edition. Setup installs the right one when you turn it on."] =
            "O ts3native.log diz que o Sims 3 Modernization instalado é a versão da outra edição. O instalador põe a certa quando você o liga.",

        // components
        ["Required"] = "Obrigatório",
        ["Installs {0} into Game\\Bin."] = "Instala a {0} em Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] = "Substitui a cópia instalada pela {0}. As suas configurações ficam.",
        ["Updates {0} to {1}. Your settings stay."] = "Atualiza da {0} para a {1}. As suas configurações ficam.",
        ["{0} is installed and up to date. Setup copies it again."] = "A {0} está instalada e atualizada. O instalador copia de novo.",
        ["RTX Remix bridge · optional"] = "RTX Remix bridge · opcional",
        ["Runs the renderer in its own 64-bit process: about 600 MB more free memory for long sessions and big worlds. Same picture as DXVK."] =
            "Roda o renderizador num processo próprio de 64 bits: uns 600 MB a mais de memória livre para sessões longas e mundos grandes. A mesma imagem do DXVK.",
        ["Installed: Setup updates it."] = "Instalado: o instalador atualiza.",
        ["Adds d3d9.dll to Game\\Bin; Remove takes it out again."] = "Adiciona a d3d9.dll em Game\\Bin; Remover a tira de novo.",
        ["Replaces your d3d9.dll ({0}); Setup keeps it and puts it back when you remove the bridge."] =
            "Substitui a sua d3d9.dll ({0}); o instalador a guarda e a devolve quando você remove o bridge.",
        ["Set for {0}. Needs a Vulkan graphics card, like DXVK."] = "Configurado para o {0}. Precisa de uma placa de vídeo com Vulkan, como o DXVK.",
        ["Graphics options will reset once"] = "As opções gráficas vão ser redefinidas uma vez",
        ["The game resets its graphics options when it sees a different graphics card. The bridge reports a GeForce GTX 580, like DXVK; your game now uses {0}. Setup keeps a copy of Options.ini and puts it back if you remove the bridge."] =
            "O jogo redefine as opções gráficas quando vê outra placa de vídeo. O bridge informa uma GeForce GTX 580, como o DXVK; o seu jogo hoje usa {0}. O instalador guarda uma cópia do Options.ini e a devolve se você remover o bridge.",
        ["Your graphics options stay"] = "As suas opções gráficas ficam",
        ["DXVK and the bridge both report a GeForce GTX 580, so the game keeps its settings."] =
            "O DXVK e o bridge informam a mesma GeForce GTX 580, então o jogo mantém as configurações.",
        ["Also available"] = "Também disponível",
        ["Game and graphics settings fixes by sims3fiend, downloaded from its GitHub release."] =
            "Correções de configurações do jogo e dos gráficos, por sims3fiend, baixadas da release dele no GitHub.",
        ["Stutter, route and crash fixes by rodneypantonial: ts3native in Game\\Bin and ts3fixes.package in Mods\\Packages, from its GitHub release."] =
            "Correções de travadas, rotas e crashes, por rodneypantonial: ts3native em Game\\Bin e ts3fixes.package em Mods\\Packages, da release dele no GitHub.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Indisponível nesta versão do jogo: ele suporta Steam 1.67.2 e EA app 1.69.47.",
        ["Official page"] = "Página oficial",
        ["Wrong build"] = "Versão errada",
        ["Update {0}"] = "Atualizar {0}",
        ["Up to date"] = "Atualizado",
        ["ASI loader found: {0}."] = "ASI loader encontrado: {0}.",
        ["Your ASI loader is d3d9.dll, which the bridge replaces: Setup adds Ultimate ASI Loader as wininet.dll."] =
            "O seu ASI loader é a d3d9.dll, que o bridge substitui: o instalador adiciona o Ultimate ASI Loader como wininet.dll.",
        ["No ASI loader found: Setup adds Ultimate ASI Loader as wininet.dll, from its GitHub release."] =
            "Nenhum ASI loader encontrado: o instalador adiciona o Ultimate ASI Loader como wininet.dll, da release dele no GitHub.",

        // install and done
        ["Installing"] = "Instalando",
        ["Setup could not finish"] = "O instalador não conseguiu terminar",
        ["{0}\n\nWhat was installed before the error can be removed in Repair or remove."] =
            "{0}\n\nO que foi instalado antes do erro pode ser removido em Reparar ou remover.",
        ["Close The Sims 3 first."] = "Feche The Sims 3 primeiro.",
        ["You're all set"] = "Tudo pronto",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Abra The Sims 3, carregue um mundo e aperte Ctrl+Shift+F11 para abrir o menu do Apex Radiance.",
        ["Installed: {0} {1}"] = "Instalado: {0} {1}",
        ["Copies of replaced files: {0}"] = "Cópias dos arquivos substituídos: {0}",
        ["A new Setup is ready"] = "Há um novo instalador pronto",
        ["Apex Radiance Setup {0} was downloaded and checked. Restart Setup to install with it."] =
            "O Apex Radiance Setup {0} foi baixado e verificado. Reinicie o instalador para instalar com ele.",
        ["Restart now"] = "Reiniciar agora",
        ["Open Game\\Bin"] = "Abrir Game\\Bin",

        // repair or remove
        ["Choose your game first."] = "Escolha o seu jogo primeiro.",
        ["The Sims 3 · {0} · {1}"] = "The Sims 3 · {0} · {1}",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] = "Remove a ApexRadiance.asi. As suas configurações em Documentos ficam.",
        ["Remove"] = "Remover",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Puts your previous d3d9.dll back. Apex Radiance stays."] = "Devolve a sua d3d9.dll anterior. O Apex Radiance fica.",
        ["The game goes back to its own Direct3D 9; Setup puts your Options.ini copy back."] =
            "O jogo volta ao próprio Direct3D 9; o instalador devolve a cópia do seu Options.ini.",
        ["Setup has no copy of your previous d3d9.dll: the game goes back to its own Direct3D 9 and resets its graphics options once."] =
            "O instalador não tem cópia da sua d3d9.dll anterior: o jogo volta ao próprio Direct3D 9 e redefine as opções gráficas uma vez.",
        ["Removes Sims3SettingsSetter.asi."] = "Remove o Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Remove o ts3native e o ts3fixes.package.",
        ["Setup added it as wininet.dll. Without a loader no .asi mod loads."] = "O instalador o adicionou como wininet.dll. Sem um loader nenhum mod .asi carrega.",
        ["Repair or update"] = "Reparar ou atualizar",
        ["Downloads and copies again everything installed here. Your settings stay."] = "Baixa e copia de novo tudo o que está instalado aqui. As suas configurações ficam.",
        ["Repair"] = "Reparar",
        ["Uninstall everything"] = "Desinstalar tudo",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Remove o que o instalador instalou e o Apex Radiance, e devolve cada arquivo que o instalador substituiu.",
        ["Uninstall"] = "Desinstalar",
        ["Remove {0}?"] = "Remover {0}?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Feche o jogo antes. O instalador remove os arquivos e devolve os que substituiu.",
        ["{0} removed."] = "{0} removido.",
        ["Uninstall everything?"] = "Desinstalar tudo?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "O instalador remove o Apex Radiance e tudo o que instalou neste jogo, e devolve cada arquivo que substituiu. As suas configurações em Documentos ficam.",
        ["Everything Setup installed was removed."] = "Tudo o que o instalador instalou foi removido.",

        // compatibility checks (ApexSetup.Core/Compat.cs)
        ["Setup fixes this"] = "O instalador corrige isto",
        ["Read more"] = "Saiba mais",
        ["Fix all now"] = "Corrigir tudo agora",
        ["Fixed."] = "Corrigido.",
        ["No incompatible settings found."] = "Nenhuma configuração incompatível encontrada.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] = "{0} em Game\\Bin carrega uma segunda cópia do Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] = "Dois ASI loaders ({0}) carregam cada mod duas vezes. Fica só um: o instalador deixa o {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "O ts3native do Sims 3 Modernization é a versão da outra edição e não faz nada. Ligue-o em Componentes para instalar a certa.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "A pasta Mods tem {0} arquivos .package mas nenhum Resource.cfg, então o jogo não carrega nenhum.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "{0} é a versão para a outra edição do jogo; ela faz o jogo mostrar \"unofficial game modification\". Use a versão {1}.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] = "O NRaas ErrorTrap não tem versão conhecida que funcione no EA app 1.69.47.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "{0} substitui código do jogo que o Sims 3 Modernization também altera (não testados juntos).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "{0} é uma versão antiga com o Apex embutido; enquanto estiver em Game\\Bin, o Apex Radiance fica desligado. O instalador a move para a pasta de cópias (instale o Sims3SettingsSetter oficial no lugar, se você o usava).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept in the backup folder)."] =
            "O ShortWaitSpin e o ThreadStackDiet do Sims3Performance conflitam com o Sims 3 Modernization. O instalador põe os dois em 0 (fica uma cópia na pasta de backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept in the backup folder)."] =
            "As texturas não comprimidas do Sims3SettingsSetter travam sob o RTX Remix bridge. O instalador as desliga (fica uma cópia na pasta de backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} define client.targetProcess = {1}, mas este jogo é o {2}: o bridge não inicia.",
        ["dxvk.conf lacks {0}: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "Falta {0} no dxvk.conf: sem isso o jogo redefine as opções gráficas ou congela ao trocar de resolução sob o bridge.",

        ["Fix settings"] = "Corrigir configurações",
        ["Everything is installed and up to date."] = "Tudo está instalado e atualizado.",
        ["Setup changes only what is listed. Nothing changes until you click Install."] = "O instalador muda só o que está listado. Nada muda até você clicar em Instalar.",
        ["{0} is installed and up to date: nothing to change."] = "A {0} está instalada e atualizada: nada a mudar.",
        ["Installed and kept as it is. Turn on to copy the latest files again."] = "Instalado e mantido como está. Ligue para copiar de novo os arquivos mais recentes.",
        ["Installed: Setup copies the latest files."] = "Instalado: o instalador copia os arquivos mais recentes.",
        ["DXVK (recommended)"] = "DXVK (recomendado)",
        ["RTX Remix bridge (64-bit, about 600 MB more free memory)"] = "RTX Remix bridge (64 bits, uns 600 MB a mais de memória livre)",
        ["The game's own Direct3D 9"] = "O Direct3D 9 do próprio jogo",
        ["Keep my d3d9.dll"] = "Manter a minha d3d9.dll",
        ["Renderer · now {0}"] = "Renderizador · hoje {0}",
        ["How the game draws. DXVK and the bridge need a Vulkan graphics card."] = "Como o jogo desenha. O DXVK e o bridge precisam de uma placa de vídeo com Vulkan.",
        ["Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it. From DXVK's GitHub release."] = "Roda o jogo em Vulkan: menos travadas, e o Apex Radiance é feito e testado com ele. Da release do DXVK no GitHub.",
        ["DXVK in its own 64-bit process: the game keeps about 600 MB more free memory. Set for {0}; needs a Vulkan graphics card."] = "O DXVK num processo próprio de 64 bits: o jogo fica com uns 600 MB a mais de memória livre. Configurado para o {0}; precisa de uma placa de vídeo com Vulkan.",
        ["Windows' Direct3D 9. Apex Radiance works with it, but it is made and tested with DXVK."] = "O Direct3D 9 do Windows. O Apex Radiance funciona com ele, mas é feito e testado com o DXVK.",
        ["Your d3d9.dll stays as it is."] = "A sua d3d9.dll fica como está.",
        ["Setup keeps your current d3d9.dll and puts it back if you remove the new one."] = "O instalador guarda a sua d3d9.dll atual e a devolve se você remover a nova.",
        ["Graphics options may reset once"] = "As opções gráficas podem ser redefinidas uma vez",
        ["The game resets its graphics options when the graphics card it sees changes, and {0} reports a different one than {1}. Setup keeps a copy of Options.ini and puts it back if you go back."] = "O jogo redefine as opções gráficas quando a placa de vídeo que ele vê muda, e {0} informa uma diferente de {1}. O instalador guarda uma cópia do Options.ini e a devolve se você voltar atrás.",
        ["Your ASI loader is d3d9.dll, which the new renderer replaces: Setup adds Ultimate ASI Loader as wininet.dll."] = "O seu ASI loader é a d3d9.dll, que o novo renderizador substitui: o instalador adiciona o Ultimate ASI Loader como wininet.dll.",
        ["Apply"] = "Aplicar",
        ["Setup changes only what differs from your game now: {0}."] = "O instalador muda só o que difere do seu jogo hoje: {0}.",
        ["remove {0}"] = "remover {0}",
        ["Applying changes"] = "Aplicando as mudanças",
        ["Remove {0}"] = "Remover {0}",
        ["Fix incompatible settings"] = "Corrigir configurações incompatíveis",
        ["Puts your previous d3d9.dll back."] = "Devolve a sua d3d9.dll anterior.",
        ["The game goes back to its own Direct3D 9 and may reset its graphics options once."] = "O jogo volta ao próprio Direct3D 9 e pode redefinir as opções gráficas uma vez.",
        ["Updates available"] = "Atualizações disponíveis",
        ["Update now"] = "Atualizar agora",
        ["{0} in The Sims 3 · {1}."] = "{0} no The Sims 3 · {1}.",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Lets script mods patch the game's code, by LazyDuchess; several community fixes need it. Downloaded from its GitHub release."] = "Permite que mods de script alterem o código do jogo, por LazyDuchess; várias correções da comunidade precisam dele. Baixado da release dele no GitHub.",
        ["Installed · official page"] = "Instalado · página oficial",
        ["Removes MonoPatcher.asi and its package."] = "Remove o MonoPatcher.asi e o pacote dele.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] = "O TS3Patch.asi do Smooth Patch altera o mesmo código do jogo que o Smooth Patch do Sims3SettingsSetter. O instalador o move para a pasta de cópias (o S3SS faz o mesmo trabalho).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] = "O Gameplay Core Mod do simler90 e o NRaas StoryProgression substituem o mesmo código do jogo. Deixe só um deles.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] = "O MonoPatcher precisa do Microsoft Visual C++ Redistributable (x86), que não está instalado.",
        ["Performance patches for the game's code. Published on Nexus Mods only, which needs a free account: Setup opens its page, and when your download finishes it finds the file in Downloads and installs it."] = "Patches de desempenho para o código do jogo. Publicado só no Nexus Mods, que pede uma conta grátis: o instalador abre a página e, quando o download termina, encontra o arquivo em Downloads e o instala.",
        ["Download on Nexus Mods"] = "Baixar no Nexus Mods",
        ["Choose the downloaded file"] = "Escolher o arquivo baixado",
        ["Ready to install"] = "Pronto para instalar",
        ["Waiting for your download in {0}…"] = "Aguardando o seu download em {0}…",
        ["Not the Performance Patch"] = "Não é o Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] = "{0} não tem o Sims3Performance.asi. Se o download for .7z ou .rar, extraia e escolha o Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Remove o Sims3Performance.asi e o .ini dele.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] = "Baixe o Sims 3 Performance Patch no Nexus Mods primeiro.",
        // about and dialogs
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance por loinyx. O RTX Remix bridge é da NVIDIA, portado para The Sims 3 por dalexlol e alexvixell35-stack. O Sims3SettingsSetter é de sims3fiend, o Sims 3 Modernization de rodneypantonial e o Ultimate ASI Loader de ThirteenAG: o instalador os baixa das releases deles no GitHub.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: avisos de terceiros",
        ["Read"] = "Ler",
        ["OK"] = "OK",
        ["Cancel"] = "Cancelar",
    };
}
