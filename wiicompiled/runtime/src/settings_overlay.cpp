#include "frontend/runtime_mode.h"
#include <aurora/aurora.h>
#include <aurora/imgui.h>
#include <unordered_map>
#include <string>
#include <cstdlib>
extern "C" uint8_t* aurora_load_png_rgba(const char* path, uint32_t* width, uint32_t* height);
#include "netplay/game_layout.h"
#include "netplay_lobby.h"
#include "netplay_ui.h"
#include "settings_overlay.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "input_bindings.h"
#include "keybinds.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "display_settings.h"
#include <dolphin/gx/GXAurora.h>
#include "runtime_log.h"
#include "memory.h"
#include "wii_remote_input.h"
#include "virtual_remote.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

#include <dolphin/pad.h>

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include "spm_map_sweep.h"
#include <vector>
#include <fstream>
#include <cstring>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.

namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

struct DebugText { const char* text[5]; };   // en, de, fr, es, it
constexpr DebugText kDebugText[] = {
    {{"Map", "Karte", "Carte", "Mapa", "Mappa"}},
    {{"Door (optional)", "Tür (optional)", "Porte (facultatif)", "Puerta (opcional)", "Porta (facoltativo)"}},
    {{"Warp to this map", "Zu dieser Karte teleportieren", "Aller à cette carte", "Ir a este mapa", "Vai a questa mappa"}},
    {{"Warp: story progress and flags are not changed.", "Teleport: Story-Fortschritt und Flags bleiben unverändert.", "Téléportation : la progression et les flags ne changent pas.", "Teletransporte: el progreso y los flags no cambian.", "Teletrasporto: progresso e flag non cambiano."}},
    {{"Filter", "Filter", "Filtre", "Filtro", "Filtro"}},
    {{"Item id", "Item-ID", "ID de l'objet", "ID del objeto", "ID oggetto"}},
    {{"Add to inventory", "Zum Inventar hinzufügen", "Ajouter à l'inventaire", "Añadir al inventario", "Aggiungi all'inventario"}},
    {{"Remove", "Entfernen", "Retirer", "Quitar", "Rimuovi"}},
    {{"Add returns %d (1 = added, 0 = no room)", "Ergebnis: %d (1 = hinzugefügt, 0 = kein Platz)", "Résultat : %d (1 = ajouté, 0 = plus de place)", "Resultado: %d (1 = añadido, 0 = sin espacio)", "Risultato: %d (1 = aggiunto, 0 = nessuno spazio)"}},
    {{"Adds the character / Pixl and makes it selectable.", "Fügt die Figur / den Pixl hinzu und macht sie wählbar.", "Ajoute le personnage / Pixl et le rend sélectionnable.", "Añade el personaje / Pixl y lo hace seleccionable.", "Aggiunge il personaggio / Pixl e lo rende selezionabile."}},
    {{"Coins", "Münzen", "Pièces", "Monedas", "Monete"}},
    {{"Add coins", "Münzen hinzufügen", "Ajouter des pièces", "Añadir monedas", "Aggiungi monete"}},
    {{"HP", "HP", "PV", "PS", "PS"}},
    {{"Max HP", "Max. HP", "PV max", "PS máx.", "PS max"}},
    {{"Set HP", "HP setzen", "Fixer les PV", "Fijar PS", "Imposta PS"}},
    {{"Story position (GSW 0): %d", "Story-Position (GSW 0): %d", "Position de l'histoire (GSW 0) : %d", "Posición de la historia (GSW 0): %d", "Posizione della storia (GSW 0): %d"}},
    {{"Set", "Setzen", "Fixer", "Fijar", "Imposta"}},
    {{"GSW index", "GSW-Nummer", "Numéro GSW", "Número GSW", "Numero GSW"}},
    {{"Value (0-255)", "Wert (0-255)", "Valeur (0-255)", "Valor (0-255)", "Valore (0-255)"}},
    {{"Set GSW", "GSW setzen", "Fixer GSW", "Fijar GSW", "Imposta GSW"}},
    {{"Flag (GSWF)", "Flag (GSWF)", "Flag (GSWF)", "Flag (GSWF)", "Flag (GSWF)"}},
    {{"Clear", "Löschen", "Effacer", "Borrar", "Cancella"}},
    {{"Read", "Lesen", "Lire", "Leer", "Leggi"}},
    {{"Last read: %d", "Zuletzt gelesen: %d", "Dernière lecture : %d", "Última lectura: %d", "Ultima lettura: %d"}},
    {{"Map name", "Kartenname", "Nom de la carte", "Nombre del mapa", "Nome della mappa"}},
    {{"Position (X Y Z)", "Position (X Y Z)", "Position (X Y Z)", "Posición (X Y Z)", "Posizione (X Y Z)"}},
    {{"Controller inputs", "Controller-Eingaben", "Touches pressées", "Botones pulsados", "Tasti premuti"}},
    {{"Map %s", "Karte %s", "Carte %s", "Mapa %s", "Mappa %s"}},
    {{"Read current values", "Aktuelle Werte lesen", "Lire les valeurs actuelles", "Leer valores actuales", "Leggi valori attuali"}},
    {{"Level", "Level", "Niveau", "Nivel", "Livello"}},
    {{"Attack", "Angriff", "Attaque", "Ataque", "Attacco"}},
    {{"Arcade tokens", "Spielhallen-Marken", "Jetons d'arcade", "Fichas de arcade", "Gettoni arcade"}},
    {{"now %d", "aktuell %d", "actuel %d", "actual %d", "attuale %d"}},
    {{"Not in a map.", "Nicht in einer Karte.", "Pas dans une carte.", "No estás en un mapa.", "Non in una mappa."}},
    {{"Click an entry to remove it. Add items from the Items menu.", "Eintrag anklicken zum Entfernen. Items über das Menü Items hinzufügen.", "Cliquez sur une entrée pour la retirer. Ajoutez des objets depuis le menu Objets.", "Haz clic en una entrada para quitarla. Añade objetos desde el menú Objetos.", "Fai clic su una voce per rimuoverla. Aggiungi oggetti dal menu Oggetti."}},
    {{"Key items", "Schlüssel-Items", "Objets clés", "Objetos clave", "Oggetti chiave"}},
    {{"Usable items", "Benutzbare Items", "Objets utilisables", "Objetos usables", "Oggetti usabili"}},
    {{"Characters and Pixls", "Figuren und Pixls", "Personnages et Pixls", "Personajes y Pixls", "Personaggi e Pixl"}},
    {{"  [selectable]", "  [wählbar]", "  [sélectionnable]", "  [seleccionable]", "  [selezionabile]"}},
    {{"Title screen", "Titelbildschirm", "Écran titre", "Pantalla de título", "Schermata del titolo"}},
    {{"  [in use]", "  [aktiv]", "  [utilisé]", "  [en uso]", "  [in uso]"}},
    {{"The character you play can't be removed.", "Die gespielte Figur kann nicht entfernt werden.", "Le personnage joué ne peut pas être retiré.", "No se puede quitar al personaje que juegas.", "Il personaggio che usi non può essere rimosso."}},
    {{"Remove all Pixls", "Alle Pixls entfernen", "Retirer tous les Pixls", "Quitar todos los Pixls", "Rimuovi tutti i Pixl"}},
    {{"Type", "Typ", "Type", "Tipo", "Tipo"}},
    {{"Number", "Nummer", "Numéro", "Número", "Numero"}},
    {{"Value", "Wert", "Valeur", "Valor", "Valore"}},
    {{"Pit", "Pit", "Puits", "Foso", "Pozzo"}},
    {{"Floor", "Etage", "Étage", "Piso", "Piano"}},
    {{"Room %s (experimental)", "Raum %s (experimentell)", "Salle %s (expérimental)", "Sala %s (experimental)", "Stanza %s (sperimentale)"}},
    {{"Warp", "Teleport", "Téléporter", "Teletransportar", "Teletrasporta"}},
    {{"Warp / TP", "Teleport", "Téléportation", "Teletransporte", "Teletrasporto"}},
    {{"Story / Variables", "Story / Variablen", "Histoire / Variables", "Historia / Variables", "Storia / Variabili"}},
    {{"Items", "Items", "Objets", "Objetos", "Oggetti"}},
    {{"Inventory", "Inventar", "Inventaire", "Inventario", "Inventario"}},
    {{"Pit of 100 Trials", "Grube der 100 Prüfungen", "Puits des 100 épreuves", "Foso de las 100 pruebas", "Pozzo delle 100 prove"}},
    {{"Script variables", "Skript-Variablen", "Variables de script", "Variables de script", "Variabili di script"}},
    {{"HUD displays", "HUD-Anzeigen", "Affichages HUD", "Indicadores HUD", "Indicatori HUD"}},
    {{"Stats", "Werte", "Stats", "Estadísticas", "Statistiche"}},
    {{"Characters / Pixls", "Figuren / Pixls", "Personnages / Pixls", "Personajes / Pixls", "Personaggi / Pixl"}},
    {{"Player", "Spieler", "Joueur", "Jugador", "Giocatore"}},
    {{"Flags", "Flags", "Flags", "Flags", "Flag"}},
    {{"Settings", "Einstellungen", "Réglages", "Ajustes", "Impostazioni"}},
    {{"Enhancements", "Verbesserungen", "Améliorations", "Mejoras", "Miglioramenti"}},
    {{"Network", "Netzwerk", "Réseau", "Red", "Rete"}},
    {{"Dev Tools", "Entwickler", "Outils dev", "Herramientas", "Strumenti dev"}},
    {{"Search...", "Suchen...", "Rechercher...", "Buscar...", "Cerca..."}},
    {{"General", "Allgemein", "Général", "General", "Generali"}},
    {{"Audio", "Audio", "Audio", "Audio", "Audio"}},
    {{"Graphics", "Grafik", "Graphismes", "Gráficos", "Grafica"}},
    {{"Controls", "Steuerung", "Contrôles", "Controles", "Controlli"}},
    {{"Gameplay", "Gameplay", "Jeu", "Juego", "Gioco"}},
    {{"LAN Play", "LAN-Spiel", "Jeu en LAN", "Juego en LAN", "Gioco in LAN"}},
    {{"Warp", "Teleport", "Téléporter", "Teletransportar", "Teletrasporta"}},
    {{"Flag watch", "Flag-Überwachung", "Suivi des flags", "Vigilancia de flags", "Monitor flag"}},
    {{"Display / Language", "Anzeige / Sprache", "Affichage / Langue", "Pantalla / Idioma", "Schermo / Lingua"}},
    {{"Resolution", "Auflösung", "Résolution", "Resolución", "Risoluzione"}},
    {{"Aspect", "Seitenverhältnis", "Format d'image", "Relación de aspecto", "Proporzioni"}},
    {{"Spinning coins and animated tiles", "Drehende Münzen und animierte Kacheln", "Pièces tournantes et tuiles animées", "Monedas giratorias y bloques animados", "Monete rotanti e tessere animate"}},
    {{"Watch flags", "Flags überwachen", "Surveiller les flags", "Vigilar flags", "Monitora flag"}},
    {{"Clear log", "Log leeren", "Vider le journal", "Vaciar registro", "Svuota log"}},
    {{"Changes appear here: pick up an item or open a chest to see which flag it sets.", "Änderungen erscheinen hier: Item aufheben oder Truhe öffnen, um das gesetzte Flag zu sehen.", "Les changements apparaissent ici : ramassez un objet ou ouvrez un coffre pour voir quel flag il active.", "Los cambios aparecen aquí: recoge un objeto o abre un cofre para ver qué flag activa.", "I cambiamenti appaiono qui: raccogli un oggetto o apri un forziere per vedere quale flag imposta."}},
    {{"Heal", "Heilen", "Soigner", "Curar", "Cura"}},
    {{"Set 99 coins", "99 Münzen setzen", "Mettre 99 pièces", "Poner 99 monedas", "Imposta 99 monete"}},
    {{"Unlock all characters and Pixls", "Alle Figuren und Pixls freischalten", "Débloquer tous les personnages et Pixls", "Desbloquear todos los personajes y Pixls", "Sblocca tutti i personaggi e Pixl"}},
    {{"Save position", "Position speichern", "Sauver la position", "Guardar posición", "Salva posizione"}},
    {{"Load position", "Position laden", "Charger la position", "Cargar posición", "Carica posizione"}},
    {{"Arrive at X Y Z", "Ankunft bei X Y Z", "Arriver en X Y Z", "Llegar a X Y Z", "Arriva a X Y Z"}},
    {{"Teleport", "Teleportieren", "Téléporter", "Teletransportar", "Teletrasporta"}},
    {{"Gravity:", "Schwerkraft:", "Gravité :", "Gravedad:", "Gravità:"}},
    {{"Down", "Unten", "Bas", "Abajo", "Giù"}},
    {{"Up (upside down)", "Oben (kopfüber)", "Haut (à l'envers)", "Arriba (al revés)", "Su (capovolto)"}},
    {{"Left", "Links", "Gauche", "Izquierda", "Sinistra"}},
    {{"Right", "Rechts", "Droite", "Derecha", "Destra"}},
    {{"Position: %.0f  %.0f  %.0f", "Position: %.0f  %.0f  %.0f", "Position : %.0f  %.0f  %.0f", "Posición: %.0f  %.0f  %.0f", "Posizione: %.0f  %.0f  %.0f"}},
    {{"Exit", "Beenden", "Quitter", "Salir", "Esci"}},
    {{"Close (F10)", "Schließen (F10)", "Fermer (F10)", "Cerrar (F10)", "Chiudi (F10)"}},
    {{"Menu size", "Menügröße", "Taille du menu", "Tamaño del menú", "Dimensione menu"}},
    {{"Accelerometer", "Beschleunigungssensor", "Accéléromètre", "Acelerómetro", "Accelerometro"}},
    {{"No accelerometer data yet.", "Noch keine Beschleunigungsdaten.", "Pas encore de données d'accéléromètre.", "Aún no hay datos del acelerómetro.", "Ancora nessun dato dell'accelerometro."}},
    {{"Hold still...", "Ruhig halten...", "Ne bougez pas...", "No te muevas...", "Stai fermo..."}},
    {{"Wii Remotes (Bluetooth)", "Wii-Fernbedienungen (Bluetooth)", "Wiimotes (Bluetooth)", "Wii Remotes (Bluetooth)", "Wii Remote (Bluetooth)"}},
    {{"Use WASD + mouse preset", "WASD + Maus Voreinstellung nutzen", "Utiliser le préréglage ZQSD/WASD + souris", "Usar preajuste WASD + ratón", "Usa preset WASD + mouse"}},
    {{"Applies to every port.", "Gilt für alle Ports.", "S'applique à tous les ports.", "Se aplica a todos los puertos.", "Vale per tutte le porte."}},
    {{"Presets", "Voreinstellungen", "Préréglages", "Preajustes", "Preset"}},
    {{"GameCube", "GameCube", "GameCube", "GameCube", "GameCube"}},
    {{"Master", "Gesamt", "Général", "General", "Generale"}},
    {{"Music", "Musik", "Musique", "Música", "Musica"}},
    {{"UI", "Oberfläche", "Interface", "Interfaz", "Interfaccia"}},
    {{"Voices", "Stimmen", "Voix", "Voces", "Voci"}},
    {{"Mute", "Stumm", "Muet", "Silenciar", "Muto"}},
    {{"Mute shortcut", "Stumm-Kürzel", "Raccourci muet", "Atajo de silencio", "Scorciatoia muto"}},
    {{"Devices asked for rooms directly:", "Direkt nach Räumen gefragte Geräte:", "Appareils interrogés directement pour les salons :", "Dispositivos consultados directamente:", "Dispositivi interrogati direttamente:"}},
    {{"Remove", "Entfernen", "Retirer", "Quitar", "Rimuovi"}},
    {{"Add", "Hinzufügen", "Ajouter", "Añadir", "Aggiungi"}},
    {{"Disable bloom", "Bloom deaktivieren", "Désactiver le bloom", "Desactivar bloom", "Disattiva bloom"}},
    {{"Disable copy filter", "Kopierfilter deaktivieren", "Désactiver le filtre de copie", "Desactivar filtro de copia", "Disattiva filtro di copia"}},
    {{"Skip draws while shaders compile", "Zeichnen überspringen, solange Shader kompilieren", "Ignorer les dessins pendant la compilation des shaders", "Omitir dibujos mientras se compilan shaders", "Salta i disegni durante la compilazione degli shader"}},
    {{"Show FPS", "FPS anzeigen", "Afficher les FPS", "Mostrar FPS", "Mostra FPS"}},
    {{"Graphics API: %s", "Grafik-API: %s", "API graphique : %s", "API gráfica: %s", "API grafica: %s"}},
    {{"Display mode", "Anzeigemodus", "Mode d'affichage", "Modo de pantalla", "Modalità schermo"}},
    {{"Windowed", "Fenster", "Fenêtré", "Ventana", "Finestra"}},
    {{"Borderless fullscreen", "Rahmenloser Vollbildmodus", "Plein écran sans bordure", "Pantalla completa sin bordes", "Schermo intero senza bordi"}},
    {{"Exclusive fullscreen", "Exklusiver Vollbildmodus", "Plein écran exclusif", "Pantalla completa exclusiva", "Schermo intero esclusivo"}},
    {{"Off", "Aus", "Désactivé", "Desactivado", "Disattivato"}},
    {{"240 FPS (fast PCs)", "240 FPS (schnelle PCs)", "240 FPS (PC rapides)", "240 FPS (PC rápidos)", "240 FPS (PC veloci)"}},
    {{"Frame interpolation (experimental)", "Bildinterpolation (experimentell)", "Interpolation d'images (expérimental)", "Interpolación de fotogramas (experimental)", "Interpolazione fotogrammi (sperimentale)"}},
    {{"Frame interpolation is experimental, you might find visual artifacts", "Die Bildinterpolation ist experimentell, es können Bildfehler auftreten", "L'interpolation est expérimentale, des artefacts visuels sont possibles", "La interpolación es experimental, pueden verse artefactos visuales", "L'interpolazione è sperimentale, potrebbero comparire artefatti visivi"}},
    {{"Vsync (no screen tearing)", "VSync (kein Bildreißen)", "VSync (sans déchirement d'image)", "VSync (sin desgarros de imagen)", "VSync (senza tearing)"}},
    {{"Applied when the next scene renderer is created.", "Wird beim nächsten Erstellen des Szenen-Renderers angewendet.", "Appliqué à la prochaine création du moteur de rendu de scène.", "Se aplica al crear el siguiente renderizador de escena.", "Applicato alla prossima creazione del renderer di scena."}},
    {{"Button mapping", "Tastenbelegung", "Mappage des boutons", "Asignación de botones", "Mappatura tasti"}},
    {{"Stick and trigger mapping", "Stick- und Triggerbelegung", "Mappage des sticks et gâchettes", "Asignación de sticks y gatillos", "Mappatura stick e grilletti"}},
    {{"Vibration", "Vibration", "Vibration", "Vibración", "Vibrazione"}},
    {{"Expressions (Dolphin syntax)", "Ausdrücke (Dolphin-Syntax)", "Expressions (syntaxe Dolphin)", "Expresiones (sintaxis Dolphin)", "Espressioni (sintassi Dolphin)"}},
    {{"No controller connected", "Kein Controller verbunden", "Aucune manette connectée", "Ningún mando conectado", "Nessun controller collegato"}},
    {{"Assign connected controller", "Verbundenen Controller zuweisen", "Assigner la manette connectée", "Asignar mando conectado", "Assegna controller collegato"}},
    {{"Assign a controller to edit its buttons", "Controller zuweisen, um seine Tasten zu bearbeiten", "Assignez une manette pour modifier ses boutons", "Asigna un mando para editar sus botones", "Assegna un controller per modificarne i tasti"}},
    {{"Click a binding, then press an input. No input for 10 seconds clears it.", "Belegung anklicken, dann eine Eingabe drücken. Ohne Eingabe nach 10 Sekunden wird sie gelöscht.", "Cliquez sur une touche puis appuyez sur une entrée. Sans entrée pendant 10 secondes, elle est effacée.", "Haz clic en un botón y pulsa una entrada. Sin entrada en 10 segundos se borra.", "Fai clic su un tasto e premi un input. Senza input per 10 secondi viene cancellato."}},
    {{"LT / L2 = left trigger. RT / R2 = right trigger.", "LT / L2 = linker Trigger. RT / R2 = rechter Trigger.", "LT / L2 = gâchette gauche. RT / R2 = gâchette droite.", "LT / L2 = gatillo izquierdo. RT / R2 = gatillo derecho.", "LT / L2 = grilletto sinistro. RT / R2 = grilletto destro."}},
    {{"LB / L1 = left shoulder. RB / R1 = right shoulder.", "LB / L1 = linke Schultertaste. RB / R1 = rechte Schultertaste.", "LB / L1 = bouton gauche. RB / R1 = bouton droit.", "LB / L1 = botón izquierdo. RB / R1 = botón derecho.", "LB / L1 = dorsale sinistro. RB / R1 = dorsale destro."}},
    {{"Right shoulder (RB / R1)", "Rechte Schultertaste (RB / R1)", "Bouton droit (RB / R1)", "Botón derecho (RB / R1)", "Dorsale destro (RB / R1)"}},
    {{"Press a keyboard key.", "Eine Taste drücken.", "Appuyez sur une touche.", "Pulsa una tecla.", "Premi un tasto."}},
    {{"Replaces the gamepad on this port. F10 opens settings.", "Ersetzt das Gamepad an diesem Port. F10 öffnet die Einstellungen.", "Remplace la manette sur ce port. F10 ouvre les réglages.", "Sustituye al mando en este puerto. F10 abre los ajustes.", "Sostituisce il gamepad su questa porta. F10 apre le impostazioni."}},
    {{"Pick the controller type or remap buttons in the launcher's Controls page.", "Controller-Typ oder Tastenbelegung auf der Steuerungsseite des Launchers ändern.", "Choisissez le type de manette ou remappez les boutons dans la page Contrôles du lanceur.", "Elige el tipo de mando o reasigna botones en la página Controles del lanzador.", "Scegli il tipo di controller o rimappa i tasti nella pagina Controlli del launcher."}},
    {{"External media is playing; game music is muted.", "Externe Medien laufen; die Spielmusik ist stumm.", "Un média externe est en lecture ; la musique du jeu est coupée.", "Se reproduce un medio externo; la música del juego está silenciada.", "È in riproduzione un media esterno; la musica del gioco è silenziata."}},
    {{"No external media is currently playing.", "Derzeit laufen keine externen Medien.", "Aucun média externe n'est en lecture.", "No se reproduce ningún medio externo.", "Nessun media esterno in riproduzione."}},
    {{"Original 4:3", "Original 4:3", "Original 4:3", "Original 4:3", "Originale 4:3"}},
    {{"Fill Window", "Fenster füllen", "Remplir la fenêtre", "Llenar la ventana", "Riempi la finestra"}},
    {{"Cinematic 2.39:1", "Kinoformat 2.39:1", "Cinéma 2.39:1", "Cinemático 2.39:1", "Cinematografico 2.39:1"}},
    {{"Auto (screen size)", "Automatisch (Bildschirmgröße)", "Auto (taille de l'écran)", "Auto (tamaño de pantalla)", "Auto (dimensione schermo)"}},
    {{"Custom", "Benutzerdefiniert", "Personnalisé", "Personalizado", "Personalizzato"}},
    {{"Switching between 4:3 and widescreen takes effect after a restart.", "Der Wechsel zwischen 4:3 und Breitbild gilt nach einem Neustart.", "Le passage entre 4:3 et écran large prend effet après un redémarrage.", "El cambio entre 4:3 y panorámico se aplica tras reiniciar.", "Il passaggio tra 4:3 e widescreen ha effetto dopo il riavvio."}},
    {{"Not scanning", "Keine Suche", "Pas de recherche", "Sin buscar", "Nessuna ricerca"}},
    {{"Menu language", "Menüsprache", "Langue du menu", "Idioma del menú", "Lingua del menu"}},
    {{"Game language", "Spielsprache", "Langue du jeu", "Idioma del juego", "Lingua del gioco"}},
    {{"Takes effect after restarting the game.", "Wird nach einem Neustart des Spiels wirksam.", "Prend effet après le redémarrage du jeu.", "Se aplica al reiniciar el juego.", "Ha effetto dopo il riavvio del gioco."}},
    {{"Mouse acts as the Wii pointer", "Maus steuert den Wii-Zeiger", "La souris sert de pointeur Wii", "El ratón funciona como puntero Wii", "Il mouse funziona da puntatore Wii"}},
    {{"Moving the mouse over the game aims Tippi's pointer, and a click presses A.", "Mausbewegung zielt mit Tippis Zeiger, ein Klick drückt A.", "Bouger la souris sur le jeu vise avec le pointeur de Tippi, un clic appuie sur A.", "Mover el ratón sobre el juego apunta con el puntero de Tippi y un clic pulsa A.", "Muovere il mouse sul gioco punta con il puntatore di Tippi, un clic preme A."}},
    {{"Rooms", "Räume", "Salles", "Salas", "Stanze"}},
};

