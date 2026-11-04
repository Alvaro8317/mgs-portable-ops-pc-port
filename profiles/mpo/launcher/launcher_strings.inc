// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every text the launcher shows (F25d), one line per text:
//
//   MPO_TEXT(<id>, "<English>", "<Spanish>")
//
// A new text is one line here, in any place (the ids are only names); then
// tr(Text::<id>) in launcher_main.cpp. Placeholders are {0}, {1}, ... and
// both languages must use the same ones (mpo_launcher_core_test checks it,
// and that no text is empty). The file is UTF-8: the font covers Latin-1
// (mpo_launcher_font_test checks every character of this table).
//
// Kept out: names that do not change with the language (Vulkan, ISO,
// setup.py, PPSSPP) and what setup.py prints (it stays in English, see
// docs/sessions/F25d.md).

// The window and the header.
MPO_TEXT(kWindowTitle, "MGS: Portable Ops - PC port", "MGS: Portable Ops - port para PC")
MPO_TEXT(kHeader, "Metal Gear Solid: Portable Ops - unofficial PC port", "Metal Gear Solid: Portable Ops - port no oficial para PC")
MPO_TEXT(kNotAffiliated, "Not affiliated with Konami. You need your own copy of the game.", "Sin relación con Konami. Necesitas tu propia copia del juego.")
MPO_TEXT(kReadingInstall, "Reading the install...", "Leyendo la instalación...")
MPO_TEXT(kCannotReadInstall, "Could not read the install state.", "No se pudo leer el estado de la instalación.")
MPO_TEXT(kTryAgain, "Try again", "Reintentar")
MPO_TEXT(kMessageBoxTitle, "MPO launcher", "Launcher de MPO")
MPO_TEXT(kMissingFiles, "The port's files (source/profiles/mpo/scripts/setup.py) are not next to the launcher. Unpack the whole release again.", "Los archivos del port (source/profiles/mpo/scripts/setup.py) no están junto al launcher. Vuelve a descomprimir la release entera.")

// The file browser.
MPO_TEXT(kBrowserGo, "Go", "Ir")
MPO_TEXT(kBrowserUp, "Up", "Subir")
MPO_TEXT(kBrowserHome, "Home", "Inicio")
MPO_TEXT(kBrowserShowHidden, "Show hidden folders", "Mostrar las carpetas ocultas")
MPO_TEXT(kBrowserFolderTag, "[folder]", "[carpeta]")
MPO_TEXT(kBrowserEmpty, "(nothing to choose here)", "(aquí no hay nada que elegir)")
MPO_TEXT(kBrowserChooseFolder, "Choose this folder", "Elegir esta carpeta")
MPO_TEXT(kBrowserNoSuchFolder, "No such folder: {0}", "No existe la carpeta: {0}")
MPO_TEXT(kBrowserCannotOpen, "Cannot open this folder: {0}", "No se puede abrir esta carpeta: {0}")
MPO_TEXT(kCancel, "Cancel", "Cancelar")
MPO_TEXT(kShortcutPpsspp, "PPSSPP saves", "Partidas de PPSSPP")
MPO_TEXT(kShortcutPpssppFlatpak, "PPSSPP saves (Flatpak)", "Partidas de PPSSPP (Flatpak)")

