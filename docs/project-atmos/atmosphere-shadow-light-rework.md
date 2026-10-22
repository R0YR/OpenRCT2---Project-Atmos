# Schatten und Objektlichter – Revision vom 11.10.2026

Diese Revision ersetzt das zuvor abgelehnte Volumenmodell vollständig. Schatten stammen aus den tatsächlich gezeichneten, undurchsichtigen Spritekonturen. Zusammenhängende Pixel-Flächen werden projiziert, vereinigt und weichgezeichnet; Überlappungen verdunkeln nicht mehrfach. Die Deckkraft bleibt begrenzt. Fußwege, ihre Geländer und Stützen sind bereits im Painter ausdrücklich von Schattenquellen ausgeschlossen. Separate Wegmöbel bleiben Objekte. Der Slider Shadow softness regelt die Kanten unabhängig von Stärke und Länge.

Lichtquellen stammen aus Weglampen-Metadaten, konservativ erkannten Lampen-/Laternen-Objektnamen und tatsächlichen Fahrzeug-Entitäten. Dekorative Autos bekommen keine Scheinwerfer. Bewegliche Mine-Train-, Eisenbahn-, Monorail-, Boots-, Auto- und Kart-Fahrzeuge erhalten vorn einen gerichteten Lichtkegel; hinten am letzten Wagen zwei rote Rücklichter. Tag-/Nachtstärke, Hauptstärke, Scheinwerfer, Rücklichter und Radius haben eigene englische Menüregler. Lampen mit transparenten Sprite-Spitzen bleiben durch nahe Tiefen-/Bodenprüfungen sichtbar. Die Lichtverdeckung ist eine konservative Näherung.

Gewitter bekommen kurze, ausblendende Szenenblitze. Lightning und Flash interval (s) regeln Intensität und Abstand. Normales Regen-/Schneewetter erzeugt keine Blitze. Die globale Disable-lightning-Einstellung wird respektiert. Atmosphere unterdrückt den zusätzlichen Vanilla-Palettenblitz und hält UI sowie schwarzen Hintergrund unverändert. Der lokale Effekt benutzt eigene Zeit/Zufallswerte, ohne den Simulations-RNG oder Wetterzustand zu verändern.

Das Preset Soft clouds and sunlight aktiviert die dezenten Schatten und Objektlichter. Everything off entfernt auch die neuen Licht-/Blitzeffekte. Einstellungen werden gespeichert und geladen.

## Prüfungen

Release-x64-Core und UI bauen; die neue bin/openrct2.exe wurde erfolgreich gelinkt und gestartet. Die Sandbox verlangt weiterhin /Z7 statt /Zi sowie /DEBUG:NONE beim Linken; Release-Optimierungen bleiben aktiv.

- test-atmosphere-shadows.ps1: tatsächlicher BMP/RLE-Code, Transparenz, Glas, Zoom, überschriebenes Casterpixel, Weg-/Stützenausschluss, dünne Sortierboxen, 320 Kombinationen aus Kameraversatz/Zoom/Drehung/Sonne, vereinigte Quad-Abdeckung und parallele Blur-Zeilen.
- test-atmosphere-lights.ps1: vorderer Kegel, hintere rote Lichtquellen, Tag/Nacht/Aus, Transparenz an Lampenspitzen, Vordergrundverdeckung, UI/Hintergrund und Farbkanal-Anordnungen.
- test-atmosphere-compositor.ps1: Speicherung/Aus, Gewitter-Timing/Neuladen, explizite Empfänger, UI/Hintergrund und ungerade Bildgrößen.
- Wolkenprüfungen bestehen weiterhin.

Vorher-/Nachheraufnahmen und Build-/Laufzeitlogs: artifacts/shadows-lights-rework. Der Testpark enthält RCT1-Objekte, die diese Installation durch Fallback-Sprites ersetzt.

## Grenzen

Schatten stammen aus sichtbaren Spriteflächen, nicht aus verdeckter/offscreen 3D-Geometrie. Sie fallen auf Boden und Wege; erhöhte Dächer werden nicht rekonstruiert. Speziallampen ohne erkennbare Objektnamen benötigen zusätzliche Metadaten. Exakte Lichtstrahl-Verdeckung wird nicht behauptet.

Leistung: die neue Schattenstufe kostet im nahen, wenig bebauten 1280x720-Test etwa 1 ms. In der dichten herausgezoomten Ansicht wurden vor der abschließenden Rasteroptimierung etwa 38 ms gemessen. Dichte Ansichten bleiben deshalb die wichtigste Leistungsgrenze; der Build/Funktionserfolg ist kein allgemeines Performance-PASS.
