// Translations of the texts the Violet widgets write themselves (ui/widgets.cpp; see ui/i18n.h). Part of Apex Radiance.
#include "ui/i18n.h"
#include <iterator>

namespace {

// {English (exactly as in the code), Portuguese (Brazil), Spanish, French}
const I18n::Entry kEntries[] = {
    {"See what is in use; click a resource to open its settings", "Veja o que está em uso; clique em um recurso para abrir seus ajustes", "Mira qué está en uso; pulsa un recurso para abrir sus ajustes", "Consultez les fonctions utilisées ; cliquez pour ouvrir leurs réglages"},
    {"Performance and screen", "Desempenho e tela", "Rendimiento y pantalla", "Performances et écran"},
    {"Waiting for game settings", "Aguardando ajuste do jogo", "Esperando un ajuste del juego", "En attente d'un réglage du jeu"},
    {"Restore the controls shown here? Other settings and saved files stay.", "Restaurar os controles desta página? Os outros ajustes e arquivos salvos serão mantidos.", "¿Restablecer los controles de esta página? Se conservan los demás ajustes y archivos guardados.", "Rétablir les commandes de cette page ? Les autres réglages et fichiers enregistrés seront conservés."},
    {"SMAA - Low", "SMAA · Baixa", "SMAA · Baja", "SMAA · Faible"},
    {"SMAA - Medium", "SMAA · Média", "SMAA · Media", "SMAA · Moyenne"},
    {"SMAA - High", "SMAA · Alta", "SMAA · Alta", "SMAA · Élevée"},
    {"SMAA - Ultra", "SMAA · Ultra", "SMAA · Ultra", "SMAA · Ultra"},
    {"SMAA - Extreme", "SMAA · Extrema", "SMAA · Extrema", "SMAA · Extrême"},
    {"FXAA - Fast", "FXAA · Rápida", "FXAA · Rápida", "FXAA · Rapide"},
    {"FXAA - Balanced", "FXAA · Equilibrada", "FXAA · Equilibrada", "FXAA · Équilibrée"},
    {"FXAA - High", "FXAA · Alta", "FXAA · Alta", "FXAA · Élevée"},
    {"FXAA - Extreme", "FXAA · Extrema", "FXAA · Extrema", "FXAA · Extrême"},
    {"Automatic focus", "Foco automático", "Enfoque automático", "Mise au point automatique"},
    {"Fixed focus", "Foco fixo", "Enfoque fijo", "Mise au point fixe"},
    {"Turn off the game's Edge Smoothing", "Desative a suavização de bordas do jogo", "Desactiva el suavizado de bordes del juego", "Désactivez le lissage des bords du jeu"},
    {"The Sims 3's Edge Smoothing prevents these enabled Apex effects from working:", "A suavização de bordas do The Sims 3 impede estes efeitos ativos do Apex de funcionar:", "El suavizado de The Sims 3 impide que funcionen estos efectos activos de Apex:", "Le lissage des Sims 3 empêche ces effets Apex activés de fonctionner :"},
    {"How to turn it off", "Como desativar", "Cómo desactivarlo", "Comment le désactiver"},
    {"1. Open The Sims 3 menu and choose Options > Graphics.", "1. Abra o menu do The Sims 3 e entre em Opções > Gráficos.", "1. Abre el menú de Los Sims 3 y entra en Opciones > Gráficos.", "1. Ouvrez le menu des Sims 3, puis Options > Graphismes."},
    {"2. Set Edge Smoothing to Off and apply the change.", "2. Desative a suavização de bordas e aplique a alteração.", "2. Desactiva el suavizado de bordes y aplica el cambio.", "2. Désactivez le lissage des bords et appliquez le changement."},
    {"3. Return to the game. Apex will check compatibility again.", "3. Volte ao jogo. O Apex verificará a compatibilidade novamente.", "3. Vuelve al juego. Apex comprobará la compatibilidad de nuevo.", "3. Revenez au jeu. Apex vérifiera à nouveau la compatibilité."},
    {"Your Apex settings are kept. This notice disappears when the conflict is resolved.", "Suas configurações do Apex são mantidas. O aviso desaparece quando o conflito é resolvido.", "Se mantienen tus ajustes de Apex. El aviso desaparece al resolver el conflicto.", "Vos réglages Apex sont conservés. Cet avis disparaît une fois le conflit résolu."},

    {"Advanced", "Avançado", "Avanzado", "Avancé"},
    {"Open this page", "Abrir esta página", "Abrir esta página", "Ouvrir cette page"},
    {"Changed from the default", "Diferente do padrão", "Distinto del valor predeterminado", "Modifié par rapport à la valeur par défaut"},
    {"Reset to default", "Voltar ao padrão", "Restablecer valor predeterminado", "Rétablir la valeur par défaut"},
    {"Measured cost on your GPU per frame", "Custo medido na sua GPU por quadro", "Costo medido en tu GPU por fotograma", "Coût mesuré sur votre GPU par image"},
    {"{} turned on", "{} ligado", "{} activado", "{} activé"},
    {"{} turned off", "{} desligado", "{} desactivado", "{} désactivé"},
    {"{} reset", "{} voltou ao padrão", "{} restablecido", "{} réinitialisé"},
    {"{} changed", "{} alterado", "{} cambiado", "{} modifié"},
    {"Pick a color", "Escolher uma cor", "Elegir un color", "Choisir une couleur"},
    {"Automatic ({})", "Automático ({})", "Automático ({})", "Automatique ({})"},
    {"Language", "Idioma", "Idioma", "Langue"},
    {"The language of this menu", "O idioma deste menu", "El idioma de este menú", "La langue de ce menu"},
    {"Language, menu key, text size and saving", "Idioma, tecla do menu, tamanho do texto e salvamento", "Idioma, tecla del menú, tamaño del texto y guardado",
     "Langue, touche du menu, taille du texte et sauvegarde"},
};
const I18n::Table kTable(kEntries, std::size(kEntries));

} // namespace