// Before the install.
MPO_TEXT(kStepGame, "1. Your copy of the game", "1. Tu copia del juego")
MPO_TEXT(kStepGameHelp, "Choose the ISO of Metal Gear Solid: Portable Ops (USA, ULUS-10202). Nothing of the game leaves this computer: the port is compiled here, from your ISO.", "Elige la ISO de Metal Gear Solid: Portable Ops (EE. UU., ULUS-10202). Nada del juego sale de este equipo: el port se compila aquí, a partir de tu ISO.")
MPO_TEXT(kBrowse, "Browse...", "Examinar...")
MPO_TEXT(kChooseIso, "Choose the ISO", "Elige la ISO")
MPO_TEXT(kStepFonts, "2. PSP firmware fonts", "2. Fuentes del firmware de la PSP")
MPO_TEXT(kStepFontsHelp, "The folder flash0/font of a PSP or of PPSSPP. If it is next to the ISO, it is found by itself. Without it the game runs, and the save screens and the on-screen keyboard use a substitute font.", "La carpeta flash0/font de una PSP o de PPSSPP. Si está junto a la ISO, se encuentra sola. Sin ella el juego funciona, y las pantallas de guardado y el teclado en pantalla usan una fuente sustituta.")
MPO_TEXT(kChooseFontsFolder, "Choose the fonts folder", "Elige la carpeta de las fuentes")
MPO_TEXT(kStepInstall, "3. Install", "3. Instalar")
MPO_TEXT(kCompileAfterInstall, "Then compile every stage in the background", "Después, compilar todas las etapas en segundo plano")
MPO_TEXT(kInstallTime, "The first install takes 20 to 40 minutes and needs about 15 GB free (10 GB once done). You can play as soon as it ends; the stages keep compiling while you play.", "La primera instalación tarda de 20 a 40 minutos y necesita unos 15 GB libres (10 GB al terminar). Puedes jugar en cuanto acabe; las etapas se siguen compilando mientras juegas.")
MPO_TEXT(kAntivirus, "Some antivirus programs (Avast in particular) remove part of the compiler by mistake. Adding an exception for {0} first avoids it (see README.md).", "Algunos antivirus (sobre todo Avast) borran parte del compilador por error. Para evitarlo, añade antes una excepción para {0} (ver README.es.md).")
MPO_TEXT(kInstallButton, "Install", "Instalar")
MPO_TEXT(kUpdateSection, "Update", "Actualizar")
MPO_TEXT(kUpdateNeeded, "This version of the port is newer than the installed game.", "Esta versión del port es más nueva que el juego instalado.")
MPO_TEXT(kUpdateHelp, "Update compiles the game again with this version (only what changed; the stages already compiled are kept). Play waits until then.", "Actualizar vuelve a compilar el juego con esta versión (solo lo que cambió; las etapas ya compiladas se conservan). Jugar espera hasta entonces.")
MPO_TEXT(kUpdateButton, "Update", "Actualizar")

// Play.
MPO_TEXT(kPlaySection, "Play", "Jugar")
MPO_TEXT(kPlayButton, "Play", "Jugar")
MPO_TEXT(kPlaying, "Playing...", "Jugando...")
MPO_TEXT(kStagesCompiled, "Stages compiled: {0} of {1} ({2} optimized)", "Etapas compiladas: {0} de {1} ({2} optimizadas)")
MPO_TEXT(kStagesInterpreter, "A stage that is not compiled yet runs on the interpreter: it plays, but slower. A quick version of every stage comes first, then the optimized one.", "Una etapa que aún no está compilada corre en el intérprete: se puede jugar, pero va más lenta. Primero se compila una versión rápida de cada etapa y después la optimizada.")
MPO_TEXT(kStagesStarting, "starting", "empezando")
MPO_TEXT(kStopCompiling, "Stop compiling (after this stage)", "Dejar de compilar (después de esta etapa)")
MPO_TEXT(kCompileInBackground, "Compile the stages in the background", "Compilar las etapas en segundo plano")

// Settings (F24a).
MPO_TEXT(kSettingsSection, "Settings", "Ajustes")
MPO_TEXT(kSettingsUnknown, "This setup.py does not know the settings.", "Este setup.py no conoce los ajustes.")
MPO_TEXT(kRenderer, "Renderer", "Renderizador")
MPO_TEXT(kRendererSoft, "Software (slow, for diagnosis)", "Software (lento, para diagnóstico)")
MPO_TEXT(kResolution, "Resolution", "Resolución")
MPO_TEXT(kPixels, "{0} pixels", "{0} píxeles")
MPO_TEXT(kVulkanOnly, "(Vulkan only)", "(solo con Vulkan)")
MPO_TEXT(kWindow, "Window", "Ventana")
MPO_TEXT(kFullscreenNote, "(fullscreen)", "(pantalla completa)")
MPO_TEXT(kFullscreen, "Fullscreen (borderless, at the desktop's resolution)", "Pantalla completa (sin bordes, a la resolución del escritorio)")
MPO_TEXT(kAspect, "Aspect", "Proporción")
MPO_TEXT(kAspectNative, "16:9 (native)", "16:9 (nativa)")
MPO_TEXT(kAspectNote, "Wider than 16:9 shows more of the 3D at the sides; the HUD and the menus are stretched.", "Más ancho que 16:9 muestra más del 3D a los lados; el HUD y los menús salen estirados.")
MPO_TEXT(kTexturePack, "Texture pack", "Pack de texturas")
MPO_TEXT(kTexturePackOn, "Use it if there is one", "Usarlo si hay uno")
MPO_TEXT(kTexturePackOff, "The game's textures", "Las texturas del juego")
MPO_TEXT(kTexturePackFound, "Pack found in {0}. An HD pack asks more of the GPU and the memory.", "Hay un pack en {0}. Un pack HD le pide más a la GPU y a la memoria.")
MPO_TEXT(kTexturePackMissing, "No pack in {0} (see Texture packs in the README).", "No hay ningún pack en {0} (ver Packs de texturas en el README).")
MPO_TEXT(kLanguage, "Language", "Idioma")
MPO_TEXT(kLanguageAuto, "System ({0})", "El del sistema ({0})")
MPO_TEXT(kNotSaved, "Not saved: {0}", "No se guardó: {0}")
MPO_TEXT(kApplyNextPlay, "They apply the next time you press Play.", "Se aplican la próxima vez que pulses Jugar.")
MPO_TEXT(kSavedAt, "Changes are saved at once, in {0}", "Los cambios se guardan al momento, en {0}")

