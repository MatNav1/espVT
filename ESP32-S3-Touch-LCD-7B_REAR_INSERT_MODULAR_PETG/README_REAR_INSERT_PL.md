# Obudowa ESP32-S3 Touch LCD 7B — montaż ekranu od tyłu

Ta wersja wykorzystuje cztery fabryczne gwintowane blaszki M3 modułu Waveshare.
Wyświetlacz wsuwa się do przedniego korpusu od strony pleców i przykręca czterema
krótkimi śrubami M3. Szkło nie jest dociskane osobną ramką.

## Części

- `ESP32-S3-Touch-LCD-7B_front_REAR_INSERT_PETG.stl` — przedni korpus/wanna
- `ESP32-S3-Touch-LCD-7B_rear_MODULAR_PETG.stl` — tylna pokrywa VESA/RAM
- `ESP32-S3-Touch-LCD-7B_panel_UM982_PETG.stl` — dolny panel z dwoma SMA
- `ESP32-S3-Touch-LCD-7B_panel_RS232_PETG.stl` — wymienny dolny panel DB9
- `test_Waveshare_M3_126.20x65.65_PETG.stl` — cienki wzornik rozstawu gwintów

## Mocowanie wyświetlacza

- rozstaw fabrycznych gwintów: 126,20 × 65,65 mm
- gwint: 4 × M3
- otwory przelotowe w podporach: Ø3,4 mm
- zalecane śruby wyświetlacza: M3×6 mm z cienką podkładką
- luz kieszeni wokół szkła: 0,35 mm na stronę
- przednia krawędź korpusu wystaje 1,8 mm ponad powierzchnię szkła

Przed ostatecznym skręceniem należy sprawdzić głębokość gwintu w blaszkach.
Śruba nie może dojść do szkła ani elementów wyświetlacza. Jeśli M3×6 mm okaże
się za długa, należy użyć M3×5 mm lub dodatkowej podkładki.

Zalecam najpierw wydrukować wzornik rozstawu gwintów. Zużywa mało materiału
i pozwala potwierdzić dopasowanie 126,20 × 65,65 mm przed drukiem korpusu.

## Kolejność składania

1. Wsuń cały moduł Waveshare od strony pleców przedniego korpusu.
2. Ułóż cztery gwintowane blaszki na wydrukowanych podporach.
3. Przykręć moduł od tyłu czterema krótkimi śrubami M3.
4. Podłącz przewody, UM982 lub RS232 oraz włącznik.
5. Załóż tylną pokrywę i skręć ją czterema długimi śrubami obudowy.

## Druk PETG

- dysza 0,4 mm; warstwa 0,20 mm
- 4 obrysy, 5 warstw dolnych i górnych
- 30–40% gyroid
- drukuj płaskim frontem na stole
- zastosuj podpory „tylko od stołu” pod czterema wewnętrznymi podporami M3