// Debug menu text in the game's language (falls back to the English text itself).
const char* Tr(const char* en);

int g_debugLanguage = -1;
int g_debugLanguage_ui = 0;   // -1: follow the game's language, else 0 en, 1 de, 2 fr, 3 es, 4 it (set in the Debug menu)
int DebugLanguageIndex();

const char* Tr(const char* en) {
    const int lang = DebugLanguageIndex();
    for (const auto& row : kDebugText)
        if (std::strcmp(row.text[0], en) == 0) return row.text[lang];
    return en;
}

int DebugLanguageIndex() {
    if (g_debugLanguage >= 0) return g_debugLanguage;
    switch (RuntimeConfigFile::Language()) {
    case 2: return 1;
    case 3: return 2;
    case 4: return 3;
    case 5: return 4;
    default: return 0;
    }
}

// Every ImGui text call of this file goes through ImT, which looks the text up in the translation table
// (the English text itself when there is no entry), so a missing translation never hides anything.
namespace ImT {
template <class... A> bool Button(const char* l, A&&... a) { return ImGui::Button(Tr(l), std::forward<A>(a)...); }
template <class... A> bool SmallButton(const char* l, A&&... a) { return ImGui::SmallButton(Tr(l), std::forward<A>(a)...); }
template <class... A> bool Checkbox(const char* l, A&&... a) { return ImGui::Checkbox(Tr(l), std::forward<A>(a)...); }
template <class... A> bool RadioButton(const char* l, A&&... a) { return ImGui::RadioButton(Tr(l), std::forward<A>(a)...); }
template <class... A> bool SliderInt(const char* l, A&&... a) { return ImGui::SliderInt(Tr(l), std::forward<A>(a)...); }
template <class... A> bool SliderFloat(const char* l, A&&... a) { return ImGui::SliderFloat(Tr(l), std::forward<A>(a)...); }
template <class... A> bool Selectable(const char* l, A&&... a) { return ImGui::Selectable(Tr(l), std::forward<A>(a)...); }
template <class... A> bool MenuItem(const char* l, A&&... a) { return ImGui::MenuItem(Tr(l), std::forward<A>(a)...); }
template <class... A> bool InputInt(const char* l, A&&... a) { return ImGui::InputInt(Tr(l), std::forward<A>(a)...); }
template <class... A> bool InputText(const char* l, A&&... a) { return ImGui::InputText(Tr(l), std::forward<A>(a)...); }
inline bool InputTextWithHint(const char* l, const char* hint, char* buf, size_t size, ImGuiInputTextFlags f = 0) {
    return ImGui::InputTextWithHint(Tr(l), Tr(hint), buf, size, f);
}
inline bool BeginMenu(const char* l, bool enabled = true) { return ImGui::BeginMenu(Tr(l), enabled); }
inline bool BeginCombo(const char* l, const char* preview, ImGuiComboFlags f = 0) { return ImGui::BeginCombo(Tr(l), Tr(preview), f); }
inline void SeparatorText(const char* l) { ImGui::SeparatorText(Tr(l)); }
template <class... A> void Text(const char* f, A&&... a) { ImGui::Text(Tr(f), std::forward<A>(a)...); }
template <class... A> void TextDisabled(const char* f, A&&... a) { ImGui::TextDisabled(Tr(f), std::forward<A>(a)...); }
template <class... A> void TextWrapped(const char* f, A&&... a) { ImGui::TextWrapped(Tr(f), std::forward<A>(a)...); }
template <class... A> void BulletText(const char* f, A&&... a) { ImGui::BulletText(Tr(f), std::forward<A>(a)...); }
template <class... A> void SetTooltip(const char* f, A&&... a) { ImGui::SetTooltip(Tr(f), std::forward<A>(a)...); }
inline void TextUnformatted(const char* t, const char* end = nullptr) { ImGui::TextUnformatted(end ? t : Tr(t), end); }
inline bool Combo(const char* l, int* cur, const char* const items[], int n, int height = -1) {
    std::vector<const char*> translated;
    for (int i = 0; i < n; ++i) translated.push_back(Tr(items[i]));
    return ImGui::Combo(Tr(l), cur, translated.data(), n, height);
}
inline bool Combo(const char* l, int* cur, const char* zeroSeparated, int height = -1) { return ImGui::Combo(Tr(l), cur, zeroSeparated, height); }
}  // namespace ImT


const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

bool g_topBarVisible = false;
bool g_exitPromptOpen = false;
bool g_rumbleEnabled = RuntimeConfigFile::RumbleEnabled(true);
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 120:
        return 1;
    case 180:
        return 2;
    case 240:
        return 3;
    default:
        return 0;
    }
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("borderless");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(false);  // off unless turned on
bool g_frameGraph = RuntimeConfigFile::FrameGraph(false);
std::atomic<bool> g_vsync{RuntimeConfigFile::VSync(true)};  // default on: no tearing
#if defined(__APPLE__)
bool g_metalFxSpatialUpscaling = RuntimeConfigFile::MetalFxSpatialUpscaling(false);
#endif
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// PlayStation layout: bumpers drive the GC triggers, Z moves to Create/Share.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kPlayStationPreset = {
    "south", "east", "west", "north", "start", "back",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;
std::atomic_bool g_bootShadersReady = false;
bool g_bootShaderNotice = false;
Clock::time_point g_bootShaderWaitStart{};
constexpr uint32_t kBootShaderNoticeThreshold = 100;
constexpr auto kBootShaderWaitLimit = std::chrono::minutes(3);

// Render heights: 1x is 480 lines in every aspect (DisplaySettings), the width follows the aspect.
constexpr std::array<ResolutionItem, 12> kResolutions = {{
    {"Auto (screen size)", 0.0f}, {"0.5x (240p)", 0.5f}, {"0.75x (360p)", 0.75f}, {"1x (480p)", 1.0f},
    {"1.5x (720p)", 1.5f}, {"2x (960p)", 2.0f}, {"2.25x (1080p)", 2.25f}, {"3x (1440p)", 3.0f},
    {"4x (1920p)", 4.0f}, {"4.5x (4K)", 4.5f}, {"6x (2880p)", 6.0f}, {"8x (3840p)", 8.0f},
}};

struct AspectItem {
    const char* label;
    int32_t mode;
};
constexpr std::array<AspectItem, 4> kAspects = {{
    {"Original 4:3", DisplaySettings::kAspect4x3},
    {"16:9", DisplaySettings::kAspect16x9},
    {"Fill Window", DisplaySettings::kAspectFill},
    {"Cinematic 2.39:1", DisplaySettings::kAspectCinematic},
}};

constexpr std::array<uint32_t, 4> kFrameInterpolationTargetFps{0, 120, 180, 240};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
}

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.renderScale = scale;
    DisplaySettings::Request(settings);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void SetAspectMode(int32_t mode) {
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.aspect = mode;
    DisplaySettings::Request(settings);
    RuntimeConfigFile::SetAspectMode(mode);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

using ControllerNames::FindNativeButton;

uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};


// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

using ControllerNames::NativeButtonForValue;

std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}