// Saves (F18c, F22c).
MPO_TEXT(kSavesSection, "Saves", "Partidas guardadas")
MPO_TEXT(kSavesHelp, "Bring a save from a PSP or PPSSPP (the PSP/SAVEDATA/ULUS10202... folder), or take yours to them.", "Trae una partida de una PSP o de PPSSPP (la carpeta PSP/SAVEDATA/ULUS10202...), o lleva allí las tuyas.")
MPO_TEXT(kImportSave, "Import a save...", "Importar una partida...")
MPO_TEXT(kExportSaves, "Export my saves...", "Exportar mis partidas...")
MPO_TEXT(kChooseImportFolder, "Choose the save folder to import", "Elige la carpeta de la partida que quieres importar")
MPO_TEXT(kChooseExportFolder, "Choose where to export the saves", "Elige dónde exportar las partidas")

// The fonts, after the install (F22c).
MPO_TEXT(kFontsSection, "PSP firmware fonts", "Fuentes del firmware de la PSP")
MPO_TEXT(kFontsInstalled, "Installed.", "Instaladas.")
MPO_TEXT(kFontsMissing, "Missing: the save screens and the on-screen keyboard use a substitute font.", "Faltan: las pantallas de guardado y el teclado en pantalla usan una fuente sustituta.")
MPO_TEXT(kFontsHelp, "Choose the flash0/font folder of a PSP, or assets/flash0/font in the PPSSPP folder.", "Elige la carpeta flash0/font de una PSP, o assets/flash0/font en la carpeta de PPSSPP.")
MPO_TEXT(kChangeFonts, "Change the fonts...", "Cambiar las fuentes...")
MPO_TEXT(kAddFonts, "Add the fonts...", "Añadir las fuentes...")

// What runs (the title above its progress bar and log).
MPO_TEXT(kWorkInstall, "Install", "Instalación")
MPO_TEXT(kWorkUpdate, "Update", "Actualización")
MPO_TEXT(kWorkFonts, "Fonts", "Fuentes")
MPO_TEXT(kWorkImportSave, "Import save", "Importar una partida")
MPO_TEXT(kWorkExportSaves, "Export saves", "Exportar las partidas")
MPO_TEXT(kInstallFolder, "Folder: {0}", "Carpeta: {0}")
MPO_TEXT(kInstallIso, "ISO: {0}", "ISO: {0}")

// When something fails.
MPO_TEXT(kFailCannotRunSetup, "Cannot run setup.py", "No se puede ejecutar setup.py")
MPO_TEXT(kFailCannotStart, "Cannot start: {0}", "No se puede iniciar: {0}")
MPO_TEXT(kFailWork, "{0} failed", "Falló: {0}")
MPO_TEXT(kFailStartStages, "Cannot start compiling the stages", "No se puede empezar a compilar las etapas")
MPO_TEXT(kFailStages, "Compiling the stages failed", "Falló la compilación de las etapas")
MPO_TEXT(kFailCannotStartGame, "Cannot start the game", "No se puede iniciar el juego")
MPO_TEXT(kFailGame, "The game stopped with an error", "El juego se detuvo con un error")
MPO_TEXT(kFailExplain, "The last lines are below; the full logs are in the logs folder of the install folder. If the game stopped while playing, what happened since your last save is lost.", "Abajo están las últimas líneas; los registros completos están en la carpeta logs de la carpeta de instalación. Si el juego se detuvo mientras jugabas, se pierde lo que pasó desde la última vez que guardaste.")
MPO_TEXT(kCopyToClipboard, "Copy to the clipboard", "Copiar al portapapeles")
MPO_TEXT(kClose, "Close", "Cerrar")
