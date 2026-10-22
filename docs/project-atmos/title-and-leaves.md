# Titel, UI und Laub — 19.10.2026

## Titel und Bedienelemente

Der Code der Ursprungsversion wurde verglichen: `titleLogo`, `titleVersion` und `titleExit` waren dort ausdrücklich von rechteckigen Ausschlussmasken ausgenommen. Dieses Verhalten ist wiederhergestellt. Die tatsächlichen Sprite-/Schriftpixel bleiben geschützt, während die Aussparungen den bearbeiteten Hintergrund zeigen. Titelmenü-Schaltflächen behalten ihren vollständigen Schutz.

Das bereitgestellte `Banner-PSD.png` wurde unverändert als `resources/project-atmos/project-atmos-logo.png` übernommen. Das Original besitzt bereits einen Alphakanal. Es wird als natives G1-Sprite mit transparenten Aussparungen dargestellt, mittig auf derselben vertikalen Mittellinie wie das OpenRCT2-Logo. Die Breite beträgt nach der gewünschten Halbierung höchstens 336 Pixel; schmalere Fenster verkleinern das Banner, damit beide Logos Platz behalten. MSBuild und CMake nehmen die Ressource mit.

Sichtbare Toolbar-/Status-/Gästezahl-/Datums-/Ticker-Widgets werden vollständig geschützt. Ein reiner Farbvergleich konnte identische Welt- und UI-Palettenindizes verwechseln und dadurch Effekte auf einzelne Bedienpixel anwenden. Ausgeblendete Widgets und echte Zwischenräume bleiben ausgenommen. Das transparente Logo erhält keine pauschale Rechteckmaske.

## Laub

Jedes Blatt speichert jetzt Karten-X/Y, absolute Höhe und die Bodenhöhe seines Ursprungsbaums. Die Startposition wird aus einem tatsächlich sichtbaren Kronenpixel und seiner Szenentiefe rekonstruiert. Kamerabewegung und -rotation projizieren diese Weltposition neu. Der Fall erfolgt entlang der Höhenachse, mit deutlich geringerem Tempo von 6–11 Welteinheiten pro Sekunde und sanftem Wind/Flattern.

Vor dem Zeichnen wird jeder Blattpixel gegen die unveränderte Szenentiefe geprüft. Ein Vordergrundgebäude verdeckt ein Blatt hinter sich. Ein Parkwechsel löscht alte Partikel. Auch beim Herauszoomen läuft die Fallbewegung weiter.

Die Blätter verwenden drei Helligkeitsstufen in einer kleinen Blattkontur. Ihr Kern hat maximal 72 Prozent Deckkraft; Randtöne 46 beziehungsweise 56 Prozent. Ein- und Ausblenden reduzieren diese Werte zusätzlich. Dadurch bleiben sie erkennbar, ohne wie undurchsichtige dunkle Kästchen zu wirken.

Laub wird vor dem Tilt-Shift in die Weltfarbe eingeblendet und erhält für die Unschärfe seine eigene Tiefe. Es liegt nicht mehr als abschließende Wetterebene über dem fertigen Bild. Die Fokusmessung bleibt an der ursprünglichen Szene verankert; einzelne Partikel verstellen den Fokus nicht. Wasserreflexe werden nicht auf darüberliegende Blattpixel angewendet. Laub fordert seine notwendige Tiefenebene auch dann an, wenn andere Effekte ausgeschaltet sind.

## Prüfung und Start

- Release x64 mit Core-/UI-Build, Link und Versionsstart bestanden.
- `test-atmosphere-ui-exclusions.py`: Produktionsvisitor kompiliert; transparente Titel-Aussparungen, vollständige sichtbare Widgets, unsichtbare/ausgeblendete Elemente und Grenzen geprüft. PNG-Kopien sind bytegleich, Layout geprüft von 576 bis 3840 Pixel Breite.
- `test-atmosphere-leaves.py`: Produktionssimulation und Pixelmischung mit kontrollierter Uhr; 32 Projektions-/Höhen-/Rotations-/Zoomfälle, langsames Fallen, mindestens drei Farbtöne, Transparenz, Gebäude-/UI-Verdeckung, eigene Partikeltiefe und Parkwechsel bestanden.
- `test-atmosphere-core.ps1`: tatsächliche Renderer-Bibliotheken; erzeugtes Laub sichtbar, und aktivierter Tilt-Shift reduziert seinen Spitzenkontrast im Test von 22 auf 13. Schatten-/Wasserregressionen bestehen weiterhin.
- Weitere fünf aktuelle Python-Regressionstests bestanden. Laufbilder von Titel und dicht bebautem Park unter `artifacts/title-leaf-qa/` visuell geprüft. Die Laufbilder ergänzen die kontrollierten Tests; sie decken nicht jede mögliche Objektkombination ab.

Start über `Project-Atmos/Start-Project-Atmos.cmd`. Der Starter liest den aktiven Buildnamen aus der Arbeitskopie. Eine bereits laufende ältere EXE wird nicht beendet oder überschrieben. Die ursprüngliche Version außerhalb von `Project-Atmos` und die gelieferten Originalbilder bleiben unverändert. Performancearbeit bleibt zurückgestellt.
