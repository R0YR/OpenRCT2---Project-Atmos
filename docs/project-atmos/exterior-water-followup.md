# Kartenrand, Ballonspuren und Wasser — 19.10.2026

## Ursachen und Änderungen

Die bisherige Randerkennung suchte vom Bildschirmrand aus nach ununterbrochenen dunklen Pixelzeilen. Ein Ballon unterbrach diese Suche. Dadurch wurde der ganze Bereich zwischen Ballon und Karte anders eingefärbt, vernebelt und unscharf gezeichnet. Das erzeugte horizontale Licht-/Dunkelstreifen und rechteckige Begrenzungen. Die Klassifikation erfolgt jetzt je Pixel: native leere Kartensprites tragen `kExteriorSurface`; unbekannter leerer Hintergrund wird separat erkannt. Dunkle Gebäude, Wasser und UI bleiben unterscheidbar.

Die bereits vorhandene native Oberflächemaske wurde vom X8-Renderer überhaupt nicht angelegt. Wasser- und Schattenempfängerkennzeichen erreichten deshalb den eigentlichen Bildlauf nicht, obwohl isolierte Rastertests funktionierten. Die Maske wird jetzt zusammen mit der Tiefe angelegt, bei Größenänderungen neu aufgebaut und beim Scrollen mitgeführt. Die weiterhin deaktivierte Silhouetten-Höhenebene bleibt optional; Scrollen greift ohne sie nicht auf leere Speicherbereiche zu.

Unbekannte Tiefenwerte verwenden einen festen Zeilenradius. Sie übernehmen keine Unschärfe mehr von einzelnen fliegenden Objekten oder unterschiedlichen Worker-Abschnitten. Nebel läuft wie Wolken über den Kartenrand hinaus und wird durch die räumliche Ausblendung begrenzt. Die Wolken-Cachekennung berücksichtigt auch die Kartengröße. Sonnenstrahlen erhalten eine sanfte Weltkoordinaten-Modulation entlang ihrer Länge, um gleichförmige Bänder zu verringern; ihre Richtung und Kameraprojektion bleiben erhalten. Strahlen und Tilt-Shift ohne Tiefenfokus fordern ebenfalls die native Maske an.

## Verifikation

- Release x64 gebaut und Versionsstart erfolgreich; aktiver Build `Project-Atmos-edge-fix.exe`.
- `test-atmosphere-core.ps1`: tatsächlicher X8-Renderer prüft Anlegen, Scrollen ohne Silhouettenhöhe, Resize und Abschalten. Tatsächlicher Wolken-/Nebel-/Tilt-Compositor prüft, dass ein eingesetztes und wieder entferntes Flugobjekt keine entfernten Pixel verändert oder Spuren hinterlässt. Wasser-/Glas-Blits, Gäste-/Fahrzeugschatten und Laub bestehen weiterhin.
- `test-atmosphere-water-clouds.py`: Produktionsfunktionen prüfen Ballon-unabhängige Masken bei 31/256/1919 Pixel Breite, dunkle Szenerie, Wasser, UI, abschnittsunabhängige Unschärfe sowie Wolken und Nebel über Wasser und Außenflächen.
- Native Scrollroutine zusätzlich mit 242 überlappenden Verschiebungen geprüft, mit und ohne Höhenebene (`artifacts/exterior-surface-tests/`).
- Projektions-, Wetter-, UI- und Laubregressionen bestanden.
- Laufansicht des kleinen Parkfixtures mit Wolken, Nebel, Strahlen, Schatten und 35 Prozent Tilt-Shift unter `artifacts/edge-fix-qa/` visuell geprüft: keine rechteckig abgeschnittene Atmosphäre. Der genaue Park aus den Nutzerbildern wurde damit nicht nachgestellt.
- 11.637 Dateien der Ursprungsversion per SHA-256 unverändert bestätigt. Nutzerprofil bleibt erhalten.

Start über `Project-Atmos/Start-Project-Atmos.cmd`; ältere laufende Spielinstanzen müssen für die neue Version geschlossen werden. Performanceoptimierung bleibt zurückgestellt.

## Nachkorrektur: sichtbare Außenkacheln

Die vorherige Änderung vereinheitlichte die Klassifikation, behielt aber die ursprünglichen Palettenwerte jeder Außenkachel bei. Native Blanktiles verwenden Index 10; gelöschte oder wettergefilterte Außenpixel können andere Indizes besitzen. Dadurch wurde ihre geometrische Begrenzung unter transparenter Atmosphäre sichtbar. Zusätzlich konnte die farbbasierte Erkennung gefilterte, unbemalte Pixel übersehen und sie fälschlich als Wolkenschattenempfänger behandeln.

Alle Außenpixel beginnen jetzt mit derselben Hintergrundfarbe (Palette 10). Bei vorhandener Szenentiefe entscheidet fehlende native Abdeckung statt des gefilterten Farbwerts über unbemalten Hintergrund. Wasserflags, bekannte Szenerietiefe und UI behalten ihre Priorität. Wolken und sanfter Nebel dürfen weiterhin über den Rand ziehen; Boden-/Wolkenschatten werden außerhalb nicht angewandt.

Der erweiterte native Compositortest mischt echte Außenflags mit gelöschten und künstlich wettergefilterten Hintergrundindizes in einer großen Fläche. Das gesamte Ergebnis muss trotz aktiver Wolken, Nebel, Schatten und Tilt-Shift exakt dem einheitlichen Hintergrund entsprechen. Zusätzlich bleiben Ballonspuren-, Wasser-, Scroll- und Laubtests erfolgreich. Eine Laufansicht mit einer Kopie der aktuellen Nutzereinstellungen liegt in `artifacts/exterior-clean-qa/`; das eigentliche Nutzerprofil bleibt unverändert. Der spezifische Wasserpark aus dem Nutzerbild wurde nicht geladen.

Release-Build und Versionsstart bestanden. Neuer aktiver Build: `Project-Atmos-exterior-clean.exe`.
