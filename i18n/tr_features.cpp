// Translations of the menu texts (see ui/i18n.h). Part of Apex Radiance.
// Performance features' descriptions (patches/performance_patches.cpp, shown on hover), the "Not available" note
// (framework/patch_base.cpp, ApexPatch::UnavailableReason) and the Sims3SettingsSetter detection line
// (framework/s3ss_detect.cpp, S3SSDetect::Summary).
#include "ui/i18n.h"
#include "apex_version.h"
#include <iterator>

namespace {

// {English (exactly as in the code), Portuguese (Brazil), Spanish, French}
const I18n::Entry kEntries[] = {
    {"S3SS limits FPS; disable its FPS limit and restart to use Apex's limiter", "O S3SS limita os FPS; desative esse limite e reinicie para usar o limitador do Apex", "S3SS limita los FPS; desactiva ese límite y reinicia para usar el limitador de Apex", "S3SS limite les FPS ; désactivez cette limite et redémarrez pour utiliser celle d’Apex"},
    {"Keep current", "Preservar atual", "Mantener actual", "Conserver actuel"},
    {"Changes apply when you restart the game", "As mudanças entram em vigor ao reiniciar o jogo", "Los cambios se aplican al reiniciar el juego", "Les modifications prennent effet au redémarrage du jeu"},
    {"Limit FPS with Apex", "Limitar FPS com o Apex", "Limitar FPS con Apex", "Limiter les FPS avec Apex"},
    {"Use only one FPS limiter: Apex, DXVK, or your graphics driver", "Use apenas um limitador de FPS: Apex, DXVK ou o driver de vídeo", "Usa un solo limitador de FPS: Apex, DXVK o el controlador gráfico", "Utilisez un seul limiteur de FPS : Apex, DXVK ou le pilote graphique"},
    {"Choose a rate the game can sustain, below your monitor's maximum refresh rate", "Escolha uma taxa que o jogo consiga manter, abaixo da frequência máxima do monitor", "Elige una tasa que el juego pueda mantener, inferior a la frecuencia máxima del monitor", "Choisissez une cadence que le jeu peut maintenir, sous la fréquence maximale de l’écran"},
    {"Target frame rate", "Limite de quadros", "Límite de fotogramas", "Limite d’images"},
    {"Apex wait: {:.2f} ms | Present: {:.2f} ms", "Espera do Apex: {:.2f} ms | Present: {:.2f} ms", "Espera de Apex: {:.2f} ms | Present: {:.2f} ms", "Attente Apex : {:.2f} ms | Present : {:.2f} ms"},
    {"Enable G-SYNC or FreeSync in your graphics driver; Apex does not activate it", "Ative G-SYNC ou FreeSync no driver de vídeo; o Apex não ativa essa função", "Activa G-SYNC o FreeSync en el controlador gráfico; Apex no lo activa", "Activez G-SYNC ou FreeSync dans le pilote graphique ; Apex ne l’active pas"},
    {"DXVK or driver settings may override V-Sync; this cannot guarantee flicker-free output", "O DXVK ou o driver pode sobrescrever o V-Sync; isso não garante eliminar cintilações", "DXVK o el controlador puede sobrescribir V-Sync; esto no garantiza eliminar el parpadeo", "DXVK ou le pilote peut remplacer V-Sync ; cela ne garantit pas l’absence de scintillement"},
    {"Restore synchronization", "Restaurar sincronização", "Restaurar sincronización", "Rétablir la synchronisation"},
    {"V-Sync", "V-Sync", "V-Sync", "V-Sync"},
    // ---- Performance feature descriptions (hover) ----
    {"Keeps nearby lots in full detail farther away; uses more memory. Validated baseline: distance 300. "
     "Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Mantém os lotes próximos em detalhe completo a uma distância maior; usa mais memória. Base validada: distância 300. "
     "Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Mantiene los solares cercanos con detalle completo a mayor distancia; usa más memoria. Base validada: distancia 300. "
     "Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Garde les terrains proches en détail complet à plus grande distance ; utilise plus de mémoire. Base validée : distance 300. "
     "Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Pauses lot-detail streaming while the neighborhood map is open, then resumes it after the map closes. This avoids doing lot "
     "streaming work during the map transition. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Pausa o streaming de detalhes dos lotes enquanto o mapa da vizinhança está aberto e retoma depois que ele fecha. Isso evita trabalho de "
     "streaming de lotes durante a transição do mapa. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Pausa el streaming de detalle de solares mientras el mapa del barrio está abierto y lo reanuda al cerrarlo. Esto evita trabajo de "
     "streaming de solares durante la transición del mapa. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Suspend le streaming détaillé des terrains pendant l'ouverture de la carte du quartier, puis le reprend à sa fermeture. Cela évite du "
     "travail de streaming pendant la transition de la carte. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"When more lots are in range than the game keeps in full detail, a lot already in full detail keeps it until a lot about 1.4 times nearer needs it, so lots switch back and forth less while the camera moves. "
     "Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Quando há mais lotes ao alcance do que o jogo mantém em detalhe total, um lote que já está em detalhe total o mantém até que um lote cerca de 1,4 vez mais próximo precise dele, e os lotes trocam menos para lá e para cá enquanto a câmera se move. "
     "Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Cuando hay más solares al alcance de los que el juego mantiene con todo el detalle, un solar que ya lo tiene lo mantiene hasta que lo necesita un solar unas 1,4 veces más cercano, así que los solares cambian con menos frecuencia mientras la cámara se mueve. "
     "Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Quand plus de terrains sont à portée que le jeu n'en garde en détail complet, un terrain déjà en détail complet le garde jusqu'à ce qu'un terrain environ 1,4 fois plus proche en ait besoin : les terrains changent moins souvent quand la caméra bouge. "
     "Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Remembers which of the game's packages holds each file the game asks for, so it does not search every package again. Fewer small "
     "stutters when objects, textures and lots load. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Memoriza qual pacote do jogo contém cada arquivo que o jogo pede, para não procurar em todos os pacotes de novo. Menos travadas "
     "pequenas quando objetos, texturas e lotes carregam. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Recuerda qué paquete del juego contiene cada archivo que el juego pide, para no volver a buscar en todos los paquetes. Menos tirones "
     "pequeños al cargar objetos, texturas y solares. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Retient quel paquet du jeu contient chaque fichier que le jeu demande, pour ne pas fouiller à nouveau tous les paquets. Moins de "
     "petites saccades au chargement des objets, textures et terrains. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Lets Faster Game File Lookups also remember files that no package has, so the game does not search every package for them again "
     "and again. Needs Faster Game File Lookups. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Faz as buscas rápidas de arquivos também memorizarem os arquivos que nenhum pacote tem, para o jogo não procurá-los em todos os "
     "pacotes repetidas vezes. Requer as buscas rápidas de arquivos. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Hace que las búsquedas rápidas de archivos también recuerden los archivos que ningún paquete tiene, para que el juego no los busque "
     "en todos los paquetes una y otra vez. Requiere las búsquedas rápidas de archivos. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Permet aux recherches rapides de fichiers de retenir aussi les fichiers qu'aucun paquet ne contient, pour que le jeu ne les cherche "
     "plus sans cesse dans tous les paquets. Nécessite les recherches rapides de fichiers. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Remembers which files of a kind each of the game's packages holds, so Create a Sim and Sim loading do not read the list of every "
     "package again. Fewer small stutters when Sims change outfits or load. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Memoriza quais arquivos de cada tipo cada pacote do jogo contém, para o Criar um Sim e o carregamento de Sims não lerem de novo a "
     "lista de todos os pacotes. Menos travadas pequenas quando Sims trocam de roupa ou carregam. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Recuerda qué archivos de cada tipo contiene cada paquete del juego, para que Crear un Sim y la carga de Sims no vuelvan a leer la "
     "lista de todos los paquetes. Menos tirones pequeños cuando los Sims cambian de ropa o cargan. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Retient quels fichiers de chaque type contient chaque paquet du jeu, pour que Créer un Sim et le chargement des Sims ne relisent pas "
     "la liste de tous les paquets. Moins de petites saccades quand les Sims changent de tenue ou se chargent. Fait partie d'" APEX_PRODUCT_NAME
     ". Crédits : @loinyx"},
    {"While the camera moves, the soft ambient shading of the outdoor walls of newly loaded lots waits until the camera stops (or "
     "two seconds), and never more than one wall pass runs per frame, so panning over a neighborhood that is loading stutters "
     "less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Enquanto a câmera se move, o sombreamento suave das paredes externas dos lotes recém-carregados espera a câmera parar (ou dois "
     "segundos), e nunca roda mais de uma passada de paredes por quadro, então mover a câmera sobre um bairro carregando trava menos. "
     "Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Mientras la cámara se mueve, el sombreado suave de las paredes exteriores de los solares recién cargados espera a que la cámara se "
     "detenga (o dos segundos), y nunca se hace más de una pasada de paredes por fotograma, así que recorrer un barrio que se está cargando "
     "da menos tirones. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Pendant que la caméra bouge, l'ombrage doux des murs extérieurs des terrains tout juste chargés attend que la caméra s'arrête (ou deux "
     "secondes), et jamais plus d'une passe de murs par image : survoler un quartier en cours de chargement saccade moins. Fait partie d'"
     APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"While the camera moves, lots relight, and lots that just loaded are built, in smaller steps each frame instead of taking up to "
     "15 ms (lighting) or 35 ms (building) at once, so panning over busy neighborhoods stutters less. Everything finishes "
     "as soon as the camera stops. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Enquanto a câmera se move, os lotes são reiluminados, e os lotes que acabaram de carregar são montados, em passos menores a cada "
     "quadro em vez de levar até 15 ms (iluminação) ou 35 ms (montagem) de uma vez, então mover a câmera sobre bairros cheios trava menos. "
     "Tudo termina assim que a câmera para. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Mientras la cámara se mueve, los solares se reiluminan, y los solares recién cargados se construyen, en pasos más pequeños cada "
     "fotograma en lugar de tomar hasta 15 ms (iluminación) o 35 ms (construcción) de golpe, así que recorrer barrios concurridos da menos "
     "tirones. Todo termina en cuanto la cámara se detiene. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Pendant que la caméra bouge, les terrains se rééclairent, et les terrains qui viennent de charger se construisent, par petites "
     "étapes à chaque image au lieu de prendre jusqu'à 15 ms (éclairage) ou 35 ms (construction) d'un coup : survoler des quartiers animés "
     "saccade moins. Tout se termine dès que la caméra s'arrête. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Compresses the textures the game builds while you play (terrain, Sims, lot views, thumbnails) several times faster, with "
     "exactly the same result, so those moments stutter less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Comprime as texturas que o jogo cria enquanto você joga (terreno, Sims, vistas de lotes, miniaturas) várias vezes mais rápido, com "
     "exatamente o mesmo resultado, então esses momentos travam menos. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Comprime las texturas que el juego crea mientras juegas (terreno, Sims, vistas de solares, miniaturas) varias veces más rápido, con "
     "exactamente el mismo resultado, así que esos momentos dan menos tirones. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Compresse les textures que le jeu crée pendant que vous jouez (sol, Sims, vues des terrains, miniatures) plusieurs fois plus vite, "
     "avec exactement le même résultat : ces moments saccadent moins. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Compresses what the game stores in its caches and saves (Sims, objects, terrain) with a much faster compressor in the game's own "
     "format, so those moments stutter less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Comprime o que o jogo guarda nos caches e nos saves (Sims, objetos, terreno) com um compressor muito mais rápido no próprio formato "
     "do jogo, então esses momentos travam menos. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Comprime lo que el juego guarda en sus cachés y partidas (Sims, objetos, terreno) con un compresor mucho más rápido en el propio "
     "formato del juego, así que esos momentos dan menos tirones. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Compresse ce que le jeu stocke dans ses caches et ses sauvegardes (Sims, objets, sol) avec un compresseur bien plus rapide, au format "
     "du jeu : ces moments saccadent moins. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"When the game builds a Sim (Create a Sim, and when a Sim changes outfits), it sorts the triangles of hair and other see-through "
     "layers with a slow test of every triangle against every point of the mesh. This does the same sort many times faster, with "
     "exactly the same result, so those moments stutter less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Quando o jogo monta um Sim (no Criar um Sim e quando um Sim troca de roupa), ele ordena os triângulos do cabelo e de outras camadas "
     "transparentes com um teste lento de cada triângulo contra cada ponto da malha. Isto faz a mesma ordenação muitas vezes mais rápido, "
     "com exatamente o mesmo resultado, então esses momentos travam menos. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Cuando el juego construye un Sim (en Crear un Sim y cuando un Sim cambia de ropa), ordena los triángulos del pelo y de otras capas "
     "transparentes con una prueba lenta de cada triángulo contra cada punto de la malla. Esto hace la misma ordenación muchas veces más "
     "rápido, con exactamente el mismo resultado, así que esos momentos dan menos tirones. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Quand le jeu construit un Sim (dans Créer un Sim et quand un Sim change de tenue), il trie les triangles des cheveux et des autres "
     "couches transparentes avec un test lent de chaque triangle contre chaque point du maillage. Ceci fait le même tri bien plus vite, avec "
     "exactement le même résultat : ces moments saccadent moins. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Every part of the game shares one memory manager. When two parts need it at once, the second one used to go to sleep at once "
     "and wake up late, and freeing a big block of memory made everyone wait. Now it waits a few microseconds before sleeping, and "
     "big blocks are handed back to Windows in the background. Nothing else changes. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Todas as partes do jogo dividem um único gerenciador de memória. Quando duas partes precisavam dele ao mesmo tempo, a segunda "
     "dormia na hora e acordava atrasada, e liberar um bloco grande de memória fazia todo mundo esperar. Agora ela espera alguns "
     "microssegundos antes de dormir, e os blocos grandes são devolvidos ao Windows em segundo plano. Nada mais muda. Parte do "
     APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Todas las partes del juego comparten un único gestor de memoria. Cuando dos partes lo necesitaban a la vez, la segunda se "
     "dormía enseguida y despertaba tarde, y liberar un bloque grande de memoria hacía esperar a todos. Ahora espera unos "
     "microsegundos antes de dormirse, y los bloques grandes se devuelven a Windows en segundo plano. Nada más cambia. Parte de "
     APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Toutes les parties du jeu partagent un seul gestionnaire de mémoire. Quand deux parties en avaient besoin en même temps, la "
     "seconde s'endormait aussitôt et se réveillait en retard, et libérer un gros bloc de mémoire faisait attendre tout le monde. "
     "Désormais elle attend quelques microsecondes avant de s'endormir, et les gros blocs sont rendus à Windows en arrière-plan. Rien "
     "d'autre ne change. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"The game is 32-bit: in a long session the save can fail (Error 12) for lack of one large free block of memory, even with "
     "plenty free in total. This keeps a reserve of free address space that is handed back right before every save, and when "
     "memory gets tight it empties the game's cache of files it is not using. Nothing you see changes. Part of " APEX_PRODUCT_NAME
     ". Credits: @loinyx",
     "O jogo é 32 bits: numa sessão longa o salvamento pode falhar (Erro 12) por falta de um bloco grande de memória livre, mesmo com "
     "bastante livre no total. Isto guarda uma reserva de espaço de endereços livre que é devolvida logo antes de cada salvamento, e "
     "quando a memória aperta esvazia o cache de arquivos que o jogo não está usando. Nada do que você vê muda. Parte do " APEX_PRODUCT_NAME
     ". Créditos: @loinyx",
     "El juego es de 32 bits: en una sesión larga el guardado puede fallar (Error 12) por falta de un bloque grande de memoria libre, "
     "aun con mucha libre en total. Esto guarda una reserva de espacio de direcciones libre que se devuelve justo antes de cada guardado, "
     "y cuando falta memoria vacía la caché de archivos que el juego no está usando. Nada de lo que ves cambia. Parte de " APEX_PRODUCT_NAME
     ". Créditos: @loinyx",
     "Le jeu est en 32 bits : dans une longue session, la sauvegarde peut échouer (Erreur 12) faute d'un grand bloc de mémoire libre, "
     "même avec beaucoup de mémoire libre au total. Ceci garde une réserve d'espace d'adressage libre rendue juste avant chaque "
     "sauvegarde, et quand la mémoire manque, vide le cache des fichiers que le jeu n'utilise pas. Rien de visible ne change. Fait "
     "partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Every frame the game asked Windows to repaint its own window, although the picture comes from the graphics card: "
     "a repaint message went through every window handler each frame for nothing. Now Windows repaints it only when it "
     "needs to (when the window is uncovered or resized). Nothing you see changes. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "A cada quadro o jogo pedia ao Windows para repintar a própria janela, embora a imagem venha da placa de vídeo: uma "
     "mensagem de repintura passava por todos os tratadores da janela a cada quadro, à toa. Agora o Windows só a repinta quando "
     "precisa (quando a janela é descoberta ou redimensionada). Nada do que você vê muda. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "En cada fotograma el juego pedía a Windows que repintara su propia ventana, aunque la imagen viene de la tarjeta gráfica: un "
     "mensaje de repintado pasaba por todos los manejadores de la ventana en cada fotograma, para nada. Ahora Windows solo la repinta "
     "cuando lo necesita (al descubrirse o cambiar de tamaño la ventana). Nada de lo que ves cambia. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "À chaque image, le jeu demandait à Windows de repeindre sa propre fenêtre, alors que l'image vient de la carte graphique : un "
     "message de repeinte passait par tous les gestionnaires de la fenêtre à chaque image, pour rien. Désormais Windows ne la repeint "
     "que quand il le faut (fenêtre découverte ou redimensionnée). Rien de visible ne change. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"The game's scripts do two common things with extra work: every comparison of two decimal numbers first called a "
     "separate library to check each number, and every look-up of a type's information went through a lock and a search. "
     "The check is now done right where the comparison is, and type information already found is remembered, with the same "
     "results, so scripts (Sim decisions, routing, timers) do less work. Nothing you see changes. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Os scripts do jogo fazem duas coisas comuns com trabalho a mais: cada comparação de dois números decimais chamava antes "
     "uma biblioteca separada para verificar cada número, e cada consulta às informações de um tipo passava por uma trava e uma "
     "busca. Agora a verificação é feita ali mesmo, onde a comparação acontece, e as informações de tipo já encontradas ficam "
     "guardadas, com os mesmos resultados, então os scripts (decisões dos Sims, rotas, temporizadores) trabalham menos. Nada do que "
     "você vê muda. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Los scripts del juego hacen dos cosas habituales con trabajo de más: cada comparación de dos números decimales llamaba antes "
     "a una biblioteca aparte para comprobar cada número, y cada consulta de la información de un tipo pasaba por un bloqueo y una "
     "búsqueda. Ahora la comprobación se hace allí mismo, donde está la comparación, y la información de tipo ya encontrada se "
     "recuerda, con los mismos resultados, así que los scripts (decisiones de los Sims, rutas, temporizadores) trabajan menos. Nada "
     "de lo que ves cambia. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Les scripts du jeu font deux choses courantes avec du travail en trop : chaque comparaison de deux nombres décimaux appelait "
     "d'abord une bibliothèque séparée pour vérifier chaque nombre, et chaque consultation des informations d'un type passait par "
     "un verrou et une recherche. La vérification se fait maintenant là où a lieu la comparaison, et les informations de type déjà "
     "trouvées sont mémorisées, avec les mêmes résultats : les scripts (décisions des Sims, itinéraires, minuteries) travaillent "
     "moins. Rien de visible ne change. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"While the camera moves, objects that just loaded or moved are placed in the scene a few hundred per frame instead of all "
     "at once, so panning over a lot that streams in stutters less. An object may appear a frame or two later; everything is "
     "placed at once as soon as the camera stops. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Enquanto a câmera se move, objetos que acabaram de carregar ou se mover entram na cena algumas centenas por quadro em vez de todos de "
     "uma vez, então mover a câmera sobre um lote carregando trava menos. Um objeto pode aparecer um ou dois quadros depois; tudo entra de "
     "uma vez assim que a câmera para. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Mientras la cámara se mueve, los objetos que acaban de cargarse o moverse entran en la escena unos cientos por fotograma en lugar de "
     "todos a la vez, así que recorrer un solar que se está cargando da menos tirones. Un objeto puede aparecer uno o dos fotogramas "
     "después; todo entra de golpe en cuanto la cámara se detiene. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Pendant que la caméra bouge, les objets qui viennent de se charger ou de bouger entrent dans la scène quelques centaines par image au "
     "lieu de tous d'un coup : survoler un terrain en cours de chargement saccade moins. Un objet peut apparaître une ou deux images plus "
     "tard ; tout entre d'un coup dès que la caméra s'arrête. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Remembers where the game found each lot when it looks one up by its ID, instead of searching the whole world every time, "
     "and keeps a quick index of every object by its ID next to the game's own list. Fewer stutters when lot lights update "
     "and less work for the game's scripts. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Memoriza onde o jogo encontrou cada lote ao procurá-lo pelo ID, em vez de vasculhar o mundo inteiro toda vez, e mantém um índice "
     "rápido de todos os objetos pelo ID ao lado da lista do próprio jogo. Menos travadas quando as luzes dos lotes atualizam e menos "
     "trabalho para os scripts do jogo. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Recuerda dónde encontró el juego cada solar al buscarlo por su ID, en lugar de recorrer todo el mundo cada vez, y mantiene un índice "
     "rápido de todos los objetos por su ID junto a la lista del propio juego. Menos tirones cuando se actualizan las luces de los solares "
     "y menos trabajo para los scripts del juego. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Retient où le jeu a trouvé chaque terrain quand il le cherche par son ID, au lieu de parcourir tout le monde à chaque fois, et tient "
     "un index rapide de tous les objets par leur ID à côté de la liste du jeu. Moins de saccades quand l'éclairage des terrains se met à "
     "jour et moins de travail pour les scripts du jeu. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Rooms light up much sooner when you enter a lot, change floors or switch lamps: the lot you are on and the floor you look at "
     "go first, rooms reach their final look in fewer steps, and several small rooms are lit per frame. Part of " APEX_PRODUCT_NAME ". "
     "Credits: @loinyx",
     "Os cômodos acendem bem mais rápido quando você entra num lote, troca de andar ou mexe nas luzes: o lote onde você está e o andar "
     "que você olha vêm primeiro, os cômodos chegam ao visual final em menos etapas e vários cômodos pequenos são iluminados por quadro. "
     "Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Las habitaciones se iluminan mucho antes al entrar en un solar, cambiar de piso o tocar las lámparas: el solar donde estás y el piso "
     "que miras van primero, las habitaciones llegan a su aspecto final en menos pasos y se iluminan varias habitaciones pequeñas por "
     "fotograma. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Les pièces s'éclairent bien plus vite quand vous entrez sur un terrain, changez d'étage ou touchez aux lampes : le terrain où vous "
     "êtes et l'étage que vous regardez passent en premier, les pièces atteignent leur aspect final en moins d'étapes et plusieurs petites "
     "pièces sont éclairées par image. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},

    // ---- "Not available" note of a feature (ApexPatch::UnavailableReason) ----
    {"Not available on {}", "Não disponível em {}", "No disponible en {}", "Non disponible sur {}"},
    {"Not available on {} (game code not scanned yet)", "Não disponível em {} (código do jogo ainda não analisado)",
     "No disponible en {} (código del juego aún no analizado)", "Non disponible sur {} (code du jeu pas encore analysé)"},
    {"Not available on {}: missing {}", "Não disponível em {}: falta {}", "No disponible en {}: falta {}", "Non disponible sur {} : il manque {}"},
    {"an unknown game version", "uma versão desconhecida do jogo", "una versión desconocida del juego", "une version inconnue du jeu"},

    // ---- Sims3SettingsSetter detection line (Settings > Compatibility > Details) ----
    {"not scanned yet", "ainda não verificado", "aún no verificado", "pas encore vérifié"},
    {"official Sims3SettingsSetter loaded ({})", "Sims3SettingsSetter oficial carregado ({})", "Sims3SettingsSetter oficial cargado ({})",
     "Sims3SettingsSetter officiel chargé ({})"},
    {"official Sims3SettingsSetter not loaded", "Sims3SettingsSetter oficial não carregado", "Sims3SettingsSetter oficial no cargado",
     "Sims3SettingsSetter officiel non chargé"},
    {"; OLD COMBINED BUILD loaded ({}): " APEX_PRODUCT_NAME "'s features stay off",
     "; VERSÃO COMBINADA ANTIGA carregada ({}): os recursos do " APEX_PRODUCT_NAME " ficam desligados",
     "; VERSIÓN COMBINADA ANTIGUA cargada ({}): las funciones de " APEX_PRODUCT_NAME " quedan desactivadas",
     "; ANCIENNE VERSION COMBINÉE chargée ({}) : les fonctions d'" APEX_PRODUCT_NAME " restent désactivées"},
    {"; an older {} is also installed (idle): delete it from Game\\Bin", "; um {} mais antigo também está instalado (inativo): apague-o de Game\\Bin",
     "; también hay un {} más antiguo instalado (inactivo): bórralo de Game\\Bin", "; un ancien {} est aussi installé (inactif) : supprimez-le de Game\\Bin"},
};
const I18n::Table kTable(kEntries, std::size(kEntries));

} // namespace
