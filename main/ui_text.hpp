#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Texts of the terminal's own user interface (menus, status screen, notices).
// Machine content from object pools is not translated here.
//
// Each row of UI_TEXTS is ID, English, German, Polish (UTF-8, natural case).
// To add a language, append a column to every row (nullptr falls back to
// English) and a value to ui_text::Language. Format rows use printf
// conversions, which every translation must keep in the same order.
#define UI_TEXTS(X)                                                                                              \
    /* Status screen */                                                                                         \
    X(StatusHeader, "ISOBUS access terminal", "ISOBUS-Zugangsterminal", "Terminal dostępowy ISOBUS")            \
    X(CanReady, "CAN ready", "CAN bereit", "CAN gotowy")                                                        \
    X(CanStarting, "CAN starting", "CAN startet", "CAN uruchamia się")                                         \
    X(Status, "Status", "Status", "Status")                                                                     \
    X(LoadingMachineData, "Loading machine data", "Maschinendaten werden geladen", "Ładowanie danych maszyny")  \
    X(VtServerReady, "VT server ready   AUX-N available", "VT-Server bereit   AUX-N verfügbar",                  \
      "Serwer VT gotowy   AUX-N dostępny")                                                                      \
    X(WaitingForMachine, "Waiting for machine connection", "Warte auf Maschinenverbindung",                     \
      "Oczekiwanie na połączenie z maszyną")                                                                    \
    X(StatusStarting, "Starting", "Startet", "Uruchamianie")                                                    \
    X(StatusInitializing, "Initializing ISOBUS terminal", "ISOBUS-Terminal wird initialisiert",                \
      "Inicjalizacja terminala ISOBUS")                                                                         \
    X(StatusVtStartFailed, "ISOBUS VT start failed", "Start des ISOBUS-VT fehlgeschlagen",                     \
      "Uruchomienie ISOBUS VT nie powiodło się")                                                                \
    X(StatusStartupTaskFailed, "ISOBUS startup task failed", "ISOBUS-Starttask fehlgeschlagen",                \
      "Zadanie startowe ISOBUS nie powiodło się")                                                               \
    X(StatusCanStartFailed, "CAN start failed", "CAN-Start fehlgeschlagen", "Uruchomienie CAN nie powiodło się") \
    X(StatusWorkingSetSuspended, "Working set suspended after a VT protocol error",                             \
      "Working Set nach VT-Protokollfehler angehalten", "Working set wstrzymany po błędzie protokołu VT")        \
    X(StatusParsingPool, "Parsing ISOBUS object pool", "ISOBUS-Objektpool wird verarbeitet",                   \
      "Przetwarzanie puli obiektów ISOBUS")                                                                     \
    X(StatusRendererFailed, "VT renderer startup failed", "Start des VT-Renderers fehlgeschlagen",             \
      "Uruchomienie renderera VT nie powiodło się")                                                             \
    X(StatusAddressClaimFailed, "ISOBUS address claim setup failed", "ISOBUS-Adressvergabe fehlgeschlagen",    \
      "Konfiguracja adresu ISOBUS nie powiodła się")                                                            \
    X(StatusVtReady, "ISOBUS VT and AUX-N assignment ready", "ISOBUS-VT und AUX-N-Zuordnung bereit",            \
      "ISOBUS VT i przypisania AUX-N gotowe")                                                                   \
    X(StatusCanSmokeTest, "CAN smoke test active; AgIsoStack++ disabled",                                       \
      "CAN-Schnelltest aktiv; AgIsoStack++ deaktiviert", "Test CAN aktywny; AgIsoStack++ wyłączony")             \
    X(StatusEcuOnline, "Machine ECU online, SA %u", "Maschinen-ECU online, SA %u", "ECU maszyny online, SA %u") \
    X(StatusEcuOffline, "Machine ECU offline", "Maschinen-ECU offline", "ECU maszyny offline")                  \
    X(StatusWorkingSetReceived, "Working set received", "Working Set empfangen", "Odebrano working set")        \
    X(StatusWorkingSetRepaint, "Working set repainted", "Working Set neu gezeichnet", "Working set odświeżony") \
    X(StatusActiveMask, "Active mask %u", "Aktive Maske %u", "Aktywna maska %u")                               \
    X(StatusSoftKeyMask, "Soft key mask %u", "Softkey-Maske %u", "Maska softkey %u")                            \
    X(StatusFocusObject, "Focus object %u", "Fokusobjekt %u", "Obiekt z fokusem %u")                            \
    X(StatusAuxInputFound, "AUX-N input found", "AUX-N-Eingabegerät gefunden", "Znaleziono wejście AUX-N")      \
    X(StatusAuxResult, "AUX-N result %u", "AUX-N-Ergebnis %u", "Wynik AUX-N %u")                                \
    X(GnssModuleOff, "GNSS module off", "GNSS-Modul aus", "Moduł GNSS wyłączony")                               \
    X(GnssPinsRequired, "GNSS UART pins required", "GNSS-UART-Pins erforderlich", "Wymagane piny UART GNSS")    \
    X(GnssNtripTxRequired, "GNSS NTRIP TX pin required", "GNSS-NTRIP-TX-Pin erforderlich",                     \
      "Wymagany pin TX NTRIP GNSS")                                                                             \
    X(GnssNoFix, "GNSS no fix", "GNSS kein Fix", "GNSS brak fixa")                                              \
    X(GnssFix, "GNSS %s, %u satellites", "GNSS %s, %u Satelliten", "GNSS %s, %u satelitów")                    \
    X(GnssNtripOn, ", NTRIP on", ", NTRIP aktiv", ", NTRIP aktywny")                                            \
    X(GnssNtripWaiting, ", NTRIP waiting", ", NTRIP wartet", ", NTRIP oczekuje")                                \
    /* Terminal menu */                                                                                         \
    X(MenuSubtitle, "Terminal menu", "Terminalmenü", "Menu terminala")                                          \
    X(MenuMachines, "Machines", "Maschinen", "Maszyny")                                                         \
    X(MenuTerminal, "Terminal", "Terminal", "Terminal")                                                         \
    X(MenuGnss, "GNSS / sections", "GNSS / Teilbreiten", "GNSS / sekcje")                                      \
    X(MenuAudio, "Sound", "Ton", "Dźwięk")                                                                      \
    X(MenuInformation, "Information", "Information", "Informacje")                                              \
    X(MenuBack, "Back", "Zurück", "Powrót")                                                                     \
    /* Machines page */                                                                                         \
    X(MachinesSubtitle, "VT clients and stored object pools", "VT-Steuergeräte und gespeicherte Objektpools",   \
      "Sterowniki VT i zapisane pule obiektów")                                                                 \
    X(MachineController, "%sController SA %u", "%sSteuergerät SA %u", "%sSterownik SA %u")                     \
    X(MachineActive, "Active   ", "Aktiv   ", "Aktywny   ")                                                     \
    X(MachineDetail, "NAME %016llX   stored versions: %u", "NAME %016llX   gespeicherte Versionen: %u",         \
      "NAME %016llX   zapisane wersje: %u")                                                                     \
    X(MachinesNone, "No controllers waiting for the VT", "Keine Steuergeräte warten auf das VT",               \
      "Brak sterowników oczekujących na VT")                                                                    \
    X(MachinesStorage, "Storage: %u pools   %u KB free", "Speicher: %u Pools   %u KB frei",                    \
      "Pamięć: %u pul   %u KB wolne")                                                                           \
    X(EraseActivePool, "Delete active pool", "Aktiven Pool löschen", "Usuń pulę aktywnej")                      \
    X(EraseAllPools, "Delete all pools", "Alle Pools löschen", "Usuń wszystkie pule")                           \
    X(WorkingSetNoPool, "No pool", "Kein Pool", "Brak puli")                                                    \
    /* Terminal page */                                                                                         \
    X(TerminalSubtitle, "Settings sent to machines over ISOBUS", "Einstellungen für Maschinen per ISOBUS",      \
      "Ustawienia przekazywane maszynom przez ISOBUS")                                                          \
    X(LanguageSetting, "Language: %s   Country: %s", "Sprache: %s   Land: %s", "Język: %s   Kraj: %s")          \
    X(UnitsMetric, "Units: metric", "Einheiten: metrisch", "Jednostki: metryczne")                              \
    X(UnitsImperial, "Units: imperial", "Einheiten: imperial", "Jednostki: imperialne")                         \
    X(DecimalComma, "Decimal separator: comma", "Dezimaltrennzeichen: Komma", "Separator dziesiętny: przecinek") \
    X(DecimalPoint, "Decimal separator: point", "Dezimaltrennzeichen: Punkt", "Separator dziesiętny: kropka")   \
    X(Time24, "Time format: 24 h", "Zeitformat: 24 h", "Format czasu: 24 h")                                    \
    X(Time12, "Time format: 12 h AM/PM", "Zeitformat: 12 h AM/PM", "Format czasu: 12 h AM/PM")                 \
    X(DateDayMonthYear, "Date: DD/MM/YYYY", "Datum: TT/MM/JJJJ", "Data: DD/MM/RRRR")                            \
    X(DateMonthDayYear, "Date: MM/DD/YYYY", "Datum: MM/TT/JJJJ", "Data: MM/DD/RRRR")                            \
    X(DateYearMonthDay, "Date: YYYY-MM-DD", "Datum: JJJJ-MM-TT", "Data: RRRR-MM-DD")                            \
    X(TerminalNumber, "Terminal number: %u (1-32)", "Terminalnummer: %u (1-32)", "Numer terminala: %u (1-32)")  \
    X(WorkingArea, "Working area: %u x %u", "Arbeitsbereich: %u x %u", "Obszar roboczy: %u x %u")               \
    X(TerminalCapabilities, "VT %u   version 6   %u x %u   12 keys", "VT %u   Version 6   %u x %u   12 Tasten", \
      "VT %u   wersja 6   %u x %u   12 klawiszy")                                                               \
    X(RestartRequired, "VT number and working area take effect after a restart",                                \
      "VT-Nummer und Arbeitsbereich gelten nach einem Neustart", "Numer VT i obszar roboczy wymagają restartu") \
    /* ISOBUS page */                                                                                           \
    X(IsobusSubtitle, "Network diagnostics on the terminal", "Netzwerkdiagnose am Terminal",                    \
      "Diagnostyka sieci wyświetlana na terminalu")                                                             \
    X(Bus, "Bus", "Bus", "Magistrala")                                                                          \
    X(BusLoad, "Bus load", "Buslast", "Obciążenie")                                                             \
    X(NodesOnline, "Nodes online", "Teilnehmer online", "Węzły online")                                         \
    X(VtAddress, "VT address", "VT-Adresse", "Adres VT")                                                        \
    X(NodeLine, "SA %3u   function %3u   mfr. %4u   NAME %016llX", "SA %3u   Funktion %3u   Herst. %4u   NAME %016llX", \
      "SA %3u   funkcja %3u   prod. %4u   NAME %016llX")                                                        \
    X(RefreshDevices, "Refresh devices", "Geräte aktualisieren", "Odśwież urządzenia")                          \
    /* AUX page */                                                                                              \
    X(AuxSubtitleLegacy, "AUX-O compatibility mode / VT version 2", "AUX-O-Kompatibilitätsmodus / VT-Version 2", \
      "Tryb zgodności AUX-O / VT wersja 2")                                                                     \
    X(AuxSubtitle, "AUX-N type 2 / ISO 11783-6 VT version 6", "AUX-N Typ 2 / ISO 11783-6 VT-Version 6",          \
      "AUX-N typ 2 / ISO 11783-6 VT wersja 6")                                                                  \
    X(AuxFunctions, "Machine functions: %u", "Maschinenfunktionen: %u", "Funkcje maszyny: %u")                 \
    X(AuxInputs, "Inputs: %u", "Eingänge: %u", "Wejścia: %u")                                                   \
    X(AuxLearning, "Learning...", "Anlernen...", "Uczenie...")                                                  \
    X(AuxLearn, "Learn button", "Taste anlernen", "Ucz przycisk")                                               \
    X(AuxDeleteSelected, "Delete selected", "Auswahl löschen", "Usuń wybrane")                                  \
    X(AuxDeleteAll, "Delete all", "Alle löschen", "Usuń wszystkie")                                             \
    X(AuxOnOff, "On/off", "Ein/Aus", "Wł/wył")                                                                  \
    X(AuxAnalogueLatching, "Analog latching", "Analog rastend", "Analog stały")                                \
    X(AuxButton, "Button", "Taster", "Przycisk")                                                                \
    X(AuxAnalogueTwoWay, "Analog 2-way", "Analog 2-seitig", "Analog 2-kier.")                                   \
    X(AuxAnalogueOneWay, "Analog 1-way", "Analog 1-seitig", "Analog 1-kier.")                                   \
    X(AuxDualOnOff, "2x on/off", "2x Ein/Aus", "2x wł/wył")                                                     \
    X(AuxDualButton, "2x button", "2x Taster", "2x przycisk")                                                   \
    X(AuxDualUp, "2x up", "2x auf", "2x góra")                                                                  \
    X(AuxDualDown, "2x down", "2x ab", "2x dół")                                                                \
    X(AuxAnalogueOnOff, "Analog+on/off", "Analog+Ein/Aus", "Analog+wł/wył")                                    \
    X(AuxLatchingOnOff, "Latch.+on/off", "Rast.+Ein/Aus", "St.+wł/wył")                                        \
    X(AuxQuadratureButton, "Quad. button", "Quad. Taster", "Kwadr. przycisk")                                  \
    X(AuxQuadratureLatching, "Quad. latching", "Quad. rastend", "Kwadr. analog st.")                           \
    X(AuxQuadratureAnalogue, "Quad. analog", "Quad. analog", "Kwadr. analog")                                  \
    X(AuxEncoder, "Encoder 2-way", "Drehgeber", "Enkoder 2-kier.")                                             \
    X(AuxUnknown, "Unknown", "Unbekannt", "Nieznany")                                                           \
    /* GNSS page */                                                                                             \
    X(GnssSubtitle, "Receiver state, NTRIP and automation locks", "Empfänger, NTRIP und Automatiksperren",      \
      "Stan odbiornika, NTRIP i blokady automatyki")                                                            \
    X(GnssSource, "Source", "Quelle", "Źródło")                                                                 \
    X(GnssSourceOff, "Module off", "Modul aus", "Moduł wyłączony")                                              \
    X(GnssSatellites, "Satellites", "Satelliten", "Satelity")                                                   \
    X(GnssSpeedCourse, "Speed / course", "Geschwindigkeit / Kurs", "Prędkość / kierunek")                       \
    X(GnssPosition, "Position", "Position", "Pozycja")                                                          \
    X(NtripConnected, "Connected", "Verbunden", "Połączony")                                                    \
    X(NtripWaiting, "Waiting", "Wartet", "Oczekiwanie")                                                         \
    X(NtripOff, "Off", "Aus", "Wyłączony")                                                                      \
    X(SectionMinimum, "Minimum for sections", "Minimum für Teilbreiten", "Minimum dla sekcji")                  \
    X(FixGateFloat, "RTK float or fix", "RTK Float oder Fix", "RTK float lub fix")                              \
    X(FixGateFixed, "RTK fix only", "Nur RTK Fix", "Tylko RTK fix")                                             \
    X(Automation, "Automation", "Automatik", "Automatyka")                                                      \
    X(AutomationValue, "%s   %u sections   outputs locked", "%s   %u Teilbreiten   Ausgänge gesperrt",           \
      "%s   %u sekcji   wyjścia zablokowane")                                                                   \
    X(OutputsLocked, "Outputs stay locked until TC-SC is implemented", "Ausgänge bleiben bis zur TC-SC-Umsetzung gesperrt", \
      "Wyjścia pozostają zablokowane do implementacji TC-SC")                                                   \
    /* Sound page */                                                                                            \
    X(AudioSubtitle, "Volume of terminal signals and machine alarms", "Lautstärke von Signalen und Alarmen",    \
      "Głośność sygnałów terminala i alarmów maszyny")                                                          \
    X(AudioControl, "Operating signals", "Bediensignale", "Sygnały robocze")                                    \
    X(AudioAlarms, "Alarms", "Alarme", "Alarmy")                                                                \
    X(AudioMuted, "Sound muted - turn on", "Ton stumm - einschalten", "Dźwięk wyciszony - włącz")               \
    X(AudioOn, "Sound on - mute", "Ton an - stummschalten", "Dźwięk włączony - wycisz")                         \
    X(TestSignal, "Test signal", "Signal testen", "Test sygnału")                                               \
    X(TestAlarm, "Test alarm", "Alarm testen", "Test alarmu")                                                   \
    X(AudioHint, "Range 0-100 %, step 5 %. Settings are saved.", "Bereich 0-100 %, Schritt 5 %. Wird gespeichert.", \
      "Zakres 0-100 %, krok 5 %. Ustawienia są zapamiętywane.")                                                 \
    /* Information page */                                                                                      \
    X(InfoSubtitle, "Terminal capabilities and state", "Fähigkeiten und Zustand des Terminals",                 \
      "Możliwości i stan terminala")                                                                            \
    X(InfoSoftware, "Software", "Software", "Oprogramowanie")                                                   \
    X(InfoVtVersion, "VT version", "VT-Version", "Wersja VT")                                                   \
    X(InfoScreen, "Screen", "Bildschirm", "Ekran")                                                              \
    X(InfoScreenValue, "%d x %d touch", "%d x %d Touch", "%d x %d dotykowy")                                    \
    X(InfoAreaSoftKeys, "Area / soft keys", "Bereich / Softkeys", "Obszar / softkey")                           \
    X(InfoAreaSoftKeysValue, "480 x 480 / 12 keys", "480 x 480 / 12 Tasten", "480 x 480 / 12 klawiszy")         \
    X(InfoPoolStorage, "Pool storage", "Pool-Speicher", "Pamięć pul")                                           \
    X(InfoFreeRam, "Free RAM", "Freier RAM", "Wolna pamięć RAM")                                                \
    X(InfoDiagnostics, "Diagnostics", "Diagnose", "Diagnostyka")                                                \
    X(InfoDiagnosticsScreenOnly, "On screen only", "Nur am Bildschirm", "Tylko na ekranie")                     \
    /* Input editor */                                                                                          \
    X(EditorNumber, "Enter number", "Zahl eingeben", "Wprowadź liczbę")                                         \
    X(EditorText, "Enter text", "Text eingeben", "Wprowadź tekst")                                              \
    X(EditorList, "Select item", "Eintrag wählen", "Wybierz pozycję")                                           \
    X(KeyUp, "Up", "Auf", "Góra")                                                                               \
    X(KeyDown, "Down", "Ab", "Dół")                                                                             \
    X(KeyCancel, "Cancel", "Abbruch", "Anuluj")                                                                 \
    X(KeyBackspace, "Delete", "Löschen", "Cofnij")                                                              \
    X(KeyClear, "Clear", "Leeren", "Wyczyść")                                                                   \
    X(KeySpace, "Space", "Leerzeichen", "Spacja")                                                               \
    /* Notices */                                                                                               \
    X(NoticeIdentify, "VT terminal identification", "VT-Terminal-Identifikation", "Identyfikacja terminala VT") \
    X(NoticeSettingsSent, "Settings saved and sent to the network", "Einstellungen gespeichert und gesendet",  \
      "Ustawienia zapisane i wysłane do sieci")                                                                 \
    X(NoticeSettingsFailed, "Setting changed, saving or sending failed", "Geändert, Speichern oder Senden fehlgeschlagen", \
      "Ustawienie zmienione, błąd zapisu lub wysyłania")                                                        \
    X(NoticeVtNumberSaved, "VT number saved, restart the terminal", "VT-Nummer gespeichert, Terminal neu starten", \
      "Numer VT zapisany, uruchom terminal ponownie")                                                           \
    X(NoticeVtNumberFailed, "VT number changed, but saving failed", "VT-Nummer geändert, Speichern fehlgeschlagen", \
      "Numer zmieniony, ale zapis nie udał się")                                                                \
    X(NoticeAreaSaved, "Working area saved, restart the terminal", "Arbeitsbereich gespeichert, Terminal neu starten", \
      "Rozmiar zapisany, uruchom terminal ponownie")                                                            \
    X(NoticeAreaFailed, "Working area changed, but saving or pool deletion failed",                            \
      "Arbeitsbereich geändert, Speichern oder Löschen fehlgeschlagen",                                         \
      "Rozmiar zmieniony, ale zapis lub usunięcie pul nie udało się")                                           \
    X(NoticeAudioSaved, "Sound settings saved", "Toneinstellungen gespeichert", "Ustawienia dźwięku zapisane")  \
    X(NoticeAudioFailed, "Saving sound settings failed", "Speichern der Toneinstellungen fehlgeschlagen",       \
      "Błąd zapisu ustawień dźwięku")                                                                           \
    X(NoticeAuxModeSaved, "AUX mode saved, restart the terminal", "AUX-Modus gespeichert, Terminal neu starten", \
      "Tryb AUX zapisany, uruchom terminal ponownie")                                                           \
    X(NoticeAuxModeFailed, "Saving the AUX mode failed", "Speichern des AUX-Modus fehlgeschlagen",              \
      "Nie udało się zapisać trybu AUX")                                                                        \
    X(NoticeMuted, "Sound is muted", "Ton ist stumm", "Dźwięk jest wyciszony")                                  \
    X(NoticeRefreshStarted, "Device search started", "Gerätesuche gestartet", "Wyszukiwanie urządzeń uruchomione") \
    X(NoticeRefreshFailed, "Refreshing the network failed", "Netzwerk konnte nicht aktualisiert werden",        \
      "Nie udało się odświeżyć sieci")                                                                          \
    X(NoticeActivePoolDeleted, "Stored pool of the active machine deleted", "Gespeicherter Pool der aktiven Maschine gelöscht", \
      "Zapisana pula aktywnej maszyny usunięta")                                                                \
    X(NoticeActivePoolFailed, "No pool or deletion failed", "Kein Pool oder Löschen fehlgeschlagen",            \
      "Brak puli lub błąd usuwania")                                                                            \
    X(NoticeAllPoolsDeleted, "All stored pools deleted", "Alle gespeicherten Pools gelöscht",                   \
      "Wszystkie zapisane pule usunięte")                                                                       \
    X(NoticeAllPoolsFailed, "Deleting pools failed", "Löschen der Pools fehlgeschlagen", "Błąd usuwania pul")   \
    X(NoticeAuxUnassignAllSent, "AUX-N delete command sent", "AUX-N-Löschbefehl gesendet",                      \
      "Polecenie usunięcia AUX-N wysłane")                                                                      \
    X(NoticeAuxUnassignAllFailed, "No active machine or AUX-N error", "Keine aktive Maschine oder AUX-N-Fehler", \
      "Brak aktywnej maszyny lub błąd AUX-N")                                                                   \
    X(NoticeAuxUnassignSent, "Removal of the selected assignment sent", "Entfernen der Zuordnung gesendet",     \
      "Usunięcie wybranego przypisania wysłane")                                                                \
    X(NoticeAuxSelectFirst, "Select an AUX-N function first", "Zuerst eine AUX-N-Funktion wählen",              \
      "Najpierw wybierz funkcję AUX-N")                                                                         \
    X(NoticeAuxLearnCancelled, "AUX-N learning cancelled", "AUX-N-Anlernen abgebrochen", "Uczenie AUX-N anulowane") \
    X(NoticeAuxMoveInput, "Move the chosen AUX-N control", "Gewähltes AUX-N-Bedienelement betätigen",           \
      "Porusz wybranym przyciskiem AUX-N")                                                                      \
    X(NoticeAuxLearnFailed, "Select a function and connect AUX-N", "Funktion wählen und AUX-N anschließen",     \
      "Wybierz funkcję i podłącz AUX-N")                                                                        \
    X(NoticeAuxFunctionSelected, "Function selected - touch a matching input", "Funktion gewählt - passenden Eingang antippen", \
      "Funkcja wybrana - dotknij zgodnego wejścia")                                                             \
    X(NoticeAuxFunctionFailed, "Selecting the AUX function failed", "AUX-Funktion konnte nicht gewählt werden", \
      "Nie udało się wybrać funkcji AUX")                                                                       \
    X(NoticeAuxAssignmentSent, "AUX assignment sent", "AUX-Zuordnung gesendet", "Przypisanie AUX wysłane")      \
    X(NoticeAuxAssignmentFailed, "Select a function and a matching AUX input", "Funktion und passenden AUX-Eingang wählen", \
      "Wybierz funkcję i zgodne wejście AUX")

namespace ui_text
{
enum class Id : std::uint16_t
{
#define UI_TEXT_ID(id, english, german, polish) id,
    UI_TEXTS(UI_TEXT_ID)
#undef UI_TEXT_ID
    Count,
};

enum class Language : std::uint8_t
{
    English,
    German,
    Polish,
    Count,
};

// A text that is produced in the current language each time it is shown.
using Text = std::function<std::string()>;

void set_language(Language language);
Language language();
// UTF-8 text in the current language; falls back to English.
const char *get(Id id);
// printf-style formatting with the translated format string of id.
std::string format(Id id, ...);
Text text(Id id);
Text text(Id id, unsigned value);
}