void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y. (Wii Remotes with any
        // extension never reach the PAD layer: the game reads them through KPAD.)
        if (WiiRemoteInput::KindForPort(port) == WiiRemoteInput::Kind::WiiUPro) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled(true);
bool g_wiiContinuousScan = RuntimeConfigFile::WiiContinuousScanEnabled(false);

// Accelerometer readout and zero-point calibration for a bare remote / remote + Nunchuk.
void DrawWiiRemoteAccelerometer(uint32_t port) {
    ImT::SeparatorText("Accelerometer");
    float sdlG[3] = {};
    float kpad[3] = {};
    if (WiiRemoteInput::ReadAccelDebug(port, sdlG, kpad)) {
        ImT::Text("KPAD acc: x %+.2f  y %+.2f  z %+.2f g", kpad[0], kpad[1], kpad[2]);
        ImT::TextDisabled("Flat, buttons up: (0, -1, 0). Sideways as a wheel: (1, 0, 0); z follows the turn.");
    } else {
        ImT::TextDisabled("No accelerometer data yet.");
    }
    // SDL's read of the remote's calibration block often times out over Bluetooth
    // and it falls back to a nominal zero point, leaving a small per-axis bias;
    // measured here with the remote at rest.
    if (WiiRemoteInput::IsAccelCalibrating()) {
        ImGui::ProgressBar(WiiRemoteInput::AccelCalibrationProgress(), ImVec2(220.0f, 0.0f), "Hold still...");
    } else if (ImT::Button("Calibrate (remote lying flat, buttons up)")) {
        WiiRemoteInput::StartAccelCalibration(port);
    }
    if (ImGui::IsItemHovered()) {
        ImT::SetTooltip("Put the remote down on a flat surface with the buttons facing up and do not touch it\n"
                          "for about two seconds. Corrects the steering offset of a remote held sideways.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!RuntimeConfigFile::HasWiiAccelOffset() || WiiRemoteInput::IsAccelCalibrating());
    if (ImT::Button("Clear calibration")) {
        WiiRemoteInput::ClearAccelCalibration();
    }
    ImGui::EndDisabled();
    if (const char* message = WiiRemoteInput::AccelCalibrationMessage()) {
        ImT::TextWrapped("%s", message);
    } else if (RuntimeConfigFile::HasWiiAccelOffset()) {
        const std::array<double, 3> offset = RuntimeConfigFile::WiiAccelOffset();
        ImT::TextDisabled("Stored offset: x %+.3f  y %+.3f  z %+.3f g", offset[0], offset[1], offset[2]);
    } else {
        ImT::TextDisabled("Not calibrated (using SDL's zero point; see console.log for \"fallback accelerometer calibration\").");
    }
}

// Wii Remotes (Bluetooth) menu: driver switch, pairing help, continuous scanning and the port's controller kind.
void DrawWiiRemoteSettings(uint32_t selectedGamePort) {
    if (!ImT::BeginMenu("Wii Remotes (Bluetooth)")) {
        return;
    }
    if (ImT::Checkbox("Use Wii Remotes / Wii U Pro Controllers", &g_wiiRemotesEnabled)) {
        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
    }
    if (ImGui::IsItemHovered()) {
        ImT::SetTooltip("Takes effect on the next launch. Turn this off if you use a Mayflash DolphinBar.");
    }
    ImT::TextDisabled("Pairing: Windows Settings > Bluetooth > Add device, then press 1+2");
    ImT::TextDisabled("(or the red SYNC button) on the remote. Leave the PIN empty.");
    ImT::TextDisabled("A remote that was paired before also needs to be turned on with 1+2/SYNC.");
    if (ImT::Checkbox("Keep scanning for Wii Remotes (like Dolphin's Continuous Scanning)",
                        &g_wiiContinuousScan)) {
        RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
    }
    if (ImGui::IsItemHovered()) {
        ImT::SetTooltip("While no Wii controller is connected, re-check Bluetooth every 2 seconds so a\n"
                          "remote that dropped out (\"Communications with the controller have been\n"
                          "interrupted\") or was turned on after launch comes back by itself.");
    }
    // The driver hint is only read at launch, so a rescan after the user turned
    // the setting off would still re-enumerate Wii devices in this session.
    ImGui::BeginDisabled(!g_wiiRemotesEnabled);
    if (ImT::Button("Rescan now")) {
        WiiRemoteInput::RescanNow();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (WiiRemoteInput::IsScanning()) {
        ImT::TextDisabled("Scanning... (%u so far) - press 1+2 on the remote", WiiRemoteInput::ScanCount());
    } else {
        ImT::TextDisabled("Not scanning");
    }
    ImGui::Separator();

    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForPort(selectedGamePort);
    ImT::Text("Port %u: %s", static_cast<unsigned>(selectedGamePort + 1), WiiRemoteInput::KindLabel(kind));
    if (kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        WiiRemoteInput::KpadSample sample;
        if (WiiRemoteInput::ReadKpadSample(selectedGamePort, sample)) {
            // WPAD_CL_BUTTON_* bits, in the game's own layout (no mapping involved).
            const auto held = [&](uint32_t bit, const char* on, const char* off) { return (sample.clHold & bit) ? on : off; };
            ImT::Text("Classic: %s %s %s %s  %s %s  %s %s  %s %s  %s %s %s %s", held(0x0010, "A", "a"),
                        held(0x0040, "B", "b"), held(0x0008, "X", "x"), held(0x0020, "Y", "y"), held(0x2000, "L", "l"),
                        held(0x0200, "R", "r"), held(0x0080, "ZL", "zl"), held(0x0004, "ZR", "zr"),
                        held(0x0400, "PLUS", "plus"), held(0x1000, "MINUS", "minus"), held(0x0001, "UP", "up"),
                        held(0x4000, "DOWN", "down"), held(0x0002, "LEFT", "left"), held(0x8000, "RIGHT", "right"));
            ImT::Text("Sticks: L %+.2f %+.2f (WPAD %+d %+d)  R %+.2f %+.2f (WPAD %+d %+d)", sample.clLStick[0],
                        sample.clLStick[1], static_cast<int>(sample.clLStickRaw[0]),
                        static_cast<int>(sample.clLStickRaw[1]), sample.clRStick[0], sample.clRStick[1],
                        static_cast<int>(sample.clRStickRaw[0]), static_cast<int>(sample.clRStickRaw[1]));
            ImT::TextDisabled("Capitals = held. The game reads this Classic Controller through KPAD, as on the");
            ImT::TextDisabled("console: its buttons mean what the game says they mean, no mapping applies.");
        }
    }
    if (kind == WiiRemoteInput::Kind::WiiUPro) {
        if (SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(selectedGamePort))) {
            // SDL's Wii driver posts the D-pad as joystick buttons 11-14 (the
            // SDL_GAMEPAD_BUTTON_DPAD_* values) while its default HIDAPI mapping
            // expects a hat, so SDL_GetGamepadButton never sees them; read the
            // joystick directly, like the fallback in aurora's PADRead does.
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            const auto rawButton = [&](int index) {
                return joystick != nullptr && SDL_GetJoystickButton(joystick, index);
            };
            ImT::Text("Raw D-pad: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ? "UP" : "up",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? "DOWN" : "down",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? "LEFT" : "left",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? "RIGHT" : "right");
            ImT::Text("Raw face buttons: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_EAST) ? "A" : "a",
                        rawButton(SDL_GAMEPAD_BUTTON_SOUTH) ? "B" : "b", rawButton(SDL_GAMEPAD_BUTTON_NORTH) ? "X" : "x",
                        rawButton(SDL_GAMEPAD_BUTTON_WEST) ? "Y" : "y");
            ImT::Text("Raw ZL/ZR: %d / %d (pressed above 0)",
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            ImT::TextDisabled("Capitals = held. If a button never turns to capitals while physically held,");
            ImT::TextDisabled("that press is not reaching SDL at all (a driver-level issue, not a mapping one).");
            ImT::TextDisabled("This pad uses Nintendo's own layout (a/b/x/y as labelled); the shared");
            ImT::TextDisabled("button mapping above does not apply to it.");
        }
    }
    if (kind == WiiRemoteInput::Kind::Remote || kind == WiiRemoteInput::Kind::RemoteWithNunchuk ||
        kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        DrawWiiRemoteAccelerometer(selectedGamePort);
    }

    ImGui::EndMenu();
}

const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller, MuteHotkey };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

void BeginRebind(RebindKind kind, uint16_t target, const char* label, bool secondary = false) {
    g_rebind = {};
    g_rebind.active = true;
    g_rebind.openPopup = true;
    g_rebind.kind = kind;
    g_rebind.port = static_cast<uint32_t>(g_controllerPort);
    g_rebind.target = target;
    g_rebind.secondary = secondary;
    g_rebind.label = label;
    g_rebind.deadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::copy_n(keys, std::min(count, static_cast<int>(g_rebind.keys.size())), g_rebind.keys.begin());
    g_rebind.mouse = SDL_GetMouseState(nullptr, nullptr);
    const int index = PADGetIndexForPort(g_rebind.port);
    if (kind == RebindKind::Controller && index >= 0) {
        if (auto* pad = PADGetSDLGamepadForIndex(index)) {
            g_rebind.instance = SDL_GetGamepadID(pad);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
                g_rebind.buttons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
                g_rebind.axesReady[i] = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i)))) < 8000;
        }
    }
}

void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target) RuntimeConfigFile::SetControllerButton(i, config);
    } else if (capture.kind == RebindKind::MuteHotkey) {
        g_muteHotkey = static_cast<int32_t>(value);
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_rebind.active = false;
        return;
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

void DrawRebindPrompt() {
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }
    if (!ImGui::BeginPopupModal("Rebind input", &g_rebind.active, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_rebind.active = false;
        return;
    }
    if (g_rebind.active) {
        ImT::Text("Rebind: %s", g_rebind.label.c_str());
        ImT::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger, or move a stick."
            : g_rebind.kind == RebindKind::MuteHotkey
                ? "Press a keyboard key."
                : "Press a keyboard key or click a mouse button.");
        ImT::TextUnformatted("Release any held input first. Backspace or Delete clears the mapping.");
        ImT::TextUnformatted("Escape can be bound. F10 is reserved for settings.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImT::Text("Unmapped in %d seconds", std::max(0, static_cast<int>(std::ceil(remaining))));
        const bool clear = ImT::Button("Clear mapping");
        ImGui::SameLine();
        if (ImT::Button("Cancel")) g_rebind.active = false;
        // UI clicks must not become mouse bindings (buttons activate on release).
        const bool overControl = ImGui::IsAnyItemHovered();
        if (g_rebind.active && (clear || remaining <= 0.0f)) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind != RebindKind::Controller) {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_rebind.active; ++i) {
                if (keys[i] && !g_rebind.keys[i] && i != SDL_SCANCODE_F10) CompleteRebind(i);
                g_rebind.keys[i] = keys[i];
            }
            const uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
            for (int i = 1; i <= 5 && g_rebind.active; ++i)
                if (!overControl && g_rebind.kind != RebindKind::MuteHotkey &&
                    (mouse & ~g_rebind.mouse & (1u << (i - 1))) != 0) CompleteRebind(static_cast<uint32_t>(-i - 1));
            g_rebind.mouse = mouse;
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind == RebindKind::Controller) {
            auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
            if (pad != nullptr) {
                for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
                    const bool pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                    if (pressed && !g_rebind.buttons[i]) CompleteRebind(i);
                    g_rebind.buttons[i] = pressed;
                }
                for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
                    const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
                    if (std::abs(value) < 8000) g_rebind.axesReady[i] = true;
                    if (g_rebind.axesReady[i] && std::abs(value) >= 16384)
                        CompleteRebind(PADEncodeAxisButton(i, value < 0));
                }
            }
        }
    }
    if (!g_rebind.active) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawKeyBinding(const char* label, int scancode, RebindKind kind, uint16_t target,
                    float width = 220.0f) {
    const std::string caption = std::string(KeyBindingName(scancode)) + "##binding";
    if (ImT::Button(caption.c_str(), ImVec2(width, 0.0f))) BeginRebind(kind, target, label);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImT::TextUnformatted(label);

}

bool DrawKeyboardSettings(uint32_t port) {
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    bool enabled = buttons != nullptr;
    bool usePreset = false;
    if (ImT::Checkbox("Keyboard and mouse", &enabled)) {
        PADSetKeyboardActive(port, enabled);
        PADSerializeMappings();
        buttons = PADGetKeyButtonBindings(port, &count);
        usePreset = enabled && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        });
    }
    if (!enabled) return false;
    ImT::TextDisabled("Replaces the gamepad on this port. F10 opens settings.");
    if (ImT::Button("Use WASD + mouse preset") || usePreset) {
        const std::array<int, PAD_BUTTON_COUNT> keys = {
            PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q,
            SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        };
        for (size_t i = 0; i < keys.size(); ++i)
            PADSetKeyButtonBinding(port, {keys[i], kControllerButtons[i].padButton});
        const std::array<int, PAD_AXIS_COUNT> axes = {
            SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
            SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
            SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
        };
        uint32_t axisCount = 0;
        auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
        for (uint32_t i = 0; i < axisCount; ++i)
            PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
        PADSerializeMappings();
    }
    ImT::SeparatorText("Button mapping");
    for (uint32_t i = 0; i < count; ++i) {
        int key = buttons[i].scancode;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(PADGetButtonName(buttons[i].padButton), key, RebindKind::KeyboardButton, buttons[i].padButton);
        ImGui::PopID();
    }
    ImT::SeparatorText("Stick and trigger mapping");
    uint32_t axisCount = 0;
    auto* axes = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; i < axisCount; ++i) {
        int key = axes[i].scancode;
        ImGui::PushID(static_cast<int>(count + i));
        const char* direction = PADGetAxisDirectionLabel(axes[i].padAxis);
        const std::string label = std::string(PADGetAxisName(axes[i].padAxis)) + " " +
                                  (direction != nullptr ? direction : "");
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(label.c_str(), key, RebindKind::KeyboardAxis, axes[i].padAxis);
        ImGui::PopID();
    }
    return true;
}

// Controller settings menu: port selection, controller assignment and button mapping.
int ExpressionResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void DrawExpressionSettings() {
    ImT::SeparatorText("Expressions (Dolphin syntax)");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.0f);
    ImT::TextDisabled(
        "Optional. An expression overrides nothing: its result is combined with the "
        "button mapping above. Operators ! & | ^ and functions if, min, max, clamp, "
        "timer, toggle, hold, tap, pulse, smooth, deadzone behave as they do in Dolphin.");
    ImGui::PopTextWrapPos();

    static std::array<std::string, InputBindings::kControls.size()> errors;
    static std::array<std::string, InputBindings::kControls.size()> buffers;
    static std::string importStatus;
    static int loadedPort = -1;
    static bool reloadBuffers = true;
    const auto port = static_cast<uint32_t>(g_controllerPort);

    if (loadedPort != g_controllerPort || reloadBuffers) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            buffers[i] = InputBindings::GetExpression(port, i);
        }
        errors.fill(std::string());
        loadedPort = g_controllerPort;
        reloadBuffers = false;
    }

    if (ImT::Button("Import from Dolphin")) {
        const std::string path = InputBindings::DefaultDolphinConfigPath();
        std::string summary;
        std::string error;
        if (InputBindings::ImportDolphinConfig(path, g_controllerPort + 1, port, summary, error) < 0) {
            importStatus = error;
        } else {
            importStatus = summary;
            errors.fill(std::string());
            reloadBuffers = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImT::SetTooltip("Reads [GCPad%d] from %%APPDATA%%\\Dolphin Emulator\\Config\\GCPadNew.ini,\n"
                          "or GCPadNew.ini next to the executable.", g_controllerPort + 1);
    }
    if (!importStatus.empty()) {
        ImT::TextDisabled("%s", importStatus.c_str());
    }

    for (size_t i = 0; i < InputBindings::kControls.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 2000);
        std::string& text = buffers[i];
        ImGui::SetNextItemWidth(300.0f);
        if (ImT::InputText(InputBindings::kControls[i].label, text.data(), text.capacity() + 1,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                             ExpressionResizeCallback, &text)) {
            std::string error;
            errors[i] = InputBindings::SetExpression(port, i, text, error) ? std::string() : error;
        }
        if (InputBindings::IsActive(port, i)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "active");
        }
        if (!errors[i].empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", errors[i].c_str());
        }
        ImGui::PopID();
    }
}

void DrawRumbleSettings() {
    ImT::SeparatorText("Vibration");
    if (ImT::Checkbox("Controller vibration", &g_rumbleEnabled)) {
        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
        if (!g_rumbleEnabled) {
            // Stop whatever is already running: the game will not send another
            // motor command until its own state machine decides to.
            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
                PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
            };
            PADControlAllMotors(stopAll.data());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImT::SetTooltip("Applies to every port.");
    }
}

