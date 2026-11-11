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
        ["Setup can't reach GitHub"] = "O instalador não consegue acessar o GitHub",
        ["Check your internet connection, then try again."] = "Verifique a sua conexão e tente de novo.",
        ["Try again"] = "Tentar de novo",

        // game
        ["Choose your game"] = "Escolha o seu jogo",
        ["Setup found these installs. Pick the one you play."] = "O instalador encontrou estas instalações. Escolha a que você joga.",
        ["Setup found no install. Browse for the game folder."] = "Nenhuma instalação encontrada. Procure a pasta do jogo.",
        ["Browse for another folder"] = "Procurar outra pasta",
        ["No game in this folder"] = "Nenhum jogo nesta pasta",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Escolha a pasta do The Sims 3, ou a pasta Game\\Bin dele (a que tem o TS3W.exe ou o TS3.exe).",
        ["Not installed"] = "Não instalado",
        ["Installed"] = "Instalado",
        ["the game's own Direct3D 9"] = "o Direct3D 9 do próprio jogo",
        ["another d3d9.dll"] = "outra d3d9.dll",

        // components
        ["Required"] = "Obrigatório",
        ["Installs {0} into Game\\Bin."] = "Instala a {0} em Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] = "Substitui a cópia instalada pela {0}. As suas configurações ficam.",
        ["Updates {0} to {1}. Your settings stay."] = "Atualiza da {0} para a {1}. As suas configurações ficam.",
        ["Recommended with Apex"] = "Recomendados com o Apex",
        ["No updates available"] = "Nenhuma atualização disponível",
        ["Everything in this game is on its latest version."] = "Tudo neste jogo está na versão mais recente.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Indisponível nesta versão do jogo: ele suporta Steam 1.67.2 e EA app 1.69.47.",
        ["Wrong build"] = "Versão errada",
        ["Update {0}"] = "Atualizar {0}",
        ["Up to date"] = "Atualizado",

        // install and done
        ["Setup could not finish"] = "O instalador não conseguiu terminar",
        ["Close The Sims 3 first."] = "Feche The Sims 3 primeiro.",
        ["You're all set"] = "Tudo pronto",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Abra The Sims 3, carregue um mundo e aperte Ctrl+Shift+F11 para abrir o menu do Apex Radiance.",
        ["Installed: {0} {1}"] = "Instalado: {0} {1}",

        // repair or remove
        ["Choose your game first."] = "Escolha o seu jogo primeiro.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] = "Remove a ApexRadiance.asi. As suas configurações em Documentos ficam.",
        ["Remove"] = "Remover",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Remove o Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Remove o ts3native e o ts3fixes.package.",
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
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "O ShortWaitSpin e o ThreadStackDiet do Sims3Performance conflitam com o Sims 3 Modernization. O instalador põe os dois em 0 (fica uma cópia em Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "As texturas não comprimidas do Sims3SettingsSetter travam sob o RTX Remix bridge. O instalador as desliga (fica uma cópia em S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} define client.targetProcess = {1}, mas este jogo é o {2}: o bridge não inicia.",
        ["dxvk.conf lacks {0}: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "Falta {0} no dxvk.conf: sem isso o jogo redefine as opções gráficas ou congela ao trocar de resolução sob o bridge.",

        ["Fix settings"] = "Corrigir configurações",
        ["Everything is installed and up to date."] = "Tudo está instalado e atualizado.",
        ["Will be removed"] = "Será removido",
        ["Keep"] = "Manter",
        ["Nothing to apply"] = "Nada para aplicar",
        ["Preview"] = "Pré-visualização",
        ["This is a preview: nothing was installed."] = "Isto é uma pré-visualização: nada foi instalado.",
        ["Setup is applying changes"] = "O instalador está aplicando as mudanças",
        ["Wait until it finishes before closing."] = "Espere terminar antes de fechar.",
        ["Checking the latest versions…"] = "Verificando as últimas versões…",
        ["Checking for updates…"] = "Procurando atualizações…",
        ["Close The Sims 3 to update."] = "Feche o The Sims 3 para atualizar.",
        ["Update now installs them."] = "Atualizar agora instala tudo isso.",
        ["Graphics"] = "Gráficos",
        ["Checked at {0}."] = "Verificado às {0}.",
        ["Test build {0}: kept as it is."] = "Versão de teste {0}: fica como está.",
        ["Repair or remove sets the game right."] = "Reparar ou remover deixa o jogo como antes.",
        ["Changes applied"] = "Mudanças aplicadas",
        ["Removed: {0}"] = "Removido: {0}",
        ["The Sims 3 \u00b7 {0}"] = "The Sims 3 \u00b7 {0}",
        ["{0} \u2192 {1}"] = "{0} \u2192 {1}",
        ["{0} \u00b7 up to date"] = "{0} \u00b7 atualizado",
        ["Everything is in place."] = "Tudo certo.",
        ["Everything is in place (checked at {0})."] = "Tudo certo (verificado \u00e0s {0}).",
        ["Check installation"] = "Verificar instala\u00e7\u00e3o",
        ["Change components"] = "Mudar componentes",
        ["Apex Radiance is not installed in this game."] = "O Apex Radiance n\u00e3o est\u00e1 instalado neste jogo.",
        ["{0} \u00b7 test build"] = "{0} \u00b7 build de teste",
        ["Install the official {0}"] = "Instalar a vers\u00e3o oficial {0}",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] = "Nenhum ASI loader em Game\\Bin: o Apex Radiance e os outros mods .asi n\u00e3o carregam.",
        ["{0} is missing: the RTX Remix bridge does not start."] = "Falta {0}: o RTX Remix bridge n\u00e3o inicia.",
        ["Apply 1 change"] = "Aplicar 1 mudança",
        ["Apply {0} changes"] = "Aplicar {0} mudanças",
        ["{0} is installed and up to date: nothing to change."] = "A {0} está instalada e atualizada: nada a mudar.",
        ["Keep my d3d9.dll"] = "Manter a minha d3d9.dll",
        ["Your d3d9.dll stays as it is."] = "A sua d3d9.dll fica como está.",
        ["Applying changes"] = "Aplicando as mudanças",
        ["Remove {0}"] = "Remover {0}",
        ["Fix incompatible settings"] = "Corrigir configurações incompatíveis",
        ["Updates available"] = "Atualizações disponíveis",
        ["Update now"] = "Atualizar agora",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Remove o MonoPatcher.asi e o pacote dele.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] = "O TS3Patch.asi do Smooth Patch altera o mesmo código do jogo que o Smooth Patch do Sims3SettingsSetter. O instalador o move para a pasta de cópias (o S3SS faz o mesmo trabalho).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] = "O Gameplay Core Mod do simler90 e o NRaas StoryProgression substituem o mesmo código do jogo. Deixe só um deles.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] = "O MonoPatcher precisa do Microsoft Visual C++ Redistributable (x86), que não está instalado.",
        ["Download on Nexus Mods"] = "Baixar no Nexus Mods",
        ["Choose the downloaded file"] = "Escolher o arquivo baixado",
        ["Ready to install"] = "Pronto para instalar",
        ["Waiting for your download in {0}…"] = "Aguardando o seu download em {0}…",
        ["Not the Performance Patch"] = "Não é o Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] = "{0} não tem o Sims3Performance.asi. Se o download for .7z ou .rar, extraia e escolha o Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Remove o Sims3Performance.asi e o .ini dele.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] = "Baixe o Sims 3 Performance Patch no Nexus Mods primeiro.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Encontra o seu The Sims 3 (Steam ou EA app).",
        ["Installs Apex Radiance and the fixes you choose."] = "Instala o Apex Radiance e as correções que você escolher.",
        ["Keeps a copy of every file it changes, so you can undo everything."] = "Guarda uma cópia de cada arquivo que muda, para você poder desfazer tudo.",
        ["Your Apex Radiance settings are always kept."] = "As suas configurações do Apex Radiance são sempre mantidas.",
        ["No internet connection"] = "Sem conexão com a internet",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (recomendado)",
        ["Vulkan with more memory (RTX Remix bridge)"] = "Vulkan com mais memória (RTX Remix bridge)",
        ["Windows default (Direct3D 9)"] = "Padrão do Windows (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] = "Menos travamentos; o Apex Radiance é feito e testado com ele.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card."] = "Deixa uns 600 MB a mais de memória livre para mundos grandes. Precisa de uma placa de vídeo com Vulkan.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Funciona, mas o Apex Radiance é testado com o DXVK.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] = "As configurações gráficas do jogo podem voltar ao padrão uma vez. O instalador guarda uma cópia e a devolve se você voltar atrás.",
        ["Fixes for game and graphics settings."] = "Correções para as configurações do jogo e dos gráficos.",
        ["Fixes stutters, Sims getting stuck and crashes."] = "Corrige travamentos, Sims presos e crashes.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] = "Só alguns mods de script precisam dele; ligue se um mod pedir.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] = "Deixa o jogo mais rápido. Precisa de uma conta grátis no Nexus Mods; o instalador o instala quando o download terminar.",
        ["Puts back the graphics files you had before."] = "Devolve os arquivos gráficos que você tinha antes.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] = "Volta aos gráficos padrão do jogo. As configurações gráficas do jogo podem voltar ao padrão uma vez.",
        ["Language"] = "Idioma",
        // about and dialogs
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance por loinyx. O RTX Remix bridge é da NVIDIA, portado para The Sims 3 por dalexlol e alexvixell35-stack. O Sims3SettingsSetter é de sims3fiend, o Sims 3 Modernization de rodneypantonial e o Ultimate ASI Loader de ThirteenAG: o instalador os baixa das releases deles no GitHub.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: avisos de terceiros",
        ["Read"] = "Ler",
        ["OK"] = "OK",
        ["Cancel"] = "Cancelar",
    };
}
