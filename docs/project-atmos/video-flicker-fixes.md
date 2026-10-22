# Flackern aus den zwei Videos — 19.10.2026

Analysierte Aufnahmen: `2026-10-19 21-03-14.mp4` (2,77 s) und `2026-10-19 21-06-19.mp4` (4,67 s), jeweils 2560×1440 bei 60 Bildern/s. Kontaktbögen und Detailfolgen liegen in `artifacts/video-flicker-qa/`. Die Aufnahmen werden nicht verändert.

## Ursache

Die großen dreieckigen bzw. blockigen Flächen sind von bewegten Gästen/Booten verformte Wolkenschatten und Nebelfelder. Für jeden 8×8-Knoten wurde bisher ein einzelner Tiefenwert des sichtbaren Sprites gelesen. Trat ein Gast oder Boot auf diesen Knoten, ersetzte seine aufrechte Spritetiefe die Bodentiefe. Diese falsche Projektion wurde räumlich interpoliert und bis zu fünf Bilder zwischengespeichert. Deshalb wanderten und sprangen große Flächen neben den kleinen eigentlichen Besucherschatten.

Zusätzlich verwendete die Wasserzonen-Berechnung sichtbare Wasserpixel: Ein Boot konnte ihre zwischengespeicherte Dichte verändern. Bereits drei von 16 Stichproben galten als volles Wasser. Die vertikale Wellenverschiebung wurde auf ganze Pixel gerundet und konnte dadurch Texturpixel abrupt umschalten.

## Änderung und Eigentümerschaft

Der native X8-Renderer hält jetzt zusätzlich `receiverDepth` und `receiverFlags`: die zuletzt gezeichnete Boden-/Wasseroberfläche unter aufrechten Sprites. Boden und Wasser aktualisieren diese Werte, Gäste/Boote/Gebäude erhalten sie, Außenkacheln löschen sie. Die Daten folgen demselben Pixelindex wie die Szene. Dirty-Repaint, Scrollen, Resize, Einschalten und Abschalten bewegen oder löschen alle Ebenen gemeinsam. Generischer Pixelblit und RLE-Schnellpfad behandeln sie gleich. Die Zeichenjobs sind abgeschlossen, bevor Atmosphere diese Daten liest.

Wolkenschatten/Nebel werden durch die stabile Empfängeroberfläche projiziert. UI und vorbeilaufende Sprites verändern die Knoten nicht mehr. Eine Kartenänderung invalidiert den Wolken-Cache unmittelbar. Wasserzonen verwenden das darunterliegende Material und die tatsächliche Stichprobenanzahl; die exakte sichtbare Wassermaske schützt Boote weiterhin vor Wasserfärbung und Verformung.

Die Wasserbewegung mischt jetzt vier zulässige Wasserfarben mit kontinuierlichen horizontalen und vertikalen Gewichten. Boots-, Ufer- und UI-Pixel werden nicht übernommen. Diese Farben gehen vor den Schatten-/Tilt-Shift-Stufen in die Weltfarbe ein; beide Renderwege verwenden dieselbe Mischung. Sonneneinstellungen, Besuchermodelle und das Nutzerprofil bleiben erhalten.

## Verifikation

- Release x64 Core/UI/Link und Versionsstart erfolgreich. Aktiver Build `Project-Atmos-flicker-fix.exe`.
- `test-atmosphere-core.ps1`: 18 bewegte Gäste-/Bootsphasen über mehrere Wolken-Cache-Neuberechnungen lassen die zugrunde liegenden Schatten-/Nebelfelder exakt unverändert. Kontrollversuch mit absichtlich als Boden eingespeister sichtbarer Spritetiefe reproduziert die Verformung; der Test ist damit nicht nur ein gleichförmiger Hintergrundvergleich.
- Derselbe native Compositortest prüft, dass ein Boot nach Wasser-Cache-Neuberechnung die angrenzende Ufer-/Tiefenfärbung nicht verändert. Native X8-Ebenen werden bei Allokation, Scrollen, Resize und Abschalten geprüft.
- Direkte native Generic-/RLE-Blits prüfen Boden, Wasser, aufrechte Sprites, Glas, transparente Pixel und Außenkacheln. 242 überlappende Scrollfälle bestehen mit Empfängertiefe und -material (`artifacts/receiver-depth-tests/`).
- `test-atmosphere-water-clouds.py`: kontinuierliche Wasserabtastung über 101 Vertikalphasen und zweidimensionale Mischung; Ausschluss von Boot/UI; Außenrand-, Wolken- und Nebelregressionen bestanden.
- Besucherschatten-Rastertest: 576 Kombinationen aus Sonne/Rotation/Zoom/Weichheit ohne das ursprüngliche vollständige Verschwinden. Die vorhandene Rasterabweichung der kleinen Schatten wird dadurch nicht als mathematisch null behauptet.
- Laub-, Wetter-, Gleisschatten- und Koordinatenregressionen bestanden. Unabhängige Integrationsprüfung fand keine zusätzlichen Fehler in Buffer-Lebensdauer, Worker-Reihenfolge oder Renderwegen.

Der genaue Park aus den Videos wurde nicht erneut geladen. Die gezeigte Kopplung ist im nativen Renderer reproduziert und mit dem fehlerhaften Kontrollfall abgesichert; ein visueller Vergleich im Originalpark bleibt zusätzlich sinnvoll. Performanceoptimierung bleibt zurückgestellt.