void DrawControllerSettings() {
    ImT::TextUnformatted("Every connected gamepad controls Mario (player 1), mapped for its type.");
    ImT::TextDisabled("Pick the controller type or remap buttons in the launcher's Controls page.");
    ImGui::Separator();
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const std::string label = "Port " + std::to_string(port + 1);
        ImT::RadioButton(label.c_str(), &g_controllerPort, port);
        if (port + 1 < PAD_MAX_CONTROLLERS) {
            ImGui::SameLine();
        }
    }

    ImGui::Separator();
    const uint32_t selectedGamePort = static_cast<uint32_t>(g_controllerPort);
    if (DrawKeyboardSettings(selectedGamePort)) {
        return;
    }
    ImGui::Separator();
    const char* currentName = PADGetName(selectedGamePort);
    ImT::Text("Assigned: %s", currentName != nullptr ? currentName : "None");
    if (ImT::MenuItem("Unassign controller")) {
        PADClearPort(selectedGamePort);
        g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
    }
    ImGui::Separator();
    controller_mapping_wizard::DrawSetupList();
    DrawWiiRemoteSettings(selectedGamePort);
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        ImT::TextDisabled("No controller connected");
        return;
    }

    if (ImT::BeginMenu("Assign connected controller")) {
        for (uint32_t index = 0; index < controllerCount; ++index) {
            const char* name = PADGetNameForControllerIndex(index);
            ImGui::PushID(static_cast<int>(index));
            if (ImT::MenuItem(name != nullptr ? name : "Unknown controller")) {
                PADSetPortForIndex(index, selectedGamePort);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        ImT::TextDisabled("Assign a controller to edit its buttons");
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings =
        PADGetAltButtonMappings(static_cast<uint32_t>(g_controllerPort), &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeBindingConfig(primaryNative);
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeBindingConfig(altNative);
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding combo without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != g_controllerPort) {
        altRowExpandedPort = g_controllerPort;
        altRowExpanded.fill(false);
    }

    ImT::SeparatorText("Presets");
    if (ImT::Button("GameCube")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    const auto applyPreset = [&](const std::array<const char*, PAD_BUTTON_COUNT>& preset) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, preset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    };

    ImGui::SameLine();
    if (ImT::Button("Classic Controller Pro")) {
        applyPreset(kClassicProPreset);
    }
    ImGui::SameLine();
    if (ImT::Button("PlayStation")) {
        applyPreset(kPlayStationPreset);
    }

    ImT::SeparatorText("Button mapping");
    ImT::TextDisabled("LT / L2 = left trigger. RT / R2 = right trigger.");
    ImT::TextDisabled("LB / L1 = left shoulder. RB / R1 = right shoulder.");
    ImT::TextDisabled("Click a binding, then press an input. No input for 10 seconds clears it.");
    const float bindingWidth = ImGui::CalcTextSize("Right shoulder (RB / R1)").x +
                               ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        const auto drawThreshold = [&](PADButtonMapping* mapping, bool secondary) {
            if (!PADIsAxisButton(mapping->nativeButton)) return;
            int threshold = static_cast<int>(PADAxisButtonThreshold(mapping->nativeButton));
            ImGui::SetNextItemWidth(bindingWidth);
            if (ImT::SliderInt(secondary ? "##altThreshold" : "##primaryThreshold", &threshold,
                                 1, 100, "Threshold: %d%%", ImGuiSliderFlags_AlwaysClamp)) {
                const PADButtonMapping updated = {
                    PADAxisButtonIdentity(mapping->nativeButton) | (static_cast<uint32_t>(threshold) << 8),
                    mapping->padButton,
                };
                if (secondary) PADSetAltButtonMapping(selectedGamePort, updated);
                else PADSetButtonMapping(selectedGamePort, updated);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                writeBinding(i, mappingIt->nativeButton,
                             altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                PADSerializeMappings();
            }
        };
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(bindingWidth);
        const std::string primaryCaption = std::string(current.label) + "##primary";
        if (ImT::Button(primaryCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
            BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label);
        }
        drawThreshold(mappingIt, false);
        ImGui::EndGroup();
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImT::SmallButton("+")) {
                    altRowExpanded[i] = true;
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                if (ImGui::IsItemHovered()) {
                    ImT::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImT::TextUnformatted("or");
                ImGui::SameLine();
                ImGui::BeginGroup();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                ImGui::SetNextItemWidth(bindingWidth);
                const std::string altCaption = std::string(altLabel) + "##alt";
                if (ImT::Button(altCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                drawThreshold(altIt, true);
                ImGui::EndGroup();
            }
        }
        ImGui::SameLine();
        ImT::TextUnformatted(kControllerButtons[i].label);
        ImGui::PopID();
    }
    DrawExpressionSettings();
    DrawRumbleSettings();
}

void DrawAudioSettings() {
    ImGui::SetNextItemWidth(220.0f);
    if (ImT::SliderInt("Master", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    if (ImT::SliderInt("Music", &g_musicVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_musicVolumePercent) / 100.0f;
        MusicAttenuation::SetMusicVolume(volume);
        RuntimeConfigFile::SetMusicVolume(volume);
    }
    if (ImT::SliderInt("Sound Effects", &g_soundEffectsVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_soundEffectsVolumePercent) / 100.0f;
        MusicAttenuation::SetSoundEffectsVolume(volume);
        RuntimeConfigFile::SetSoundEffectsVolume(volume);
    }
    if (ImT::SliderInt("UI", &g_uiVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_uiVolumePercent) / 100.0f;
        MusicAttenuation::SetUiVolume(volume);
        RuntimeConfigFile::SetUiVolume(volume);
    }
    if (ImT::SliderInt("Voices", &g_voicesVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_voicesVolumePercent) / 100.0f;
        MusicAttenuation::SetVoicesVolume(volume);
        RuntimeConfigFile::SetVoicesVolume(volume);
    }
    const float labelColumn = ImGui::GetCursorPosX() + ImGui::CalcItemWidth();
    if (ImT::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::SameLine();
    DrawKeyBinding("Mute shortcut", g_muteHotkey, RebindKind::MuteHotkey, 0,
                   std::max(60.0f, labelColumn - ImGui::GetCursorPosX()));
    ImGui::Separator();
    if (ImT::Checkbox("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered()) {
        ImT::SetTooltip(
            "Runs the AX/DSP voice mix off the game thread. Turn this off if you "
            "suspect an audio problem; the mix then runs inline as it used to.");
    }
    ImGui::Separator();
    if (ImT::Checkbox("Mute game music while external media is playing",
                        &g_attenuateMusicWhenMediaPlays)) {
        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
    }
#if defined(__APPLE__)
    if (ImGui::IsItemHovered()) {
        ImT::SetTooltip(
            "Detects other apps with active audio output on macOS 14.2 or later. "
            "Apps that keep an output stream running silently may keep game music muted.");
    }
#endif
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            ImT::TextDisabled("External media is playing; game music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            ImT::TextDisabled("Checking external audio...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
#if defined(__APPLE__)
            ImT::TextDisabled("External audio detection unavailable (requires macOS 14.2 or later).");
#else
            ImT::TextDisabled("Media controls are unavailable.");
#endif
        } else {
            ImT::TextDisabled("No external media is currently playing.");
        }
    }
}

// LAN Play: this PC's addresses for other players, and the devices asked for rooms directly
// (Tailscale doesn't carry broadcasts; netplay_lobby.h). Peers from `tailscale status` are asked
// anyway; this is for typing in a device's Tailscale name or address by hand.
void DrawLanPlaySettings() {
    static std::vector<std::string> ownAddresses;
    static uint64_t ownAddressesAt = 0;
    if (ownAddressesAt == 0 || SDL_GetTicks() - ownAddressesAt > 2000) {
        ownAddresses = NetplayLobby::OwnAddresses();
        ownAddressesAt = SDL_GetTicks();
    }
    ImT::TextUnformatted("Rooms are found on your Wi-Fi by themselves, and over Tailscale through");
    ImT::TextUnformatted("the devices in `tailscale status` and the ones below.");
    ImGui::Separator();
    ImT::TextUnformatted("This PC (tell other players one of these):");
    for (const auto& address : ownAddresses) {
        ImT::BulletText("%s%s", address.c_str(), address.rfind("100.", 0) == 0 ? "  (Tailscale)" : "");
    }
    ImGui::Separator();
    ImT::TextUnformatted("Devices asked for rooms directly:");
    const auto known = NetplayLobby::KnownDevices();
    if (known.empty()) {
        ImT::TextDisabled("none yet: playing together adds them");
    }
    for (const auto& device : known) {
        ImGui::PushID(device.c_str());
        if (ImT::SmallButton("Remove")) {
            NetplayLobby::RemoveKnownDevice(device);
        }
        ImGui::SameLine();
        ImT::TextUnformatted(device.c_str());
        ImGui::PopID();
    }
    static char entry[128] = {};
    static std::string result;
    ImGui::SetNextItemWidth(260.0f);
    const bool submitted = ImT::InputTextWithHint("##add-device", "Tailscale name or address", entry, sizeof(entry),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImT::Button("Add") || submitted) && entry[0] != '\0') {
        result = NetplayLobby::AddKnownDevice(entry) ? std::string("Added ") + entry : "Already in the list, or not a name";
        entry[0] = '\0';
    }
    if (!result.empty()) {
        ImT::TextDisabled("%s", result.c_str());
    }
}

void DrawGraphicsSettings() {
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    // NSMBW: the aspect (original 4:3, 16:9, fill) lives in the Aspect menu (DisplaySettings).
    struct EffectFlag {
        const char* label;
        uint32_t flag;
    };
    static constexpr std::array<EffectFlag, 1> kEffectFlags = {{
        {"Disable bloom", 0x10u},
    }};

    for (const auto& effect : kEffectFlags) {
        bool disabled = (g_disabledPostProcessingPaths & effect.flag) != 0;
        if (ImT::Checkbox(effect.label, &disabled)) {
            if (disabled) {
                g_disabledPostProcessingPaths |= effect.flag;
            } else {
                g_disabledPostProcessingPaths &= ~effect.flag;
            }
            RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
            RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
        }
    }
    ImT::TextDisabled("Applied when the next scene renderer is created.");
    ImGui::Separator();
    static constexpr const char* kDisplayModes[] = {
        "Windowed",
        "Borderless fullscreen",
        "Exclusive fullscreen",
    };
    if (ImT::Combo("Display mode", &g_displayMode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)))) {
        const auto mode = static_cast<AuroraDisplayMode>(g_displayMode);
        aurora_set_display_mode(mode);
        const AuroraDisplayMode activeMode = aurora_get_display_mode();
        if (activeMode == mode) {
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        } else {
            g_displayMode = static_cast<int>(activeMode);
        }
    }
    if (g_displayMode == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        ImT::TextDisabled(
            "Requests the closest native-resolution display mode to the output frame "
            "rate (60 Hz, or the frame interpolation target).");
    }
    constexpr std::array<const char*, 4> kFrameInterpolationModes{
        "Off", "120 FPS", "180 FPS", "240 FPS (fast PCs)",
    };
    const char* currentFrameInterpolationMode =
        kFrameInterpolationModes[static_cast<size_t>(g_frameInterpolationMode)];
    bool frameInterpolationModeChanged = false;
    if (ImT::BeginCombo("Frame interpolation (experimental)", currentFrameInterpolationMode)) {
        for (int mode = 0; mode < static_cast<int>(kFrameInterpolationModes.size()); ++mode) {
            const bool selected = g_frameInterpolationMode == mode;
            if (ImT::Selectable(kFrameInterpolationModes[static_cast<size_t>(mode)], selected)) {
                g_frameInterpolationMode = mode;
                frameInterpolationModeChanged = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (frameInterpolationModeChanged) {
        const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
        aurora_set_frame_interpolation_fps(targetFps);
        RuntimeConfigFile::SetFrameInterpolationFps(targetFps);
        LimitResolutionForFrameRate();
        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
            // Re-apply exclusive mode so the display refresh tracks the new target.
            aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
        }
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImT::TextDisabled("Frame interpolation is experimental, you might find visual artifacts");
    ImGui::PopTextWrapPos();
    if (bool vsync = g_vsync.load(); ImT::Checkbox("Vsync (no screen tearing)", &vsync)) {
        SetVSync(vsync);
        RuntimeConfigFile::SetVSync(vsync);
    }
#if defined(__APPLE__)
    const bool metalFxSupported = aurora_is_metalfx_spatial_supported();
    ImGui::BeginDisabled(!metalFxSupported);
    if (ImT::Checkbox("MetalFX spatial upscaling", &g_metalFxSpatialUpscaling)) {
        aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
        RuntimeConfigFile::SetMetalFxSpatialUpscaling(g_metalFxSpatialUpscaling);
    }
    ImGui::EndDisabled();
    switch (aurora_get_metalfx_status()) {
    case AURORA_METALFX_ACTIVE:
        ImT::TextDisabled("Active: upscaling the game image before the overlay.");
        break;
    case AURORA_METALFX_NOT_UPSCALING:
        ImT::TextDisabled("Choose a lower internal resolution to use MetalFX.");
        break;
    case AURORA_METALFX_UNSUPPORTED:
        ImT::TextDisabled("Requires macOS 13+, Metal, and a MetalFX-capable GPU.");
        break;
    case AURORA_METALFX_ERROR:
        ImT::TextDisabled("Unavailable after a renderer error; toggle off and on to retry.");
        break;
    case AURORA_METALFX_DISABLED:
        if (metalFxSupported) {
            ImT::TextDisabled("Render below output resolution for sharper lower-cost output.");
        } else {
            ImT::TextDisabled("MetalFX spatial upscaling is unavailable on this device.");
        }
        break;
    }
#endif
    if (ImT::Checkbox("Disable copy filter", &g_disableCopyFilter)) {
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (ImT::Checkbox("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    if (ImT::Checkbox("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
    ImGui::Separator();
    ImT::Text("Graphics API: %s", GraphicsApiDisplayName());
}

// The last frames' times (the UI's frame is the presented one), for the graph.
constexpr int kFrameTimeCount = 180;
float g_frameTimes[kFrameTimeCount] = {};
int g_frameTimeNext = 0;

void DrawFrameGraph() {
    float last = g_frameTimes[(g_frameTimeNext + kFrameTimeCount - 1) % kFrameTimeCount];
    float peak = 0.0f, sum = 0.0f;
    int samples = 0;
    for (float t : g_frameTimes) {
        if (t <= 0.0f) continue;
        peak = std::max(peak, t);
        sum += t;
        ++samples;
    }
    ImT::Text("Frame: %.1f ms", last);
    ImT::TextDisabled("avg %.1f  max %.1f", samples > 0 ? sum / samples : 0.0f, peak);
    const float scale = ImGui::GetFontSize() / 13.0f;
    const ImVec2 size(kFrameTimeCount * scale, 50.0f * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* list = ImGui::GetWindowDrawList();
    // 0 to 33.3 ms (two 60 Hz frames), more when a frame took longer.
    const float top = std::max(100.0f / 3.0f, peak * 1.1f);
    const auto yOf = [&](float ms) { return origin.y + size.y - std::min(ms / top, 1.0f) * size.y; };
    list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 90));
    for (const float line : {1000.0f / 60.0f, 1000.0f / 30.0f}) {
        if (line < top) {
            list->AddLine(ImVec2(origin.x, yOf(line)), ImVec2(origin.x + size.x, yOf(line)), IM_COL32(255, 255, 255, 60));
        }
    }
    const float w = size.x / kFrameTimeCount;
    for (int i = 0; i < kFrameTimeCount; ++i) {
        const float t = g_frameTimes[(g_frameTimeNext + i) % kFrameTimeCount];
        if (t <= 0.0f) continue;
        const ImU32 color = t < 1000.0f / 55.0f ? IM_COL32(90, 220, 90, 230)
                            : t < 1000.0f / 28.0f ? IM_COL32(240, 200, 60, 230)
                                                  : IM_COL32(240, 80, 70, 240);
        const float x = origin.x + i * w;
        list->AddRectFilled(ImVec2(x, yOf(t)), ImVec2(x + std::max(w, 1.0f), origin.y + size.y), color);
    }
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    {
        const float dt = ImGui::GetIO().DeltaTime;
        g_frameTimes[g_frameTimeNext] = dt > 0.0f && dt < 1.0f ? dt * 1000.0f : 0.0f;
        g_frameTimeNext = (g_frameTimeNext + 1) % kFrameTimeCount;
    }
    if (!g_showFps && !g_frameGraph) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
        if (!g_showFps) {
        } else if (presentTiming.sampleCount == 0) {
            ImT::TextUnformatted("FPS: --");
        } else {
            // Present timing includes the additional frames produced by
            // interpolation, so this remains the actual displayed FPS.
            ImT::Text("FPS: %.1f", presentTiming.framesPerSecond);
            // Replay-unsafe frames hold the presented cadence with duplicated
            // slots, so the counter alone reads 180 while the motion on screen
            // is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond <
                presentTiming.framesPerSecond * 0.95) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Motion: %.1f",
                                   presentTiming.effectiveFramesPerSecond);
            }
        }
        if (g_frameGraph) {
            DrawFrameGraph();
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImT::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Wiicompiled Startup", nullptr, kFlags)) {
        // The credits, in the game's own fonts when the game folder is readable (it always is once
        // the game can start), else in the overlay's font.
        constexpr const char* kTitle = "Made by JosiahG";
        constexpr const char* kCredit =
            "A Super Paper Mario PC recomp. Built on WiiCompiled's code.";
        // At the bottom of the screen.
        constexpr const char* kReport = "";
        // Super Paper Mario's own font, read from the game folder (font/papermarioset_US.bfn).
        static const std::shared_ptr<GameLayout::Font> pmFont = GameLayout::LoadFont("papermarioset_US.bfn");
        float startY = 0.0f;
        float titleHeight = ImGui::GetTextLineHeight();
        const bool gameFonts = GameLayout::FontReady(pmFont.get());
        char shaderLine[96] = {};
        if (g_bootShaderNotice && !g_bootShadersReady.load(std::memory_order_relaxed)) {
            std::snprintf(shaderLine, sizeof(shaderLine), "Compiling shaders... %u to go",
                          aurora_get_queued_pipeline_count());
        }
        if (gameFonts) {
            // A chapter-card style cover: storybook sky, paper hills, the heroes around a tilted
            // paper card, Tippi fluttering past. Sprites come from Play/startup/*.png.
            struct Sprite { ImTextureID id{}; float w = 0, h = 0; };
            static std::unordered_map<std::string, Sprite> sprites;
            auto Spr = [&](const char* name) -> const Sprite& {
                auto it = sprites.find(name);
                if (it != sprites.end()) return it->second;
                Sprite sp;
                uint32_t w = 0, h = 0;
                const std::string file = name[0] == '@' ? std::string(name + 1) : std::string("startup/") + name + ".png";  // '@' = a path
                if (uint8_t* rgba = aurora_load_png_rgba(file.c_str(), &w, &h)) {
                    sp.id = aurora_imgui_add_texture(w, h, rgba);
                    sp.w = float(w);
                    sp.h = float(h);
                    std::free(rgba);
                }
                return sprites.emplace(name, sp).first->second;
            };
            ImDrawList* list = ImGui::GetWindowDrawList();
            const GameLayout::View view = GameLayout::GameView();
            const float t = static_cast<float>(ImGui::GetTime());
            const ImVec2 o = viewport->Pos, sz = viewport->Size;
            const float k = view.scale;
            auto S = [&](float x, float y) { return ImVec2(view.centerX + x * k, view.centerY - y * k); };
            // Sprite of height `hgt` layout units centred at (x, y), tilted by `ang`, optionally mirrored.
            auto Draw = [&](const char* name, float x, float y, float hgt, float ang, bool flip, uint32_t col = IM_COL32_WHITE) {
                const Sprite& sp = Spr(name);
                if (!sp.id || sp.h <= 0) return;
                const float hw = hgt * sp.w / sp.h * 0.5f, hh = hgt * 0.5f;
                const float c = std::cos(ang), sn = std::sin(ang);
                auto P = [&](float dx, float dy) { return S(x + dx * c - dy * sn, y + dx * sn + dy * c); };
                const float u0 = flip ? 1.f : 0.f, u1 = flip ? 0.f : 1.f;
                list->AddImageQuad(sp.id, P(-hw, hh), P(hw, hh), P(hw, -hh), P(-hw, -hh), ImVec2(u0, 0), ImVec2(u1, 0),
                                   ImVec2(u1, 1), ImVec2(u0, 1), col);
            };
            auto Shadow = [&](float x, float y, float w) {
                const ImVec2 c = S(x, y);
                ImVec2 pts[24];
                for (int j = 0; j < 24; ++j)
                    pts[j] = ImVec2(c.x + std::cos(j * 0.2618f) * w * k, c.y + std::sin(j * 0.2618f) * w * 0.18f * k);
                list->AddConvexPolyFilled(pts, 24, IM_COL32(20, 50, 20, 90));
            };
            // Sky, halftone dots, drifting paper clouds.
            list->AddRectFilledMultiColor(o, ImVec2(o.x + sz.x, o.y + sz.y), IM_COL32(64, 150, 230, 255),
                                          IM_COL32(64, 150, 230, 255), IM_COL32(170, 222, 255, 255),
                                          IM_COL32(170, 222, 255, 255));
            const float dot = std::max(6.0f, 14.0f * k);
            for (float y = o.y + sz.y * 0.3f; y < o.y + sz.y; y += dot * 1.6f)
                for (float x = o.x + std::fmod(y, dot * 1.6f); x < o.x + sz.x; x += dot * 1.6f)
                    list->AddCircleFilled(ImVec2(x, y), dot * 0.2f * (y - o.y) / sz.y, IM_COL32(255, 255, 255, 55), 8);
            for (int i = 0; i < 5; ++i) {
                const float span = sz.x + 400.0f * k;
                const float cx = o.x + std::fmod(i * span / 5.0f + t * (12.0f + i * 4.0f) * k, span) - 200.0f * k;
                const float cy = o.y + sz.y * (0.08f + 0.06f * (i % 3));
                const float r = (26.0f + 7.0f * (i % 2)) * k;
                for (int j = 0; j < 3; ++j)
                    list->AddCircleFilled(ImVec2(cx + (j - 1) * r * 1.1f + 3 * k, cy + (j == 1 ? -r * 0.35f : 0.f) + 3 * k), r, IM_COL32(40, 80, 140, 60), 24);
                for (int j = 0; j < 3; ++j)
                    list->AddCircleFilled(ImVec2(cx + (j - 1) * r * 1.1f, cy + (j == 1 ? -r * 0.35f : 0.f)), r, IM_COL32(255, 255, 255, 240), 24);
            }
            for (int i = 0; i < 9; ++i) {
                const float r = (110.0f + 40.0f * (i % 3)) * k;
                const ImVec2 c(o.x + sz.x * i / 8.0f, o.y + sz.y + r * 0.35f);
                list->AddCircleFilled(c, r + 4 * k, IM_COL32(20, 40, 20, 255), 48);
                list->AddCircleFilled(c, r, (i % 2) ? IM_COL32(86, 178, 74, 255) : IM_COL32(64, 150, 64, 255), 48);
            }
            // The card.
            const float bob = std::sin(t * 1.6f) * 3.0f;
            const float ang = -0.025f + std::sin(t * 0.9f) * 0.006f;
            auto R = [&](float x, float y, float dx, float dy) {
                const float c = std::cos(ang), sn = std::sin(ang);
                return S(dx + x * c - y * sn, dy + bob + x * sn + y * c);
            };
            const float hw = 270.0f, hh = 140.0f, cardY = 20.0f;
            auto Q = [&](float dx, float dy, uint32_t col) {
                list->AddQuadFilled(R(-hw, cardY + hh, dx, dy), R(hw, cardY + hh, dx, dy), R(hw, cardY - hh, dx, dy), R(-hw, cardY - hh, dx, dy), col);
            };
            Q(12, -12, IM_COL32(0, 0, 0, 80));
            Q(0, 0, IM_COL32(255, 252, 240, 255));
            // Ruled lines, like a page from the Light Prognosticus.
            for (int i = 0; i < 6; ++i) {
                const float ly = cardY + hh - 50.0f - i * 40.0f;
                list->AddLine(R(-hw + 20, ly, 0, 0), R(hw - 20, ly, 0, 0), IM_COL32(170, 200, 230, 110), 2.0f * k);
            }
            list->AddQuad(R(-hw, cardY + hh, 0, 0), R(hw, cardY + hh, 0, 0), R(hw, cardY - hh, 0, 0), R(-hw, cardY - hh, 0, 0), IM_COL32(24, 18, 12, 255), 6.0f * k);
            list->AddTriangleFilled(R(hw - 40, cardY - hh, 0, 0), R(hw, cardY - hh + 40, 0, 0), R(hw - 40, cardY - hh + 40, 0, 0), IM_COL32(214, 206, 186, 255));
            list->AddTriangle(R(hw - 40, cardY - hh, 0, 0), R(hw, cardY - hh + 40, 0, 0), R(hw - 40, cardY - hh + 40, 0, 0), IM_COL32(24, 18, 12, 255), 3.0f * k);
            // Text, shrunk to fit inside the card.
            const float maxW = 2 * hw - 70.0f;
            auto Fit = [&](const std::u16string& u, float size) {
                const float w = GameLayout::MeasureText(*pmFont, u, size);
                return w > maxW ? size * maxW / w : size;
            };
            auto Outlined = [&](const char* text, float y, float size, uint32_t top, uint32_t bottom, float ink) {
                const auto u = GameLayout::Utf16(text);
                const float fs = Fit(u, size);
                for (int j = 0; j < 12; ++j) {
                    const float a = j * 0.5236f;
                    GameLayout::DrawText(list, view, *pmFont, u, std::cos(a) * ink, y + bob + std::sin(a) * ink, fs,
                                         0x18120CFFu, 0x18120CFFu, 1.0f, 1, 1);
                }
                GameLayout::DrawText(list, view, *pmFont, u, 0.0f, y + bob, fs, top, bottom, 1.0f, 1, 1);
            };
            Outlined("NEW SUPER PAPER BROS.", cardY + 88.0f, 40.0f, 0xFFF26BFFu, 0xFF9A1EFFu, 3.0f);
            Outlined("Made by JosiahG", cardY + 32.0f, 48.0f, 0xFF5A4AFFu, 0xC81E1EFFu, 3.0f);
            {
                const auto u = GameLayout::Utf16(kCredit);
                GameLayout::DrawText(list, view, *pmFont, u, 0.0f, cardY - 22.0f + bob, Fit(u, 17.0f), 0x3A2A18FFu,
                                     0x3A2A18FFu, 1.0f, 1, 1);
                // How to reach the settings, along the card's bottom edge.
                const auto hint = GameLayout::Utf16("F10 = settings menu   F11 = fullscreen / window");
                GameLayout::DrawText(list, view, *pmFont, hint, 0.0f, cardY - hh + 16.0f + bob, Fit(hint, 12.0f),
                                     0x6A5A48FFu, 0x6A5A48FFu, 1.0f, 1, 1);
            }
            if (shaderLine[0] != '\0') {
                const float bw = 180.0f, by = cardY - 70.0f;
                list->AddRectFilled(R(-bw, by + 10, 0, 0), R(bw, by - 10, 0, 0), IM_COL32(24, 18, 12, 255), 5.0f * k);
                list->AddRectFilled(R(-bw + 4, by + 6, 0, 0), R(bw - 4, by - 6, 0, 0), IM_COL32(250, 240, 210, 255), 3.0f * k);
                const float p = std::fmod(t * 0.6f, 1.0f);
                const float sx = -bw + 4 + p * (2 * bw - 58);
                list->AddRectFilled(R(sx, by + 6, 0, 0), R(sx + 50, by - 6, 0, 0), IM_COL32(255, 196, 40, 255), 3.0f * k);
                const auto u = GameLayout::Utf16(shaderLine);
                GameLayout::DrawText(list, view, *pmFont, u, 0.0f, by - 26.0f + bob, Fit(u, 15.0f), 0x3A2A18FFu, 0x3A2A18FFu,
                                     1.0f, 1, 1);
            } else {
                const float pop = 1.0f + 0.06f * std::sin(t * 5.0f), py = cardY - 80.0f + bob;
                // The start button of what the player is holding: the controller's own jump button (its icon), else
                // the keyboard's jump key. Both are 2 on the Wii Remote, which the cover waits for (frame_input.cpp).
                const int padStyle = aurora_button_icon_style();
                static const char* const kStyleDirs[3] = {"xbox", "playstation", "nintendo"};
                const std::string icon = padStyle >= 0 && padStyle < 3
                    ? "@" + RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ApplicationDataDirectory() / "button_icons" /
                                                          kStyleDirs[padStyle] / "ui" / "jump.png")
                    : std::string();
                const auto press = GameLayout::Utf16(icon.empty() ? "Press " + Keybinds::KeyLabel(Keybinds::kJump) + " to start!" : "Press");
                const auto rest = GameLayout::Utf16("to start!");
                const float fs = Fit(icon.empty() ? press : GameLayout::Utf16("Press A or 2 to start!"), 26.0f) * pop;
                const float iconSize = 34.0f * pop, gap = 8.0f;
                const float wPress = GameLayout::MeasureText(*pmFont, press, fs);
                const float wRest = icon.empty() ? 0.0f : GameLayout::MeasureText(*pmFont, rest, fs);
                float x = -(wPress + (icon.empty() ? 0.0f : gap + iconSize + gap + wRest)) * 0.5f;
                const auto outlined = [&](const std::u16string& u, float at) {  // white outline, blue fill, like the title
                    for (int j = 0; j < 12; ++j) {
                        const float a = j * 0.5236f;
                        GameLayout::DrawText(list, view, *pmFont, u, at + std::cos(a) * 2.5f, py + std::sin(a) * 2.5f, fs,
                                             0xFFFFFFFFu, 0xFFFFFFFFu, 1.0f, 0, 1);
                    }
                    GameLayout::DrawText(list, view, *pmFont, u, at, py, fs, 0x3D8BFFFFu, 0x1A3FA0FFu, 1.0f, 0, 1);
                };
                outlined(press, x);
                if (!icon.empty()) {
                    x += wPress + gap;
                    Draw(icon.c_str(), x + iconSize * 0.5f, py, iconSize, 0.0f, false);
                    outlined(rest, x + iconSize + gap);
                }
            }
            // The heroes flank the card; Peach waves from the hills.
            Shadow(-345.0f, -118.0f, 62.0f);
            Draw("mario", -345.0f, -32.0f + std::abs(std::sin(t * 2.2f)) * 10.0f, 170.0f, -0.06f + std::sin(t * 2.2f) * 0.03f, false);
            Shadow(345.0f, -118.0f, 58.0f);
            Draw("luigi", 345.0f, -36.0f + std::abs(std::sin(t * 2.2f + 1.3f)) * 12.0f, 160.0f, 0.06f + std::sin(t * 2.2f + 1.3f) * 0.03f, false);
            Draw("peach", -150.0f, -175.0f + std::sin(t * 1.4f) * 4.0f, 110.0f, std::sin(t * 1.4f) * 0.04f, false);
            // Bowser stomps in from the bottom right, in front of the card.
            Shadow(200.0f, -232.0f, 70.0f);
            Draw("bowser", 200.0f, -160.0f + std::abs(std::sin(t * 1.3f)) * 6.0f, 150.0f, -0.04f + std::sin(t * 1.3f) * 0.03f, false);
            Draw("pixl", 40.0f, -185.0f + std::sin(t * 2.6f) * 8.0f, 60.0f, std::sin(t * 1.9f) * 0.15f, false);
            // Tippi loops around the card, trailing sparkles.
            const float tx = std::sin(t * 0.7f) * 330.0f, ty = 200.0f + std::sin(t * 1.4f) * 30.0f;
            for (int j = 1; j <= 5; ++j) {
                const float tt = t - j * 0.12f;
                Draw("sparkle", std::sin(tt * 0.7f) * 330.0f, 200.0f + std::sin(tt * 1.4f) * 30.0f, 34.0f - j * 4.0f, tt * 2.0f, false,
                     IM_COL32(255, 255, 255, 220 - j * 35));
            }
            Draw("tippi", tx, ty + std::sin(t * 9.0f) * 3.0f, 64.0f, std::cos(t * 0.7f) * 0.25f, std::cos(t * 0.7f) < 0.0f);
            // Twinkles scattered over the sky.
            const float tw[6][2] = {{-330, 165}, {-210, 230}, {250, 235}, {360, 150}, {-60, 245}, {110, 175}};
            for (int j = 0; j < 6; ++j) {
                const float s2 = 0.5f + 0.5f * std::sin(t * 2.4f + j * 1.7f);
                Draw("sparkle", tw[j][0], tw[j][1], 26.0f * s2 + 6.0f, t + j, false, IM_COL32(255, 255, 255, int(120 + 135 * s2)));
            }
            startY = view.centerY + 200.0f * k;
            titleHeight = 0.0f;
        } else {
            ImGui::SetWindowFontScale(1.25f);
            const ImVec2 titleSize = ImGui::CalcTextSize(kTitle);
            startY = std::max(0.0f, viewport->Size.y * 0.3f);
            ImGui::SetCursorPos(ImVec2(std::max(0.0f, (viewport->Size.x - titleSize.x) * 0.5f), startY));
            ImT::TextUnformatted(kTitle);
            ImGui::SetWindowFontScale(1.0f);
            const float wrapWidth = std::min(viewport->Size.x - 48.0f, 640.0f);
            ImGui::SetCursorPos(ImVec2((viewport->Size.x - wrapWidth) * 0.5f, startY + titleSize.y * 2.0f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
            ImT::TextUnformatted(kCredit);
            ImGui::PopTextWrapPos();
            startY = ImGui::GetCursorPosY();
            const ImVec2 reportSize = ImGui::CalcTextSize(kReport);
            ImGui::SetCursorPos(ImVec2(std::max(0.0f, (viewport->Size.x - reportSize.x) * 0.5f),
                                       viewport->Size.y - reportSize.y - 24.0f));
            ImT::TextUnformatted(kReport);
            titleHeight = 0.0f;
        }
        const ImVec2 titleSize{0.0f, titleHeight};
        if (!gameFonts && shaderLine[0] != '\0') {
            ImGui::SetWindowFontScale(0.9f);
            const float lineX = std::max(0.0f, (viewport->Size.x - ImGui::CalcTextSize(shaderLine).x) * 0.5f);
            ImGui::SetCursorPos(ImVec2(lineX, startY + titleSize.y * 1.8f));
            ImT::TextUnformatted(shaderLine);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void UpdateBootShaderState() {
    if (g_bootShadersReady.load(std::memory_order_relaxed)) {
        return;
    }
    const auto now = Clock::now();
    if (g_bootShaderWaitStart == Clock::time_point{}) {
        g_bootShaderWaitStart = now;
    }
    const uint32_t queued = aurora_get_queued_pipeline_count();
    g_bootShaderNotice |= queued > kBootShaderNoticeThreshold;
    if (queued == 0 || now - g_bootShaderWaitStart > kBootShaderWaitLimit) {
        g_bootShadersReady.store(true, std::memory_order_release);
    }
}

void DrawExitPrompt() {
    constexpr const char* kTitle = "Exit";
    if (g_exitPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &g_exitPromptOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImT::TextUnformatted("Quit the game?");
    if (ImT::Button("Exit", ImVec2(120.0f, 0.0f))) ExitForAuroraWindowClose();
    ImGui::SameLine();
    if (ImT::Button("Cancel", ImVec2(120.0f, 0.0f))) g_exitPromptOpen = false;
    if (!g_exitPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}


// Debug > Warp: every chapter room (spm_warp_rooms.inc, from the game's own map changes). The game switches room
// on its next frame (SpmMapSweep::RequestWarp); story progress and flags stay as they are.
struct WarpRoom { const char* map; const char* door; const char* group; const char* label; };
constexpr WarpRoom kWarpRooms[] = {
#include "spm_warp_rooms.inc"
};

bool ContainsNoCase(const char* hay, const char* needle) {
    std::string h = hay, n = needle;
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

void DrawWarpMenu() {
    static char mapName[32] = "mac_02", doorName[32] = "";
    ImGui::SetNextItemWidth(160.0f);
    ImT::InputText(Tr("Map"), mapName, sizeof mapName);
    ImGui::SetNextItemWidth(160.0f);
    ImT::InputText(Tr("Door (optional)"), doorName, sizeof doorName);
    static bool usePos = false;
    static float arrival[3] = {};
    ImT::Checkbox(Tr("Arrive at X Y Z"), &usePos);
    ImGui::SameLine();
    ImGui::BeginDisabled(!usePos);
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputFloat3("##arrival", arrival, "%.0f");
    ImGui::EndDisabled();
    if (ImT::Button(Tr("Warp to this map"))) {
        SpmMapSweep::RequestWarp(mapName, doorName, usePos ? arrival : nullptr);
        SetTopBarVisible(false);
    }
    ImT::TextDisabled("%s", Tr("Warp: story progress and flags are not changed."));
    ImGui::Separator();

    static std::vector<const char*> groups;
    if (groups.empty()) {
        for (const auto& room : kWarpRooms)
            if (groups.empty() || std::strcmp(groups.back(), room.group) != 0) groups.push_back(room.group);
    }
    static int group = 0;
    static char roomFilter[48] = "";
    ImGui::SetNextItemWidth(260.0f);
    ImT::InputTextWithHint("##roomsearch", Tr("Search..."), roomFilter, sizeof roomFilter);
    const float leftWidth = 260.0f * std::max(1.0f, ImGui::GetIO().FontGlobalScale);
    if (ImGui::BeginChild("##warpgroups", ImVec2(leftWidth, 0.0f), true)) {
        for (int i = 0; i < static_cast<int>(groups.size()); ++i)
            if (ImT::Selectable(groups[i], group == i && !roomFilter[0])) { group = i; roomFilter[0] = 0; }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##warprooms", ImVec2(0.0f, 0.0f), true)) {
        for (const auto& room : kWarpRooms) {
            const bool shown = roomFilter[0] ? (ContainsNoCase(room.label, roomFilter) || ContainsNoCase(room.map, roomFilter))
                                             : std::strcmp(room.group, groups[group]) == 0;
            if (!shown) continue;
            if (ImT::Selectable(room.label)) {
                SpmMapSweep::RequestWarp(room.map, room.door, usePos ? arrival : nullptr);
                SetTopBarVisible(false);
            }
        }
    }
    ImGui::EndChild();
}

struct DebugItemName { int id; const char* names[5]; };   // en, de, fr, es, it
constexpr DebugItemName kDebugItems[] = {
#include "spm_item_names.inc"
};


const char* DebugName(const DebugItemName& item) { return item.names[DebugLanguageIndex()]; }

void DrawItemsMenu() {
    static int itemId = 1;
    static char filter[48] = "";
    ImGui::SetNextItemWidth(220.0f);
    ImT::InputText(Tr("Filter"), filter, sizeof filter);
    if (ImGui::BeginChild("##debugitems", ImVec2(300.0f, 220.0f), true)) {
        std::string needle = filter;
        for (char& c : needle) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        for (const auto& item : kDebugItems) {
            char label[96];
            if (std::strcmp(item.names[0], DebugName(item)) != 0)
                std::snprintf(label, sizeof label, "%3d  %s  (%s)", item.id, DebugName(item), item.names[0]);
            else
                std::snprintf(label, sizeof label, "%3d  %s", item.id, DebugName(item));
            if (!needle.empty()) {
                std::string hay = label;
                for (const char* name : item.names) { hay += ' '; hay += name; }
                for (char& c : hay) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (hay.find(needle) == std::string::npos) continue;
            }
            if (ImT::Selectable(label, itemId == item.id)) itemId = item.id;
        }
    }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(120.0f);
    ImT::InputInt(Tr("Item id"), &itemId);
    if (ImT::Button(Tr("Add to inventory"))) SpmMapSweep::AddItem(itemId);
    ImGui::SameLine();
    if (ImT::Button(Tr("Remove"))) SpmMapSweep::RemoveItem(itemId);
    ImT::TextDisabled(Tr("Add returns %d (1 = added, 0 = no room)"), SpmMapSweep::LastResult());
}

void DrawPartyMenu() {
    ImT::TextDisabled("%s", Tr("Adds the character / Pixl and makes it selectable."));
    for (const auto& item : kDebugItems) {
        if (item.id < 216 || item.id > 231) continue;
        char label[96];
        std::snprintf(label, sizeof label, "%s  (%d)", DebugName(item), item.id);
        if (ImT::MenuItem(label)) SpmMapSweep::Unlock(item.id, item.id >= 220);
    }
}

void DrawPlayerMenu() {
    static int coins = 100, hp = 100, maxHp = 100;
    ImGui::SetNextItemWidth(120.0f);
    ImT::InputInt(Tr("Coins"), &coins);
    ImGui::SameLine();
    if (ImT::Button(Tr("Add coins"))) SpmMapSweep::AddCoins(coins);
    ImGui::SetNextItemWidth(120.0f);
    ImT::InputInt(Tr("HP"), &hp);
    ImGui::SetNextItemWidth(120.0f);
    ImT::InputInt(Tr("Max HP"), &maxHp);
    if (ImT::Button(Tr("Set HP"))) SpmMapSweep::SetHp(hp, maxHp);
}

void DrawStoryMenu() {
    static int story = 19;
    ImT::Text(Tr("Story position (GSW 0): %d"), SpmMapSweep::Gsw0());
    ImGui::SetNextItemWidth(160.0f);
    ImT::InputInt("##story", &story);
    ImGui::SameLine();
    ImGui::PushID("story");   // several "Set"/"Read" buttons share this page: give each its own ID
    if (ImT::Button(Tr("Set"))) SpmMapSweep::SetGsw(0, story);
    ImGui::PopID();
    if (ImT::Button("-1")) SpmMapSweep::SetGsw(0, SpmMapSweep::Gsw0() - 1);
    ImGui::SameLine();
    if (ImT::Button("+1")) SpmMapSweep::SetGsw(0, SpmMapSweep::Gsw0() + 1);
    ImGui::Separator();
    static int index = 1, value = 0;
    ImGui::SetNextItemWidth(120.0f);
    ImT::InputInt(Tr("GSW index"), &index);
    ImGui::SetNextItemWidth(120.0f);
    ImT::InputInt(Tr("Value (0-255)"), &value);
    if (ImT::Button(Tr("Set GSW"))) SpmMapSweep::SetGsw(index, value);
}

void DrawFlagsMenu() {
    static int flag = 0;
    ImGui::SetNextItemWidth(140.0f);
    ImT::InputInt(Tr("Flag (GSWF)"), &flag);
    ImGui::PushID("flag");
    if (ImT::Button(Tr("Set"))) SpmMapSweep::SetSystemFlag(flag, true);
    ImGui::PopID();
    ImGui::SameLine();
    if (ImT::Button(Tr("Clear"))) SpmMapSweep::SetSystemFlag(flag, false);
    ImGui::SameLine();
    ImGui::PushID("flag");
    if (ImT::Button(Tr("Read"))) SpmMapSweep::ReadSystemFlag(flag);
    ImGui::PopID();
    ImT::TextDisabled(Tr("Last read: %d"), SpmMapSweep::LastResult());
}

bool g_hudMap = false, g_hudPos = false, g_hudInputs = false;

const char* DebugItemLabel(int id) {
    for (const auto& item : kDebugItems) if (item.id == id) return DebugName(item);
    return "?";
}

void DrawDebugHud() {
    if (!g_hudMap && !g_hudPos && !g_hudInputs) return;
    SpmMapSweep::RequestState();
    SpmMapSweep::DebugState st;
    if (!SpmMapSweep::GetState(st)) return;
    ImGui::SetNextWindowPos(ImVec2(10.0f, g_topBarVisible ? ImGui::GetFrameHeight() + 10.0f : 10.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Debug HUD", nullptr, kFlags)) {
        if (g_hudMap) ImT::Text(Tr("Map %s"), st.map);
        if (g_hudPos) ImT::Text("X %.2f  Y %.2f  Z %.2f", st.pos[0], st.pos[1], st.pos[2]);
        if (g_hudInputs) {
            static const struct { uint16_t bit; const char* name; } kButtons[] = {
                {0x0800, "A"}, {0x0400, "B"}, {0x0200, "1"}, {0x0100, "2"}, {0x1000, "-"}, {0x0010, "+"},
                {0x0008, "Up"}, {0x0004, "Down"}, {0x0001, "Left"}, {0x0002, "Right"}};
            std::string line = "Inputs:";
            for (const auto& b : kButtons) if (st.held & b.bit) line += std::string(" ") + b.name;
            ImT::TextUnformatted(line.c_str());
        }
    }
    ImGui::End();
}

void DrawHudMenu() {
    ImT::Checkbox(Tr("Map name"), &g_hudMap);
    ImT::Checkbox(Tr("Position (X Y Z)"), &g_hudPos);
    ImT::Checkbox(Tr("Controller inputs"), &g_hudInputs);
}

// The pouch snapshot holds the guest's big-endian bytes.
int PouchS32(const SpmMapSweep::DebugState& st, int offset) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(st.pouch) + offset;
    return int(int32_t(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]));
}
int PouchS16(const SpmMapSweep::DebugState& st, int offset) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(st.pouch) + offset;
    return int(int16_t(uint16_t(p[0]) << 8 | p[1]));
}

void DrawStatsMenu() {
    static const struct { const char* name; int offset; } kStats[] = {
        {"Level", 0x04}, {"Attack", 0x08}, {"HP", 0x0C}, {"Max HP", 0x10}, {"XP", 0x18}, {"Coins", 0x1C}, {"Arcade tokens", 0x688}};
    static int values[7] = {};
    SpmMapSweep::RequestState();
    SpmMapSweep::DebugState st;
    const bool have = SpmMapSweep::GetState(st);
    if (ImT::Button(Tr("Read current values")) && have)
        for (int i = 0; i < 7; ++i) values[i] = PouchS32(st, kStats[i].offset);
    for (int i = 0; i < 7; ++i) {
        ImGui::PushID(i);
        int now = 0;
        if (have) now = PouchS32(st, kStats[i].offset);
        ImGui::SetNextItemWidth(120.0f);
        ImT::InputInt(Tr(kStats[i].name), &values[i], 1, 10);
        ImGui::SameLine();
        if (ImT::Button(Tr("Set"))) SpmMapSweep::SetPouch(kStats[i].offset, values[i]);
        ImGui::SameLine();
        ImT::TextDisabled(Tr("now %d"), now);
        ImGui::PopID();
    }
}

void DrawInventoryMenu() {
    SpmMapSweep::RequestState();
    SpmMapSweep::DebugState st;
    if (!SpmMapSweep::GetState(st)) { ImT::TextDisabled("%s", Tr("Not in a map.")); return; }
    auto s16 = [&](int offset) { return PouchS16(st, offset); };
    ImT::TextDisabled("%s", Tr("Click an entry to remove it. Add items from the Items menu."));
    if (ImGui::CollapsingHeader(Tr("Key items"), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < 32; ++i) {
            const int id = s16(0x20 + 2 * i);
            if (!id) continue;
            char label[96]; std::snprintf(label, sizeof label, "%d  %s##k%d", id, DebugItemLabel(id), i);
            if (ImT::MenuItem(label)) SpmMapSweep::RemoveItem(id);
        }
    }
    if (ImGui::CollapsingHeader(Tr("Usable items"), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < 10; ++i) {
            const int id = s16(0x60 + 2 * i);
            if (!id) continue;
            char label[96]; std::snprintf(label, sizeof label, "%d  %s##u%d", id, DebugItemLabel(id), i);
            if (ImT::MenuItem(label)) SpmMapSweep::RemoveItem(id);
        }
    }
    if (ImGui::CollapsingHeader(Tr("Characters and Pixls"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImT::TextDisabled("%s", Tr("The character you play can't be removed."));
        bool tippi = st.tippi;
        if (ImT::Checkbox(DebugItemLabel(220), &tippi)) SpmMapSweep::SetTippi(tippi);
        if (ImT::Button(Tr("Remove all Pixls"))) {   // Tippi stays; the Pixl in use goes last
            int inUse = 0;
            for (int i = 0; i < 16; ++i) {
                const int id = PouchS16(st, 0xC4 + 4 * i + 2);
                if (id == 0 || id == 220) continue;
                if (id == st.curPixl) inUse = id;
                else SpmMapSweep::RemoveCharOrPixl(id);
            }
            if (inUse) SpmMapSweep::RemoveCharOrPixl(inUse);
        }
        for (int i = 0; i < 20; ++i) {
            const int offset = 0xB4 + 4 * i;
            const int id = s16(offset + 2);
            if (!id) continue;
            char label[96];
            const bool inUse = id == st.curChar || id == st.curPixl;
            std::snprintf(label, sizeof label, "%d  %s%s%s##c%d", id, DebugItemLabel(id), st.pouch[offset] ? Tr("  [selectable]") : "",
                          inUse ? Tr("  [in use]") : "", i);
            if (ImT::MenuItem(label) && id != st.curChar) SpmMapSweep::RemoveCharOrPixl(id);
        }
    }
}

void DrawVariablesMenu() {
    static int type = 0, index = 0, value = 0;
    ImGui::SetNextItemWidth(140.0f);
    ImT::Combo(Tr("Type"), &type, "GSW\0GSWF\0LSW\0LSWF\0");
    ImGui::SetNextItemWidth(140.0f);
    ImT::InputInt(Tr("Number"), &index);
    ImGui::SetNextItemWidth(140.0f);
    ImT::InputInt(Tr("Value"), &value);
    ImGui::PushID("var");
    if (ImT::Button(Tr("Read"))) SpmMapSweep::ReadVar(type, index);
    ImGui::PopID();
    ImGui::SameLine();
    ImGui::PushID("var");
    if (ImT::Button(Tr("Set"))) SpmMapSweep::SetVar(type, index, value);
    ImGui::PopID();
    ImT::TextDisabled(Tr("Last read: %d"), SpmMapSweep::LastResult());
}

const char* PitRoom(bool flopside, int floor) {
    static char room[16];
    int n;
    if (floor == 100) n = flopside ? 70 : 30;
    else if (floor % 10 == 0) n = (flopside ? 60 : 20) + (floor <= 20 ? 1 : floor <= 40 ? 2 : floor <= 60 ? 3 : 4);
    else n = (flopside ? 40 : 0) + (floor <= 25 ? 1 : floor <= 49 ? 2 : floor <= 75 ? 3 : 4);
    std::snprintf(room, sizeof room, "dan_%02d", n);
    return room;
}

void DrawPitMenu() {
    static int pit = 0, floor = 1;
    ImGui::SetNextItemWidth(140.0f);
    ImT::Combo(Tr("Pit"), &pit, "Flipside\0Flopside\0");
    ImGui::SetNextItemWidth(140.0f);
    ImT::InputInt(Tr("Floor"), &floor);
    floor = std::clamp(floor, 1, 100);
    ImT::TextDisabled(Tr("Room %s (experimental)"), PitRoom(pit == 1, floor));
    if (ImT::Button(Tr("Warp"))) {
        SpmMapSweep::SetGsw(1, (floor - 1) + (pit == 1 ? 100 : 0));
        SpmMapSweep::RequestWarp(PitRoom(pit == 1, floor), nullptr);
        SetTopBarVisible(false);
    }
}

void DrawDebugMenu() {
    ImGui::SetNextItemWidth(150.0f);
    ImT::Combo("Language / Langue", &g_debugLanguage_ui, "Auto (game)\0English\0Deutsch\0Fran\xc3\xa7" "ais\0Espa\xc3\xb1ol\0Italiano\0");
    g_debugLanguage = g_debugLanguage_ui - 1;
    ImGui::Separator();
    if (ImT::BeginMenu(Tr("Warp / TP"))) { DrawWarpMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("Story / Variables"))) { DrawStoryMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("Items"))) { DrawItemsMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("Inventory"))) { DrawInventoryMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("Pit of 100 Trials"))) { DrawPitMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("Script variables"))) { DrawVariablesMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("HUD displays"))) { DrawHudMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("Stats"))) { DrawStatsMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("Characters / Pixls"))) { DrawPartyMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("Player"))) { DrawPlayerMenu(); ImGui::EndMenu(); }
    if (ImT::BeginMenu(Tr("Flags"))) { DrawFlagsMenu(); ImGui::EndMenu(); }
}


// ------------------------------------------------------------------------------------------------------------
// F10 window: tabs along the top, pages on the left, the page's content on the right.
float g_menuScale = 1.5f;
bool g_menuScaleLoaded = false;

void LoadMenuScale() {
    g_menuScaleLoaded = true;
    std::ifstream in("menu_scale.txt");
    float v = 0.0f;
    int lang = 0;
    if (in >> v && v >= 0.8f && v <= 2.5f) g_menuScale = v;
    if (in >> lang && lang >= 0 && lang <= 5) { g_debugLanguage_ui = lang; g_debugLanguage = lang - 1; }
}

void SaveMenuScale() {
    std::ofstream out("menu_scale.txt", std::ios::trunc);
    out << g_menuScale << "\n" << g_debugLanguage_ui << "\n";
}

void DrawGeneralPage() {
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::Combo(Tr("Menu language"), &g_debugLanguage_ui, "Auto (game)\0English\0Deutsch\0Fran\xc3\xa7" "ais\0Espa\xc3\xb1ol\0Italiano\0")) {
        g_debugLanguage = g_debugLanguage_ui - 1;
        SaveMenuScale();
    }
    ImGui::SetNextItemWidth(240.0f);
    ImT::SliderFloat(Tr("Menu size"), &g_menuScale, 0.8f, 2.5f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveMenuScale();
    ImGui::Separator();
    const auto resolutionIt = std::find_if(kResolutions.begin(), kResolutions.end(), [](const ResolutionItem& item) {
        return std::fabs(item.scale - g_resolutionScale) < 0.001f;
    });
    ImGui::SetNextItemWidth(240.0f);
    if (ImT::BeginCombo(Tr("Resolution"), resolutionIt != kResolutions.end() ? resolutionIt->label : "Custom")) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - g_resolutionScale) < 0.001f;
            ImGui::BeginDisabled(IsHighFrameRateMode() && IsHighResolutionScale(resolution.scale));
            if (ImT::Selectable(resolution.label, selected)) SetResolutionScale(resolution.scale);
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    uint32_t pictureWidth = 0, pictureHeight = 0;
    AuroraGetPictureSize(&pictureWidth, &pictureHeight);
    ImT::TextDisabled("%ux%u", pictureWidth, pictureHeight);
    const DisplaySettings::Settings display = DisplaySettings::Current();
    const auto aspectIt = std::find_if(kAspects.begin(), kAspects.end(), [&](const AspectItem& item) {
        return item.mode == display.aspect;
    });
    ImGui::SetNextItemWidth(240.0f);
    if (ImT::BeginCombo(Tr("Aspect"), aspectIt != kAspects.end() ? aspectIt->label : "Custom")) {
        for (const auto& aspect : kAspects)
            if (ImT::Selectable(aspect.label, aspect.mode == display.aspect)) SetAspectMode(aspect.mode);
        ImGui::EndCombo();
    }
    if ((display.aspect != DisplaySettings::kAspect4x3) != DisplaySettings::GameWidescreen())
        ImT::TextDisabled("Switching between 4:3 and widescreen takes effect after a restart.");
}

void DrawAudioPage() { DrawAudioSettings(); DrawRebindPrompt(); }
void DrawGraphicsPage() { DrawGraphicsSettings(); }
void DrawControlsPage() {
    bool mouse = RuntimeConfigFile::MousePointer(true);
    if (ImT::Checkbox(Tr("Mouse acts as the Wii pointer"), &mouse)) RuntimeConfigFile::SetMousePointer(mouse);
    ImT::TextDisabled("%s", Tr("Moving the mouse over the game aims Tippi's pointer, and a click presses A."));
    ImGui::Separator();
    DrawControllerSettings();
    DrawRebindPrompt();
}
void DrawLanPage() { DrawLanPlaySettings(); }

void DrawGameplayPage() {
    bool coins = SpinningCoins();
    if (ImT::Checkbox(Tr("Spinning coins and animated tiles"), &coins)) SetSpinningCoins(coins);
}


void DrawWatchPage() {
    bool on = SpmMapSweep::WatchEnabled();
    if (ImT::Checkbox(Tr("Watch flags"), &on)) SpmMapSweep::SetWatch(on);
    ImGui::SameLine();
    if (ImT::Button(Tr("Clear log"))) SpmMapSweep::ClearWatchLog();
    ImT::TextDisabled("%s", Tr("Changes appear here: pick up an item or open a chest to see which flag it sets."));
    ImGui::Separator();
    if (ImGui::BeginChild("##watchlog", ImVec2(0.0f, 0.0f), true)) {
        const auto log = SpmMapSweep::GetWatchLog();
        for (size_t i = log.size(); i > 0; --i) ImT::TextUnformatted(log[i - 1].c_str());
    }
    ImGui::EndChild();
}

void DrawQuickPage() {
    if (ImT::Button(Tr("Heal"))) SpmMapSweep::Heal();
    ImGui::SameLine();
    if (ImT::Button(Tr("Set 99 coins"))) SpmMapSweep::SetPouch(0x1C, 99);
    if (ImT::Button(Tr("Unlock all characters and Pixls")))
        for (int id = 217; id <= 231; ++id) SpmMapSweep::Unlock(id, id >= 220);
    if (ImT::Button(Tr("Save position"))) SpmMapSweep::SavePosition();
    ImGui::SameLine();
    ImGui::BeginDisabled(!SpmMapSweep::HasSavedPosition());
    if (ImT::Button(Tr("Load position"))) SpmMapSweep::LoadPosition();
    ImGui::EndDisabled();
    {
        SpmMapSweep::RequestState();
        SpmMapSweep::DebugState st;
        if (SpmMapSweep::GetState(st)) ImT::TextDisabled(Tr("Position: %.0f  %.0f  %.0f"), st.pos[0], st.pos[1], st.pos[2]);
        static float target[3] = {};
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputFloat3("##teleport", target, "%.0f");
        ImGui::SameLine();
        if (ImT::Button(Tr("Teleport"))) SpmMapSweep::TeleportTo(target[0], target[1], target[2]);
        ImT::TextUnformatted(Tr("Gravity:"));
        static const char* const kGravity[] = {"Down", "Up (upside down)", "Left", "Right"};
        for (int dir = 0; dir < 4; ++dir) {
            ImGui::SameLine();
            if (ImT::Button(Tr(kGravity[dir]))) SpmMapSweep::SetGravity(dir);
        }
    }
    ImGui::Separator();
    DrawPlayerMenu();
    ImGui::Separator();
    DrawStatsMenu();
}

void DrawDisplayPage() {
    static int gameLang = -1;
    if (gameLang < 0) {
        switch (RuntimeConfigFile::Language()) {
        case 2: gameLang = 1; break;
        case 3: gameLang = 2; break;
        case 4: gameLang = 3; break;
        case 5: gameLang = 4; break;
        default: gameLang = 0; break;
        }
    }
    static constexpr const char* kCodes[] = {"en", "de", "fr", "es", "it"};
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::Combo(Tr("Game language"), &gameLang, "English\0Deutsch\0Fran\xc3\xa7" "ais\0Espa\xc3\xb1ol\0Italiano\0"))
        RuntimeConfigFile::SetLanguage(kCodes[gameLang]);
    ImGui::TextDisabled("%s", Tr("Takes effect after restarting the game."));
    ImGui::Separator();
    DrawHudMenu();
}


void DrawFlagListPage() {
    SpmMapSweep::RequestFlagSnapshot();
    DrawFlagsMenu();
}

void DrawStoryPage() { DrawStoryMenu(); ImGui::Separator(); DrawVariablesMenu(); }

enum MenuTab { kTabSettings, kTabEnhancements, kTabNetwork, kTabDev, kTabCount };
constexpr const char* kTabNames[kTabCount] = {"Settings", "Enhancements", "Network", "Dev Tools"};
struct MenuPage { int tab; const char* name; void (*draw)(); };
const MenuPage kPages[] = {
    {kTabSettings, "General", DrawGeneralPage},
    {kTabSettings, "Audio", DrawAudioPage},
    {kTabSettings, "Graphics", DrawGraphicsPage},
    {kTabSettings, "Controls", DrawControlsPage},
    {kTabEnhancements, "Gameplay", DrawGameplayPage},
    {kTabNetwork, "LAN Play", DrawLanPage},
    {kTabDev, "Warp", DrawWarpMenu},
    {kTabDev, "Player", DrawQuickPage},
    {kTabDev, "Items", DrawItemsMenu},
    {kTabDev, "Inventory", DrawInventoryMenu},
    {kTabDev, "Characters / Pixls", DrawPartyMenu},
    {kTabDev, "Story / Variables", DrawStoryPage},
    {kTabDev, "Flags", DrawFlagListPage},
    {kTabDev, "Flag watch", DrawWatchPage},
    {kTabDev, "Pit of 100 Trials", DrawPitMenu},
    {kTabDev, "Display / Language", DrawDisplayPage},
};
constexpr int kPageCount = int(sizeof kPages / sizeof kPages[0]);


void DrawMainWindow() {
    if (!g_menuScaleLoaded) LoadMenuScale();
    if (!g_topBarVisible) { ImGui::GetIO().FontGlobalScale = 1.0f; return; }
    ImGui::GetIO().FontGlobalScale = g_menuScale;
    static int tab = kTabSettings;
    static int page = 0;
    static char search[48] = "";

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y), IM_COL32(0, 0, 0, 140));
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.88f, viewport->Size.y * 0.88f), ImGuiCond_Always);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("##nspbmenu", nullptr, kFlags)) { ImGui::End(); return; }

    for (int t = 0; t < kTabCount; ++t) {
        if (t) ImGui::SameLine();
        if (ImT::Selectable(Tr(kTabNames[t]), tab == t && !search[0], 0,
                              ImVec2(ImGui::CalcTextSize(Tr(kTabNames[t])).x + 24.0f, 0.0f))) {
            tab = t;
            search[0] = 0;
            for (int i = 0; i < kPageCount; ++i) if (kPages[i].tab == t) { page = i; break; }
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImT::InputTextWithHint("##search", Tr("Search..."), search, sizeof search);
    const float titleWidth = ImGui::CalcTextSize(Tr("Title screen")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float closeWidth = ImGui::CalcTextSize(Tr("Close (F10)")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float exitWidth = ImGui::CalcTextSize(Tr("Exit")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - titleWidth - closeWidth - exitWidth - ImGui::GetStyle().ItemSpacing.x * 3.0f - 8.0f);
    if (ImT::Button(Tr("Title screen"))) {   // back to the title screen (from a map), menu closed
        SpmMapSweep::ReturnToTitle();
        SetTopBarVisible(false);
    }
    ImGui::SameLine();
    if (ImT::Button(Tr("Close (F10)"))) SetTopBarVisible(false);
    ImGui::SameLine();
    if (ImT::Button(Tr("Exit"))) g_exitPromptOpen = true;
    ImGui::Separator();

    if (ImGui::BeginChild("##pages", ImVec2(210.0f, 0.0f), true)) {
        for (int i = 0; i < kPageCount; ++i) {
            const bool shown = search[0] ? ContainsNoCase(Tr(kPages[i].name), search) || ContainsNoCase(kPages[i].name, search)
                                         : kPages[i].tab == tab;
            if (!shown) continue;
            if (ImT::Selectable(Tr(kPages[i].name), page == i)) { page = i; tab = kPages[i].tab; }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f), true)) kPages[page].draw();
    ImGui::EndChild();
    ImGui::End();
}

[[maybe_unused]] void DrawTopBar() {
    if (!g_topBarVisible) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 70));
    constexpr float kHintMargin = 10.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                 viewport->Pos.y + ImGui::GetFrameHeight() + kHintMargin),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("Settings input hint", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {
        for (const char* line : {"Settings open - game controls disabled.",
                                 "Press F10 to return to the game."}) {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(line).x) * 0.5f);
            ImT::TextUnformatted(line);
        }
    }
    ImGui::End();
    if (!ImGui::BeginMainMenuBar()) return;

    ImT::TextUnformatted("New Super Paper Bros.");
    ImGui::Separator();
    const auto resolutionIt = std::find_if(kResolutions.begin(), kResolutions.end(), [](const ResolutionItem& item) {
        return std::fabs(item.scale - g_resolutionScale) < 0.001f;
    });
    const char* resolutionLabel = resolutionIt != kResolutions.end() ? resolutionIt->label : "Custom";
    const std::string resolutionMenuLabel = std::string("Resolution: ") + resolutionLabel;
    if (ImT::BeginMenu(resolutionMenuLabel.c_str())) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - g_resolutionScale) < 0.001f;
            const bool disabled = IsHighFrameRateMode() && IsHighResolutionScale(resolution.scale);
            ImGui::BeginDisabled(disabled);
            const bool clicked = ImT::MenuItem(resolution.label, nullptr, selected);
            ImGui::EndDisabled();
            if (clicked) {
                SetResolutionScale(resolution.scale);
            }
        }
        uint32_t pictureWidth = 0;
        uint32_t pictureHeight = 0;
        AuroraGetPictureSize(&pictureWidth, &pictureHeight);
        ImGui::Separator();
        ImT::TextDisabled("Rendering %ux%u", pictureWidth, pictureHeight);
        ImGui::EndMenu();
    }

    const DisplaySettings::Settings display = DisplaySettings::Current();
    const auto aspectIt = std::find_if(kAspects.begin(), kAspects.end(), [&](const AspectItem& item) {
        return item.mode == display.aspect;
    });
    const std::string aspectMenuLabel =
        std::string("Aspect: ") + (aspectIt != kAspects.end() ? aspectIt->label : "Custom");
    if (ImT::BeginMenu(aspectMenuLabel.c_str())) {
        for (const auto& aspect : kAspects) {
            if (ImT::MenuItem(aspect.label, nullptr, aspect.mode == display.aspect)) {
                SetAspectMode(aspect.mode);
            }
        }
        // The game reads 4:3 versus widescreen once, at boot.
        if ((display.aspect != DisplaySettings::kAspect4x3) != DisplaySettings::GameWidescreen()) {
            ImGui::Separator();
            ImT::TextDisabled("Switching between 4:3 and widescreen takes effect after a restart.");
        }
        ImGui::EndMenu();
    }

    if (ImT::BeginMenu("Graphics")) {
        DrawGraphicsSettings();
        ImGui::EndMenu();
    }

    if (ImT::BeginMenu("Controller settings")) {
        DrawControllerSettings();
        // Nest capture under this menu so opening/closing the modal preserves
        // the settings popup and its current port and scroll position.
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    if (ImT::BeginMenu("LAN Play")) {
        DrawLanPlaySettings();
        ImGui::EndMenu();
    }

    if (ImT::BeginMenu("Debug")) {
        DrawDebugMenu();
        ImGui::EndMenu();
    }

    const std::string audioLabel = g_audioMuted
        ? "Audio: Muted"
        : "Audio: " + std::to_string(g_audioVolumePercent) + "%";
    // Keep the popup ID stable while the Master slider changes the visible
    // label. Without the ### suffix, ImGui treats every new percentage as a
    // different menu and closes the popup on the first drag update.
    const std::string audioMenuLabel = audioLabel + "###AudioSettingsMenu";
    if (ImT::BeginMenu(audioMenuLabel.c_str())) {
        DrawAudioSettings();
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float hideWidth = ImGui::CalcTextSize("Hide (F10)").x + style.FramePadding.x * 2.0f;
    const float exitWidth = ImGui::CalcTextSize("X").x + style.FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                  ImGui::GetWindowWidth() - hideWidth - exitWidth - style.ItemSpacing.x - 8.0f));
    if (ImT::MenuItem("Hide (F10)")) {
        SetTopBarVisible(false);
    }
    if (ImT::MenuItem("X")) {
        g_exitPromptOpen = true;
    }
    ImGui::EndMainMenuBar();
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
void UpdateCursorAutoHide() {
    const bool shouldHide =
        !g_topBarVisible && Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    // ImGui_ImplSDL3_NewFrame calls SDL_ShowCursor every frame unless this flag is set.
    if (shouldHide) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_HideCursor();
    } else {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    }
}

