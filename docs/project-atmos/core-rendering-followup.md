# Kernkorrekturen vom 19.10.2026

Die Performancearbeit ist auf ausdrücklichen Wunsch zurückgestellt. Die Full-HD-Ausgangsmessung liegt unter `artifacts/performance-fullhd/baseline/`; daraus wird noch keine Optimierungs- oder 60-FPS-Zusage abgeleitet.

## Behobene Übergänge

- **Schatten und Tiefenunschärfe:** Schatten wurden bisher nach der Unschärfe aufgetragen und ab Radius drei vollständig ausgeblendet. Sie werden jetzt in einen separaten Weltfarbpuffer vor dem Tilt-Shift geschrieben und gemeinsam mit dem Boden gefiltert. Die abschließende Bildkomposition trägt sie nicht doppelt auf. Die Schatten funktionieren nun auch bei vier und acht Welteinheiten pro Pixel.
- **Fliegende Objekte:** Der aktive Schattenpfad verarbeitet ausdrücklich Personen und Fahrzeuge. Freie Ballons, Vögel und Partikel werden nicht als Personen interpretiert. Aufgehobene, fahrende und fallende Besucher erhalten keinen falschen Bodenschatten. Schattenlängen bleiben begrenzt. Der echte Renderer-Test erzeugt Ballons und fliegende Enten auf großen Höhen und prüft, dass das Schattenbild unverändert bleibt.
- **Titelmenü:** Sichtbare Titelmenü-Schaltflächen erhalten eigene Ausschlussrechtecke. Damit schützt die Maske auch Pixel, die zufällig die gleiche Farbe wie der Hintergrund haben. Der ausgeblendete Update-Bereich erzeugt keinen ungeschärften Hintergrundstreifen mehr. Logo, Optionen, Versionsanzeige und Ende-Schaltfläche bleiben geschützt.
- **Kartenrand:** Die Wolkenebene wird nicht mehr an der binären Welt-/Außenmaske abgeschnitten. Ihre Deckkraft folgt ausschließlich der bestehenden weichen Begrenzung in Kartenkoordinaten. Nebel und Bodenschatten werden weiterhin nicht auf den Außenbereich gemalt. Ein harter künstlicher Himmelshintergrund entfällt.
- **Wasser:** Der Paint-/Blit-Pfad zeichnet ein eigenes Wasserflächen-Bit und die tatsächliche Wasserhöhe auf, auch bei transparentem Wasser. Glas behält die Tiefe seines Hintergrunds. Opaque Objekte ersetzen die Wasserkennung. Wellen, Wasserdichte und Unschärfe benutzen die Geometriekennung statt wechselnder animierter Palettenindizes. Wasser mit einer dunklen Animationsfarbe wird nicht mehr versehentlich zum Kartenaußenbereich.

## Prüfung

- Release x64: Core, UI, Link und Versionsstart bestanden.
- `scripts/test-atmosphere-core.ps1` kompiliert und verlinkt gegen die tatsächlichen Core-/UI-Bibliotheken: Wasser-/Glas-/Transparenz-Verträge, Schatten von Personen und Fahrzeugen in fünf Zoomstufen, begrenzte Ausdehnung, Ausschluss von Ballons/Vögeln/aufgehobenen Besuchern sowie Erhalt von Schatten bei starkem Tilt-Shift.
- Die fünf bestehenden aktuellen Python-Regressionstests bestehen weiterhin. Der Compositortest deckt nun ausdrücklich Wolkenüberhang, UI-Schutz und vorgeschaltete Bodenschatten ab.
- Laufende EXE: Titelbild mit maximalem Tilt-Shift, kleiner Park in Zoomstufe zwei mit weichen Wolken über dem Kartenrand sowie Wasser-/Wolken-Park mit aktiven Wellen und Reflexen. In einem vollständig bedeckten 150×200-Pixel-Ausschnitt änderte sich zwischen den erfassten Bildern kein Pixel; Wasser schimmerte dort nicht durch die stationäre Wolke. Dies prüft den konkreten Ausschnitt, nicht jede denkbare Park-/Kamera-Kombination.
- Nachweise unter `artifacts/core-render-tests/`. Kein Originalspielstand überschrieben.

## Menü und Start

Die bereits vorbereitete Menüverbesserung ist enthalten: farbige Navigationsreiter, kleine Pixelicons, Gruppenabstände und eine deutliche Auswahlmarkierung. Die Optionen bleiben im eigenen Atmosphere-Fenster.

Start über `Project-Atmos/Start-Project-Atmos.cmd`. Beide konventionellen EXE-Namen innerhalb der Arbeitskopie werden beim Build synchronisiert. Die ursprüngliche EXE außerhalb von `Project-Atmos` bleibt unverändert.

Die Schatten sind weiterhin geometrische Personen-/Fahrzeugkapseln und Trackbänder, keine vollständige 3D-Silhouettenberechnung. Performanceoptimierung folgt gesondert.
