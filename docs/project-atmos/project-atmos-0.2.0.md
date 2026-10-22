# Project:Atmos 0.2.0 — Rendering-Korrekturen

Arbeitsversion vom 19.10.2026. Start über `Start-Project-Atmos.cmd`: verwendet EXE, Spieldaten und Benutzerprofil innerhalb dieser Version. Die Ursprungsversion im Stammordner bleibt erhalten. Der Doppelpunkt ist Bestandteil des Projektnamens; Windows benötigt für das Verzeichnis `Project-Atmos`.

## Änderungen und Zuständigkeiten

- Schattenempfänger werden beim tatsächlichen Zeichnen der sichtbaren Sprites markiert. Bewegte Nachbarpixel entscheiden nicht mehr darüber, ob ein Bodenpixel Schatten empfängt. UI, aufrechte Sprites und Außenflächen bleiben geschützt. Die vorhandene Subpixelrasterung wird auch für Fahrzeuge und Schienen benutzt.
- Trackschatten folgen den nativen Fahrzeug-Bewegungstabellen einschließlich Kurven, Steigungen und Loopings. Räumlich benachbarte, aber unverbundene Trackstücke werden nicht mehr willkürlich verbunden. Pro Trackelement wird nur Sequenz null ausgewertet. Fahrzeugschatten sind breiter als die schmalen Schienenbänder und werden pro Frame aus den aktuellen Entity-Positionen erzeugt.
- Wetter begrenzt automatische Wolkenbedeckung auf 60 Prozent; höhere Reglerwerte sind eine ausdrückliche Wahl. 100 Prozent ermöglicht eine geschlossene Decke. Weniger zusätzlicher Schnee-/Blizzardnebel und keine versteckte Opazitätserhöhung. Bestehende gespeicherte hohe Reglerwerte bleiben erhalten.
- Blitze erhalten einen hellen weißen Kern und stärkere lokale Aufhellung umliegender Wolken.
- Wolkenrandbegrenzung benutzt die tatsächliche Höhe der Wolkenebene. Strahlenmuster, Ausdehnung und Abtasthöhen sind in Kartenkoordinaten verankert. Die Parkdaten werden vor den parallelen Renderjobs aktualisiert; ein bisheriger Schreibzugriff aus mehreren Ray-Workern entfällt.
- Sonnenstrahlen werden am niedrigen Sonnenstand länger und schwächer; bei höherer Sonne kürzer und kräftiger. Unterhalb der Dämmerung verschwinden sie weich.
- Lichtobjekte bleiben in Karten-XY plus Höhe über Terrain gespeichert. Beim Anfassen einer Lampe bleibt der Griffversatz erhalten; beim Verschieben über Hänge bleibt ihre absolute Höhe stabil. Freie Platzierung ist der neue Standard. Bereits ausdrücklich gespeicherte Rastereinstellungen bleiben auswählbar.
- Wasser mit fehlender Tiefeninformation wird weiterhin von Wolken bedeckt. Wolken und Nebel werden auch bei Wasserreflexen mit ihrer gemeinsamen Durchlässigkeit berücksichtigt, einschließlich reinem Nebel.

## Prüfung

- Release x64: Core, UI, Link und `--version` erfolgreich; `scripts/build-project-atmos.ps1` reproduziert den Build innerhalb der Kopie.
- Fünf gezielte Testprogramme kompilieren Produktionscode: 1280 Wolkenprojektionen, 2424 Bedeckungsfälle, 192 Koordinaten-Rundläufe, 16 Lampenverschiebungen, 576 Schatten-Stabilitätssweeps und 40 native Trackpfade mit 1188 Segmenten. Zusätzlich UI-/Außenflächenschutz, Nebel/Wasser, beide Compositorpfade und Unabhängigkeit von animierten Nachbartiefen.
- Laufende EXE mit isoliertem Testprofil und kopiertem `galtest.park`: helle Blitze, Kamerarotation und geschlossene Wolkendecke über Wasser geprüft. Der Park benötigt teilweise RCT1-Ressourcen; dabei meldet das Spiel vorhandene Ersatzbilder. Keine Originalspielstände gespeichert.
- Weitere Laufproben mit den mitgelieferten Car-Ride-, Ferris-Wheel- und Blackpool-Parks. Blackpool lief mit 2768 Besuchern und dicht übereinanderliegenden Tracks. Bildausgaben und Test-/Buildprotokolle unter `artifacts/project-atmos/`; diese Einzelbilder ersetzen keinen vollständigen visuellen Animationstest aller Perspektiven.
- Hashvergleich bestätigt 11637 unveränderte Dateien der Ursprungsversion (Quellen, Ressourcen, Dokumentation und Builddefinitionen).

## Grenzen

Die Schatten bleiben eine geometrische Näherung: Schienenbänder und Fahrzeug-/Personenkapseln, keine vollständigen dreidimensionalen Sprite-Silhouetten. Tracktypen ohne gültige native Bewegungstabelle erhalten keinen erfundenen Verbindungs-Schatten. Bestehende Entfernungslimits bleiben erhalten. Die Prüfungen decken gezielte Fälle ab; sie sind keine Garantie für jede Kombination aus Fremdobjekten, Terrain und Animation.

Die unabhängigen älteren Skripte `test-atmosphere-compositor.ps1`, `test-atmosphere-lights.ps1` und `test-atmosphere-shadows.ps1` beziehen sich auf die früher stillgelegte Silhouettenpipeline. Maßgeblich für diese Version sind die fünf neuen bzw. aktualisierten Python-Tests und die genannten Laufproben.