// Alt+Enter toggles the display mode inside aurora without going through the
// F10 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}

void ApplyInputBlockState() {
    const bool blocked = controller_mapping_wizard::IsActive() || g_rebind.active ||
                         g_exitPromptOpen || g_topBarVisible || StartupScreenVisible();
    PADBlockInput(blocked);
    InputBindings::SetInputBlocked(blocked);
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
    InputBindings::Reload();
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(static_cast<float>(g_musicVolumePercent) / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(static_cast<float>(g_soundEffectsVolumePercent) / 100.0f);
    MusicAttenuation::SetUiVolume(static_cast<float>(g_uiVolumePercent) / 100.0f);
    MusicAttenuation::SetVoicesVolume(static_cast<float>(g_voicesVolumePercent) / 100.0f);
    MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
#if defined(__APPLE__)
    aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
#endif
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(targetFps);
    AuroraSetTexturePatches(RuntimeConfigFile::SpinningCoins(true));
    aurora_set_vsync(g_vsync.load());
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    g_bootShadersReady.store(false, std::memory_order_relaxed);
    g_bootShaderNotice = false;
    g_bootShaderWaitStart = {};
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
}

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_BACKSPACE) ||
                                IsToggleKey(ev->sdl, SDL_SCANCODE_DELETE))) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        }
        // NSMBW: F11 toggles between a window and borderless fullscreen (saved like the F10 bar's
        // Display mode).
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F11)) {
            const AuroraDisplayMode mode = aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED
                                               ? AURORA_DISPLAY_MODE_BORDERLESS
                                               : AURORA_DISPLAY_MODE_WINDOWED;
            aurora_set_display_mode(mode);
            g_displayMode = static_cast<int>(aurora_get_display_mode());
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F10)) {
            SetTopBarVisible(!g_topBarVisible);
            ApplyInputBlockState();
        }
        if (!g_rebind.active && g_muteHotkey != PAD_KEY_INVALID &&
            IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        // NSMBW: Escape is the game's + (its pause menus, Keybinds); here it only closes the F10 bar
        // or the quit prompt.
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE)) {
            if (g_exitPromptOpen) {
                g_exitPromptOpen = false;
            } else if (g_topBarVisible) {
                SetTopBarVisible(false);
            }
            ApplyInputBlockState();
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

bool g_spinningCoins = RuntimeConfigFile::SpinningCoins(true);

void SetSpinningCoins(bool enabled) noexcept {
    g_spinningCoins = enabled;
    AuroraSetTexturePatches(enabled);
    RuntimeConfigFile::SetSpinningCoins(enabled);
}

bool SpinningCoins() noexcept {
    return g_spinningCoins;
}

void SetShowFps(bool show) noexcept {
    g_showFps = show;
    RuntimeConfigFile::SetShowFps(show);
}

bool ShowFps() noexcept {
    return g_showFps;
}

void SetVSync(bool enabled) noexcept {
    g_vsync.store(enabled);
    aurora_set_vsync(enabled);
    RuntimeConfigFile::SetVSync(enabled);
}

bool VSync() noexcept {
    return g_vsync.load(std::memory_order_relaxed);
}

void SetFrameGraph(bool show) noexcept {
    g_frameGraph = show;
    RuntimeConfigFile::SetFrameGraph(show);
}

bool FrameGraph() noexcept {
    return g_frameGraph;
}

void SetRenderScale(float scale) noexcept {
    SetResolutionScale(scale);
}

float RenderScale() noexcept {
    return g_resolutionScale;
}

void SetAspect(int32_t mode) noexcept {
    SetAspectMode(mode);
}

int32_t Aspect() noexcept {
    return DisplaySettings::Latest().aspect;
}

void ReleaseControllers() noexcept {
    // Aurora drives the LED white on first PADRead and never clears it, and the
    // exit paths terminate the process outright, so do it here.
    bool queued = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) continue;
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
            SDL_SetGamepadLED(pad, 0, 0, 0);
            queued = true;
        }
    }
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
    PADControlAllMotors(stopAll.data());
    // SDL hands LED and rumble reports to its own HIDAPI sender thread rather
    // than writing them here, so without this the process dies before the
    // controller ever receives them.
    if (queued) SDL_Delay(120);
}

