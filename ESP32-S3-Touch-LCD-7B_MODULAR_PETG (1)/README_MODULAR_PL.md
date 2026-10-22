# Modułowa obudowa PETG — ESP32-S3 Touch LCD 7B

Wariant ma jeden wspólny korpus tylny i mały wymienny panel elektroniki. Panel znajduje się **od spodu obudowy**, w miejscu wcześniejszego złącza RS232. Można go zmienić bez drukowania całej obudowy, a tylna płyta z VESA i RAM/AMPS pozostaje wolna.

## Części

- `ESP32-S3-Touch-LCD-7B_front_PETG.stl` — wspólny front
- `ESP32-S3-Touch-LCD-7B_rear_MODULAR_PETG.stl` — wspólny tył z VESA i RAM/AMPS
- `ESP32-S3-Touch-LCD-7B_panel_UM982_PETG.stl` — panel pod UM982
- `ESP32-S3-Touch-LCD-7B_panel_RS232_PETG.stl` — panel pod RS232/DB9

## Wymienny panel

- zewnętrzny wymiar panelu: 56 × 46 mm
- otwór serwisowy w dolnej ściance: 44 × 34 mm
- rozstaw czterech śrub panelu: 48 × 40 mm
- mocowanie: 4 × M3
- panel ma język ustalający z luzem 0,15 mm na stronę
- zalecane wkładki termiczne M3 w czterech gniazdach dolnej ścianki

## Panel UM982

Przeznaczony dla płytki pokazanej przez użytkownika:

- PCB: 25,66 × 36,58 mm
- panel ma tylko dwa otwory Ø6,8 mm pod panelowe SMA dla ANT1 i ANT2
- nie ma na nim słupków ani otworów do mocowania płytki
- złącza SMA łączą się z UM982 krótkimi przewodami/pigtailami koncentrycznymi

UM982 znajduje się na czterech słupkach po wewnętrznej stronie tylnej ścianki obudowy. Rozstaw słupków wynosi 20,07 × 30,86 mm, a otwory pilotujące Ø2,1 mm są przeznaczone pod krótkie śruby M2,5. Dolny panel pełni wyłącznie rolę przepustu dla dwóch anten.

## Panel RS232

- otwór panelowego DE-9/DB9: 20,5 × 11 mm
- rozstaw śrub DB9: 25 mm
- dwie podniesione szyny po wewnętrznej stronie do zamocowania opaskami małego konwertera TTL↔RS232

Przed drukiem panelu warto sprawdzić konkretne gniazdo na pliku `test_DB9_panel_PETG.stl`.

## Montaż i druk

- materiał: PETG
- dysza 0,4 mm, warstwa 0,20 mm
- 4 obrysy, 5 warstw dolnych i górnych
- korpus tylny: 25–35% gyroid
- panele: 40–60% wypełnienia lub pełne
- tył drukować zamkniętą płytą na stole
- panele drukować płaską stroną zewnętrzną na stole; po montażu ta strona jest skierowana w dół

Do skręcenia głównej obudowy potrzebne są 4 × M3×50 mm. Do wymiennego panelu wystarczą 4 × M3×8 mm. Można zastosować cienką uszczelkę piankową wokół języka panelu.
