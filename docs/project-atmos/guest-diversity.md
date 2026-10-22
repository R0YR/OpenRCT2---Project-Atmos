# Besucher-Varianten — 15.10.2026

Der aktuelle lokale Build ergänzt sechs visuelle Besucher-Varianten. Normale Peeps bleiben erhalten. Die beiden älteren Modelle sind entfernt; vorhandene Spielstände mit diesen Objektkennungen zeigen stattdessen originale Besucher.

| Variante | Erscheinung |
| --- | --- |
| Frau, Bob | blonde Bobfrisur, ärmelloses Oberteil, Rock |
| Frau, Pferdeschwanz | braune Haare mit Haarband, Streifenshirt, Shorts |
| Frau, lange Haare | dunkle lange Haare, Bluse mit hellem Kragen, Rock |
| Frau, Locken | deutlich größere, voluminöse dunkle Locken, einfarbiges Kleid |
| Kind, Mädchen | kleiner Körper, Pferdeschwanz, Rock |
| Kind, Junge | kleiner Körper, kurze Haare, Shorts |

Neue Gäste erhalten beim Erzeugen eine dauerhaft gespeicherte Variante. Die Gewichtung ist Original 4, jede neue Variante 1: etwa 40 % Original, 40 % erwachsene Frauen und 20 % Kinder. Bestehende aktive Varianten bleiben erhalten; entfernte ältere Varianten werden wieder normal dargestellt. Kleidungsfarben verwenden weiter die vorhandene zufällige Farbauswahl. Körperformen und Kleidung werden in der nativen indizierten Palette beim Objektladen erstellt, einschließlich aller ursprünglichen Aktionen und Gegenstandsgruppen. Kinderkörper und Hut/Ballon/Regenschirm nutzen dieselbe Skalierung um den Weltursprung; der Painter braucht keinen zusätzlichen Effektpass.

Behobene Importursache: Die ursprünglichen Varianten hatten `legacyPosition` vom Original geerbt und überschrieben dadurch die Konvertierungstabelle alter Parks. Das zuletzt geladene ältere Modell konnte sämtliche importierten Besucher ersetzen. Neue Pakete beanspruchen keine Legacy-Positionen mehr; der Loader unterbindet dies zusätzlich für alte eingebettete Atmosphere-Pakete. Die beiden entfernten Kennungen bleiben nur als normale, nicht zufällig ausgewählte Kompatibilitätsobjekte lesbar. Originale Parks werden nicht verändert.

## Zustand und Integration

- Die Besucherentität besitzt `animationObjectIndex`, Animationsgruppe und Kleidungsfarben. Es gibt keinen zusätzlichen globalen Besucherzustand und keine Änderung des Save-Formats.
- Der Objektmanager ergänzt verfügbare Pakete vor der Simulation. Gespeicherte Objektplätze bleiben erhalten; fehlende optionale Pakete werden gemeldet. Benutzerdefinierte andere Basis-Besucherobjekte bleiben auf ihrer bisherigen Basis.
- Spawn-Auswahl verwendet den synchronisierten Szenario-RNG und eine feste Variantenreihenfolge. Ohne zusätzliche Varianten wird kein weiterer Zufallswert verbraucht.
- Netzwerkclients ergänzen keine lokalen Objekte; sie verwenden die Objektplätze des Servers. Die Netzwerkkennung endet auf `-atmosphere-guests-2`, um Builds mit anderer Spawn-Simulation abzuweisen.
- `.parkobj`-Pakete sind als Custom-Objekte klassifiziert und können mit Parks bzw. Netzwerk-Karten übertragen werden. Sie referenzieren die installierten RCT2-Sprites; originale Spielgrafiken werden nicht in diesem Quellbaum mitgeliefert.
- Spritegrenzen neuer Varianten berücksichtigen sämtliche Richtungen und Frames. Unveränderte Besucherobjekte behalten die bisherige Behandlung.

Quellpakete: `resources/guest-diversity/objects/`. Lesbare JSON-Dateien und reproduzierbare ZIP-Pakete werden mit `scripts/generate-guest-diversity.py` erzeugt. Windows-Build und CMake-Installation kopieren ausschließlich `.parkobj` nach `data/object/atmosphere/peep_animations/`. Im lokalen `bin/data/object/` sind sie installiert. Änderungen am Erscheinungsalgorithmus oder den Paketen erfordern eine neue Objektversion bzw. passende Netzwerkkennung, bevor inkompatible Builds miteinander verwendet werden.

## Prüfung

Release-x64-Core und UI erfolgreich gebaut. Die bisherige `bin/openrct2.exe` ließ sich beim Linken nicht öffnen; die korrigierte Version wurde deshalb als `bin/openrct2-guests-update.exe` gelinkt und mit `--version` gestartet (Netzwerkkennung v2 bestätigt). Diese Startdatei verwenden. Windows-Build verwendet weiterhin `/Z7` und `/DEBUG:NONE` wie in der Atmosphere-Übergabe beschrieben. Keine Commits, Resets oder Änderungen an vorhandenen Parks.

`scripts/test-guest-diversity.py`: kompiliert den echten Transformationscode mit MSVC-Debugprüfungen. Originalidentität, sichtbar größere Lockenkrone, Remapfarben, Transparenz, Kinderanker, inverse Koordinatenskalierung, leere/ungültige und kleine Teil-Sprites bestanden. Sechs aktive und zwei gewöhnliche Kompatibilitätsobjekte einschließlich byteidentischer `.parkobj`-Inhalte und fehlender Legacy-Positionen geprüft.

`scripts/test-guest-diversity-integration.ps1 -Rct2Path '<RCT2-Spielordner>'`: echter Spielkern mit isoliertem Benutzerverzeichnis und Testpark. Alte v1.0-Objekte erscheinen pixelgleich zum Original und bleiben vom Spawn ausgeschlossen; Legacy-Konvertierung mit geladenen Varianten erhält das Original. Sparse-Slots, wiederholtes Laden, deterministische 1.200 Auswahlen (494 Originale), alle Animationsgruppen/Aktionen/Richtungen, Kinderskalierung und Zubehör bestanden. 121 Besucher behielten ihre Erscheinungen beim `.park`-Roundtrip im Speicher. Sechs eingebettete Archiv-Payloads wurden bytegenau geprüft und in ein unabhängig leeres Objektrepository übertragen und erfolgreich geladen.

Logs und native Vorschauen: `artifacts/guest-diversity/`. `scripts/preview-guest-diversity.py` erzeugt `guest-models.png` aus den tatsächlich gerenderten Sprites. Geprüft wurden vier Richtungen und sechs Gehframes je Variante sowie Sitzen, Karte, Essen und Hut.

## Grenzen

Dies ergänzt Erscheinungsbilder. Alter, Geschlecht, Familien, Bedürfnisse, Namen und Fahrgeschäftszulassung erhalten keine neue Simulationslogik. Fahrgeschäfte, Minigolf und Spiralrutsche verwenden weiterhin ihre eigenen Fahrgastbilder. Ein vollständiger Multiplayer-Lauf mit mehreren Prozessen wurde nicht durchgeführt; deterministische Auswahl, Versionsabgrenzung und tatsächliche Objektübertragung sind gezielt geprüft. Es wurde kein allgemeiner Performance-Benchmark durchgeführt.
