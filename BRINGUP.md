# Pierwsze uruchomienie plytki

## Przed podlaczeniem do magistrali

1. Ustaw prawdziwy kod producenta i unikalny numer terminala w `idf.py menuconfig`:
   `ISOVT_MANUFACTURER_CODE` i `ISOVT_IDENTITY_NUMBER`.
2. Wgraj aktualny firmware z katalogu projektu.
3. Otworz monitor portu szeregowego.

Nie podlaczaj terminala od razu do maszyny roboczej. Pierwszy test CAN wykonaj na
stanowisku 250 kbit/s z poprawna terminacja 120 omow na obu koncach magistrali.

## Co powinno pojawic sie po starcie

Na LCD:

- logo VTISOBUS by FarmNavi,
- u gory ekran stanu `ISOBUS ACCESS TERMINAL`,
- po uruchomieniu CAN: `CAN READY`,
- komunikat `WAITING FOR MACHINE CONNECTION`, az do otrzymania working setu.

W logu powinny pojawic sie wpisy o inicjalizacji LCD RGB, identyfikatorze GT911,
uruchomieniu CAN/TWAI i adresie terminala VT.

## Test dotyku

Dotknij ekranu poza aktywnym working setem. Pole `STATUS` powinno pokazac
wspolrzedne dotkniecia. To potwierdza komunikacje z GT911 oraz orientacje osi.

## Test ISOBUS

Po przeslaniu poprawnego object poola przez ECU terminal sam odrysuje aktywny
DataMask. Aktualnie obslugiwane sa podstawowe maski, kontenery, klawisze
programowe, przyciski, pola bool/string/liczbowe, teksty, liczby i prostokaty.

Dotkniecie przycisku wysyla do ECU pojedyncze `pressed` i `released`. Dotkniecie
pola wejscia wysyla komunikat ustawienia fokusu; pole logiczne przelacza wartosc
`ON/OFF`. Pelny edytor tekstu i liczb oraz
grafika/picture graphic sa kolejnym etapem.

## Test AUX-N

Po obecnosci urzadzenia AUX-N Type 2 log powinien zglosic wykrycie jego NAME oraz
kodu modelu. Warstwa protokolowa ma juz wysylanie przypisan i odpowiedzi; ekran
konfiguracji przypisan zostanie dodany po potwierdzeniu komunikacji z prawdziwym
kontrolerem.
