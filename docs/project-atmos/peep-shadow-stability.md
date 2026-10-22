# Peep-Schatten: zeitliche Stabilität

Der aktive `BuildObjectShadows`-Pass verwendet derzeit Kapseln für bewegliche Entitäten. Die geparkte Spritekontur-Pipeline bleibt unverändert.

Die bisherige Rasterung wertet nur einen Punkt je 2×2 Halbauflösungspixel aus. Kleine Besucher-Schatten können zwischen diesen Punkten vollständig verschwinden. Im reproduzierten Phasentest geschieht dies bei 36 von 81 Positionen. Nachträgliches Weichzeichnen kann die verlorene Abdeckung nicht wiederherstellen.

Gäste und Personal verwenden jetzt kontinuierliche Float-Projektion und 4×4 Flächenabtastung je Halbauflösungspixel. Überlagerungen werden weiterhin per Maximum vereinigt. Fahrzeuge und statische Objekte behalten ihre bisherige Rasterung. Keine neuen Zustände, Caches oder historischen Frames; Schatten werden weiter vor den Compositing-Workern vorbereitet. UI-, Außenbereich- und Empfängerbehandlung bleiben unverändert.

`scripts/test-peep-shadow-stability.py` kompiliert die tatsächlichen Helfer aus `Atmosphere.cpp`. Bestanden: 576 Sonnenwinkel-/Rotations-/Zoom-/Weichheitskombinationen mit je 256 Unterpixelpositionen, kein vollständiges Verschwinden, maximale integrierte Deckungsabweichung 10,762 %. Zusätzlich Mindestgrößen, Nullradius, punktförmige Kapseln, ungerade Bildmaße, Rand-Clipping, Schreibgrenzen, Überlagerungen, Wiederholbarkeit und kontinuierliche Projektion negativer Koordinaten geprüft.

UI-Release-x64-Build bestanden. Testartefakte: `artifacts/atmosphere-reference/peep-shadow-tests/`. Ein visueller Bewegungstest im laufenden Nutzerpark wurde nicht durchgeführt; die bestehende Tiefennachbar-Heuristik für Empfänger wurde nicht umgebaut.