// New Super Paper Bros.: the game's Controls screen (- button) shows a 3D Wii Remote with bubbles pointing at its
// buttons. With a controller in use, this draws a controls panel over it instead: a picture of that controller with a
// callout for each action the game's screen is showing, in the game's own font. The remote's textures are hidden and
// Aurora reports which of that screen's parts are drawn (UserData/button_icons/<style>/marker<N>,
// tools/make-button-icons.py): marker 1 = the remote (the screen is up), 2-6 = the Move, Flip, Jump, Use a Pixl and
// Use Tippi bubbles.
void DrawControlsPanel() {
    const int style = aurora_button_icon_style();
    static double shownSince = -1.0;
    static bool seen[8] = {};  // actions shown since the screen opened (bubbles slide in one after another)
    // The screen is up while the remote's body (marker 1) and its Move bubble (marker 2, on every version of it) are
    // drawn: either alone also appears elsewhere, and a panel opened there could not be closed (issue #7).
    const bool screenUp = aurora_button_icon_marker(1) && (seen[2] || aurora_button_icon_marker(2));
    // A new press of - or 1 hides the panel until the screen goes: both close the game's screen, and if the panel
    // ever opens over something else (issue #7, a Switch Pro report) the player can still get rid of it.
    static bool dismissed = false;
    static uint32_t prevButtons = 0;
    const uint32_t buttons = VirtualRemote::LastButtons();
    constexpr uint32_t kCloseButtons = 0x1000 | 0x0200;  // WPAD -, 1
    if (screenUp && shownSince >= 0.0 && ImGui::GetTime() - shownSince > 0.3 && (buttons & ~prevButtons & kCloseButtons) != 0)
        dismissed = true;
    prevButtons = buttons;
    if (!screenUp) dismissed = false;
    if (style < 0 || style > 2 || !screenUp || dismissed) {
        if (shownSince >= 0.0)  // closed: what it showed (diagnoses a missing callout)
            std::fprintf(stderr, "[controls panel] style %d showed move %d flip %d jump %d pixl %d tippi %d\n", style, seen[2],
                         seen[3], seen[4], seen[5], seen[6]);
        shownSince = -1.0;
        std::fill(std::begin(seen), std::end(seen), false);
        return;
    }
    if (shownSince < 0.0) shownSince = ImGui::GetTime();
    for (int m = 2; m <= 6; ++m) seen[m] = seen[m] || aurora_button_icon_marker(m);
    seen[7] = true;  // Stylish moves (shake): not on the game's screen, always worth showing
    const float alpha = std::min(1.0f, static_cast<float>(ImGui::GetTime() - shownSince) / 0.2f);  // fade in

    struct Sprite { ImTextureID id{}; float aspect = 1.0f; };
    struct Style {
        bool loaded = false;
        Sprite pad, icons[8];
        ImVec2 spots[8];  // panel.txt: where each action's button is on the picture (fractions)
    };
    static Style styles[3];
    static const char* const kActions[8] = {"", "", "move", "flip", "jump", "pixl", "tippi", "stylish"};
    Style& st = styles[style];
    if (!st.loaded) {
        st.loaded = true;
        static const char* const kNames[3] = {"xbox", "playstation", "nintendo"};
        const auto dir = RuntimeConfigFile::ApplicationDataDirectory() / "button_icons" / kNames[style];
        const auto load = [](const std::filesystem::path& path) {
            Sprite sp;
            uint32_t w = 0, h = 0;
            if (uint8_t* rgba = aurora_load_png_rgba(RuntimeConfigFile::PathToUtf8(path).c_str(), &w, &h)) {
                sp.id = aurora_imgui_add_texture(w, h, rgba);
                sp.aspect = static_cast<float>(w) / static_cast<float>(h);
                std::free(rgba);
            }
            return sp;
        };
        st.pad = load(dir / "controller.png");
        for (int m = 2; m <= 7; ++m) st.icons[m] = load(dir / "ui" / (std::string(kActions[m]) + ".png"));
        std::ifstream panel(dir / "panel.txt");
        std::string name;
        float fx = 0.0f, fy = 0.0f;
        while (panel >> name >> fx >> fy)
            for (int m = 2; m <= 7; ++m)
                if (name == kActions[m]) st.spots[m] = ImVec2(fx, fy);
    }
    if (!st.pad.id) return;

    static const std::shared_ptr<GameLayout::Font> font = GameLayout::LoadFont("papermarioset_US.bfn");
    const bool gameFont = GameLayout::FontReady(font.get());
    const GameLayout::View view = GameLayout::GameView();
    // Layout units: the game's 832 x 456 canvas, origin at the centre, y up.
    const auto at = [&](float x, float y) { return ImVec2(view.centerX + x * view.gameScaleX, view.centerY - y * view.scale); };
    const auto a = [&](int v) { return static_cast<int>(v * alpha); };
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const auto text = [&](const char* s, float x, float y, float size, uint32_t rgba, int originH) {
        if (gameFont) {
            const uint32_t c = (rgba & 0xFFFFFF00u) | static_cast<uint32_t>(a(rgba & 0xFF));
            GameLayout::DrawText(list, view, *font, GameLayout::Utf16(s), x, y, size, c, c, 1.0f, originH, 1);
        } else {
            const ImVec2 p = at(x, y);
            const float w = ImGui::CalcTextSize(s).x;
            list->AddText(ImVec2(p.x - (originH == 1 ? w / 2 : originH == 2 ? w : 0.0f), p.y - ImGui::GetTextLineHeight() / 2),
                          IM_COL32(rgba >> 24, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, a(rgba & 0xFF)), s);
        }
    };
    const auto textWidth = [&](const char* s, float size) {
        return gameFont ? GameLayout::MeasureText(*font, GameLayout::Utf16(s), size) : ImGui::CalcTextSize(s).x / view.scale;
    };

    // The card, over the whole of the game's screen (its bubbles and remote), with a header.
    const ImVec2 c0 = at(-392.0f, 205.0f), c1 = at(392.0f, -218.0f);
    list->AddRectFilled(c0, c1, IM_COL32(16, 18, 26, a(255)), 22.0f * view.scale);
    list->AddRect(c0, c1, IM_COL32(255, 255, 255, a(46)), 22.0f * view.scale, 0, 2.0f * view.scale);
    static const char* const kPadNames[3] = {"Xbox controller", "PlayStation controller", "Nintendo Switch controller"};
    text("Controls", -360.0f, 176.0f, 26.0f, 0xFFFFFFFFu, 0);
    text(kPadNames[style], 360.0f, 176.0f, 15.0f, 0xA8AEBDFFu, 2);
    list->AddLine(at(-360.0f, 152.0f), at(360.0f, 152.0f), IM_COL32(255, 255, 255, a(30)), 1.5f * view.scale);
    // How to close it (the game's - or 1, at their default places on each controller).
    static const char* const kCloseHint[3] = {"View or LB: close", "Create or L1: close", "- or L: close"};
    text(kCloseHint[style], 360.0f, -196.0f, 15.0f, 0xA8AEBDFFu, 2);

    // The controller.
    constexpr float kPadW = 300.0f, kPadCX = -28.0f, kPadCY = -18.0f;
    const float padH = kPadW / st.pad.aspect;
    const ImVec2 p0 = at(kPadCX - kPadW / 2, kPadCY + padH / 2), p1 = at(kPadCX + kPadW / 2, kPadCY - padH / 2);
    list->AddImage(st.pad.id, p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a(255)));

    // Callouts: Move on the left, the face buttons and the right stick on the right, each in the order of its button
    // from top to bottom, with a leader line to the button.
    struct Callout { int m; const char* title; const char* sub; };
    static const Callout kLeft[] = {{2, "Move", "Speak, interact"}};
    static const char* const kShakeButtons[3] = {"RB or RT: stylish moves, break free", "R1 or R2: stylish moves, break free",
                                                 "R or ZR: stylish moves, break free"};
    const Callout kRight[] = {{7, "Shake", kShakeButtons[style]}, {3, "Flip", ""}, {5, "Use a Pixl", "Cancel"},
                              {4, "Jump", "Confirm"}, {6, "Use Tippi", ""}};
    const auto callouts = [&](const Callout* items, size_t count, bool right) {
        std::vector<const Callout*> shown;
        for (size_t i = 0; i < count; ++i)
            if (seen[items[i].m]) shown.push_back(&items[i]);
        std::sort(shown.begin(), shown.end(), [&](const Callout* l, const Callout* r) { return st.spots[l->m].y < st.spots[r->m].y; });
        // Each label level with its button, pushed apart where they would touch, so the leaders run nearly level.
        std::vector<float> ys;
        for (const Callout* co : shown) ys.push_back(kPadCY + padH / 2 - st.spots[co->m].y * padH);
        constexpr float kSpacing = 50.0f, kTop = 120.0f, kBottom = -150.0f;
        for (size_t i = 0; i < ys.size(); ++i) ys[i] = std::min(ys[i], i == 0 ? kTop : ys[i - 1] - kSpacing);
        for (size_t i = ys.size(); i-- > 0;) ys[i] = std::max(ys[i], i + 1 == ys.size() ? kBottom : ys[i + 1] + kSpacing);
        for (size_t i = 0; i < shown.size(); ++i) {
            const Callout& co = *shown[i];
            const float y = ys[i];
            const float edge = right ? 360.0f : -360.0f, icon = 30.0f;
            // icon at the outer edge, text inside it
            const float ix = right ? edge - icon / 2 : edge + icon / 2;
            if (st.icons[co.m].id)
                list->AddImage(st.icons[co.m].id, at(ix - icon / 2, y + icon / 2), at(ix + icon / 2, y - icon / 2), ImVec2(0, 0),
                               ImVec2(1, 1), IM_COL32(255, 255, 255, a(255)));
            const float tx = right ? edge - icon - 8.0f : edge + icon + 8.0f;
            const bool two = co.sub[0] != '\0';
            text(co.title, tx, two ? y + 8.0f : y, 17.0f, 0xFFFFFFFFu, right ? 2 : 0);
            if (two) text(co.sub, tx, y - 11.0f, 13.0f, 0xA8AEBDFFu, right ? 2 : 0);
            // leader: from just past the text toward the button, ending on it with a dot
            const float tw = std::max(textWidth(co.title, 17.0f), two ? textWidth(co.sub, 13.0f) : 0.0f);
            const ImVec2 from = at(right ? tx - tw - 10.0f : tx + tw + 10.0f, y);
            const ImVec2 to(p0.x + st.spots[co.m].x * (p1.x - p0.x), p0.y + st.spots[co.m].y * (p1.y - p0.y));
            list->AddLine(from, to, IM_COL32(255, 255, 255, a(150)), 1.6f * view.scale);
            list->AddCircleFilled(to, 4.0f * view.scale, IM_COL32(255, 255, 255, a(235)));
            list->AddCircleFilled(from, 2.2f * view.scale, IM_COL32(255, 255, 255, a(150)));
        }
    };
    callouts(kLeft, std::size(kLeft), false);
    callouts(kRight, std::size(kRight), true);

    // Footer, centred: [1] + [2] Quick menu (the game's 1 + 2 combination).
    const float fy = -190.0f, icon = 24.0f, plus = 16.0f, gap = 10.0f;
    float x = -(icon * 2 + plus + gap + textWidth("Quick menu", 15.0f)) / 2;
    const auto footerIcon = [&](int m) {
        if (st.icons[m].id)
            list->AddImage(st.icons[m].id, at(x, fy + icon / 2), at(x + icon, fy - icon / 2), ImVec2(0, 0), ImVec2(1, 1),
                           IM_COL32(255, 255, 255, a(255)));
        x += icon;
    };
    footerIcon(5);
    text("+", x + plus / 2, fy, 15.0f, 0xA8AEBDFFu, 1);
    x += plus;
    footerIcon(4);
    text("Quick menu", x + gap, fy, 15.0f, 0xFFFFFFFFu, 0);
}

// New Super Paper Bros.: the boot screen "Hold the Wii Remote sideways." takes its line from a table in main.dol (one
// per language: the USA build has English, French and Japanese, the PAL build all five plus Japanese). Each one reads
// "Enjoy New Super Paper Bros!" instead, rewritten in guest memory once, by content, so it works for both regions;
// the controller in use is drawn on that screen by the button_icons texture sets.
void PatchBootStrings() {
    static bool done = false;
    if (done) return;
    done = true;
    static const char* const kLines[] = {
        "Hold the Wii Remote sideways.",
        "Hold the t\xe9l\xe9" "commande Wii sideways.",
        "Halte die Wii-Fernbedienung waagrecht.",
        "Tenez la t\xe9l\xe9" "commande Wii \xe0 l'horizontale.",
        "Sujeta el mando de Wii en posici\xf3n horizontal.",
        "Afferra il telecomando Wii alle estremit\xe0.",
        "\x82\x76\x82\x89\x82\x89\x83\x8a\x83\x82\x83\x52\x83\x93\x82\xf0\x81\x77\x82\xe6\x82\xb1\x82\xe0\x82\xbf\x81\x78"
        "\x82\xb5\x82\xc4\x82\xad\x82\xbe\x82\xb3\x82\xa2\x81\x42",  // Japanese, Shift-JIS
    };
    static constexpr std::string_view kText = "Enjoy New Super Paper Bros!";
    constexpr uint32_t kStart = 0x80004000u, kSize = 0x00600000u;  // main.dol's sections
    uint8_t* const mem = Memory::Contains(kStart, kSize) ? Memory::GetPointer(kStart, kSize) : nullptr;
    if (mem == nullptr) return;
    int patched = 0;
    for (const char* text : kLines) {
        const auto* line = reinterpret_cast<const uint8_t*>(text);  // bytes, not signed chars: the accented lines
        const size_t len = std::strlen(text) + 1;  // with its NUL: whole strings only
        for (uint8_t* p = mem; (p = std::search(p, mem + kSize, line, line + len)) != mem + kSize; p += len) {
            if (len - 1 < kText.size()) break;
            std::fill(p, p + len, uint8_t{0});
            std::copy(kText.begin(), kText.end(), p);
            ++patched;
        }
    }
    RT_LOGF(RT_TAG_RUNTIME, "boot screen: %d line(s) now read \"Enjoy New Super Paper Bros!\"\n", patched);
}

void Draw() noexcept {
    if (RuntimeFrontend::Active()) return;
    PatchBootStrings();
    // Wait for the frame worker's DONE phase: it has replayed the previous frame's ImGui draw lists
    // and started the next ImGui frame, so all overlay callers can now safely issue ImGui commands.
    aurora_wait_for_frame_worker();
    // Also drive the Wii Remote rescan from here: PADRead runs it too, but this
    // runs once per presented frame whatever the game is doing (e.g. sitting in
    // its "communications interrupted" prompt without polling pads). Same guest
    // thread as PADRead, so no concurrent access to the scanner's state.
    WiiRemoteInput::Poll();
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    UpdateBootShaderState();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    NetplayUi::Draw();
    DrawFpsOverlay();
    DrawDebugHud();
    DrawMainWindow();
    DrawExitPrompt();
    controller_mapping_wizard::Draw();
    ApplyInputBlockState();
    DrawControlsPanel();
    DrawStartupScreen();
}

bool StartupScreenVisible() noexcept {
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           !g_bootShadersReady.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept { ++g_presentedFrame; }
} // namespace settings_overlay
