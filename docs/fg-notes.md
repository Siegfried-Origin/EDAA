# DLSS Frame Generation in Elite (DX11) – technische Notizen

## Aufbau (addon/src/fg_present.cpp, fg.hpp, Hooks in passes.cpp)
1. **D3D12-Ausgabe:** Bei `reshade_present` (nach ReShade-Effekten und Overlay) wird das fertige
   D3D11-Bild in eine geteilte Textur kopiert (NT-Handle, D3D11 erzeugt, D3D12 öffnet; geteilter
   Fence wie im XeSS-Pfad). Ein D3D12-Flip-Swapchain auf einem Kindfenster über dem Spielfenster
   zeigt sie an. Die DXGI-Factory kommt aus **ReShades `dxgi.dll`** (`fg_factory=1`); mit der
   System-dxgi stürzte Present ab (ReShade-gewrappte Queue). ReShades zweite Effekt-Runtime auf
   dem Kindfenster wird mit `set_effects_state(false)` stillgelegt.
2. **Streamline 2.14.16:** `slInit` vor dem D3D12-Device; Device und Factory per
   `slUpgradeInterface` (eUseDXGIFactoryProxy, eUseFrameBasedResourceTagging); DLLs zur Laufzeit
   per GetProcAddress aus dem Add-on-Ordner. Reflex (eLowLatency + `frameLimitUs`), PCL-Marker,
   DLSS-G eOn, 1 generiertes Bild.
3. **Eingaben:** In `resolve()` (passes.cpp) werden Tiefe (R32F-Kopie) und EDAA-Bewegungsvektoren
   (RG16F, prev−cur UV, ohne Jitter, mvecScale 1) an `fg::capture` gegeben, in **zwei**
   abwechselnde geteilte Sätze kopiert und bei Present mit `eOnlyValidNow` auf der eigenen
   Command-List getaggt. Kameradaten: Reverse-Z unendlich, clipToPrevClip aus EDAA-Matrizen.
4. **HUD-Schutz** (`fg_hud_protect`): EDAAs HUD-Maske (Szene nach AA vs. Szene am Bloom) wird auch
   für FG gebaut; bei Present setzt `ps_fg_motion` die Vektoren auf HUD-Pixeln auf 0.
   Funktioniert voll nur mit `taa_before_hud=1`.
   Beim Hochskalieren (Supersampling < 1) nutzt FG die Maske, die EDAA für DLSS baut (`upscale_hud_mask`);
   bis 08.11. wurde sie dort nicht an FG weitergegeben (Overlay: „no HUD found this frame“).
5. **Overlay:** Abschnitt „Frame Generation“ im EDAA-Tab (An/Aus live, HUD-Schutz,
   Frame-Limit live, Status echt→angezeigt, Reflex, HUD).

## Behobene Fehler (chronologisch)
| Symptom | Ursache | Fix |
|---|---|---|
| Absturz beim ersten D3D12-Present | System-dxgi + ReShade-gewrappte Queue | Factory aus ReShades dxgi |
| Starkes Ruckeln im Menü | DLSS-G jedes Frame an/aus (fehlende Eingaben) | Hysterese, letzte Eingaben weiterverwenden |
| Ruckler alle ~2 s im Flug | Zweite ReShade-Runtime setzte „keine Eingaben nötig“ | D3D12-Runtime im reshade_present ignorieren |
| Unruhiger Takt mit framepacer | Zwei Limiter (Reflex-Sleep + externer) | Reflex-Limiter (`fg_fps_limit`) |
| 70 statt 140 FPS | Reflex-Limit gilt mit FG für **angezeigte** Bilder | Limit 140 = 70 echte |
| GPU-Hänger/Absturz an Stationen | GPU-seitiges Warten D3D11↔D3D12 | nur noch begrenzte CPU-Waits |
| An/Aus-Ruckler nach dem Fix | Übersprungene Eingaben zählten als „keine Szene“ | getrennte Zähler, 8-ms-CPU-Wait |
| ~2,5 % übersprungene Bilder | Ein Eingabesatz, Freigabe erst nach Present | doppelte Puffer, Freigabe nach Command-List |
| Absturz in `gameoverlayrenderer64.dll+0x9c130` (Lesezugriff 0xFFFF…) | DLSS-G präsentiert vom eigenen Pacer-Thread, gleichzeitig Elite vom Render-Thread → Steam-Overlay-Hook nicht threadsicher | Presents serialisiert (timed_mutex, `present`/`finish_present`) – in Bestätigung |
| Bild und Eingaben eingefroren nach „Reset to defaults“ oder „Present through D3D11 again…“ bei laufender FG (Ereignis 1002) | `fg_present_test=0` stoppte die D3D12-Ausgabe sofort, das Kindfenster blieb mit dem letzten Bild über dem Spiel, DLSS-G blieb an | Ausgabe läuft bis Spielende weiter, DLSS-G geht über den erprobten Aus-Pfad aus; D3D11 erst beim nächsten Start – in Bestätigung |

Nicht geholfen: Umgebungsvariable `SteamNoOverlayUIDrawing=1` (Steam Input braucht das Overlay ohnehin).

## Log-Meldungen (ReShade.log)
- `EDAA FG: DLSS-G state at frame N: status 0x0, frames presented 2, frames without a scene X, reset R, focus F, busy skips B`
  – presented 2 = Zwischenbild aktiv; focus 0 = Spiel im Hintergrund (DLSS-G pausiert absichtlich);
  frames without a scene = Menü/Karte/Laden; busy skips sollte 0 sein.
- `EDAA FG: DLSS-G on/off` – sollte selten sein (Start, Menüs, Laden).
- `EDAA status: …` – jede Statusänderung (z. B. „waiting for two frames with a main camera“ = Kameraschnitt).
- `Frame rate over 100.00ms` (Streamline) – Hänger/Laden/Fokuswechsel; dauerhaft = GPU überlastet.
- `Wait on gpu fence timed out` (Streamline) – GPU hängt → Absturz folgt.
- sl.log: `Override Reported: ModelPreset, Value: 3` = FG-Preset C über NVIDIA App aktiv.
- `EDAA GPU: scene X | frame … | motion … | fg_in … | hud … | dlss … | taa … | fg_out … | edaa … | elite … | N frames, M skipped`
  – GPU-Zeiten (Overlay „GPU timings“ → Measure, alle 10 s, Mittel + P99); `EDAA GPU: scene X` = Szenenwechsel
  mit Strg+Umschalt+N (–, A…E). Siehe Abschnitt GPU-Zeiten.

## GPU-Zeiten (addon/src/gpu_profiler.cpp, Phase 1, 09.11.)
- D3D11-Timestamp-Queries auf der Spiel-Queue, Ring aus 6 Frame-Sätzen, Lesen ohne Warten
  (`D3D11_ASYNC_GETDATA_DONOTFLUSH`); nicht fertige Sätze werden verworfen und als „skipped“ gezählt.
- Abschnitte: `motion` (Bewegungsvektoren), `fg_in` (Tiefenkopie, Capture, HUD-Motion), `hud` (Snapshot + Maske),
  `dlss` (DLSS/DLAA/XeSS inkl. Kopien), `taa` (TAA/TAAU), `fg_out` (Kopie des Backbuffers für D3D12).
  `frame` = Present bis Present auf der GPU-Zeitachse **inkl. Leerlauf**; `elite` = frame − EDAA (Spiel, andere Add-ons, Leerlauf).
- Nicht messbar: die DLSS-G-Erzeugung auf der D3D12-Queue.
- **GITC Uplift (DLSS 5) steckt in `dlss`**: Uplift rechnet NR „before upscaling“ auf eigenem D3D12-Gerät, und die
  D3D11-Queue wartet darauf (Uplift-Design, nicht EDAA). Messung 09.11. Szene D: dlss 3,02 ms mit NR, 0,97 ms ohne → NR ≈ 2,05 ms.
- Erstmessung 09.11. (DLSS-Hochskalierung Modell J, FG an, Limit 140): Szenen A–D alle 14,45 ms = 69,2 echte fps
  = das Reflex-Limit (140 / 2). Die GPU ist dabei **nicht ausgelastet**; EDAA gesamt 3,0–3,1 ms (davon NR ~2 ms),
  alle anderen EDAA-Passes zusammen ~0,12 ms. Ohne Fokus (kein DLSS-G, kein Limit) ~9 ms pro echtem Frame.
  Messkosten: nicht messbar (echte fps mit/ohne gleich), 0 verworfene Frames.

## Reaktivmaske für DLSS/XeSS (passes.cpp, frame_tracking.cpp, Phase 3, 09.11.)
- Einstellungen `dlss_reactive_mask` (Standard aus), `dlss_reactive_strength` (1,0), `dlss_reactive_threshold` (0,75);
  Overlay unter „DLSS model“, dazu „Show reactive mask (yellow)“ (Debug-Ansicht 8, nicht gespeichert).
- Erkennung nur über den Pipeline-Zustand: Draw ins Szenenbild, Blending an, keine Tiefe schreiben, Tiefentest
  reverse-Z vorwärts (`GREATER`/`GREATER_EQUAL`), Szenentiefe gebunden, kein Fullscreen-Dreieck/-Quad. Vor dem ersten
  solchen Draw des Frames wird das Szenenbild kopiert; am Auflösungspunkt: Maske = Anteil, den diese Draws geändert
  haben, über der Schwelle, mal Stärke (`ps_reactive`). DLSS bekommt `max(HUD-Maske, Reaktivmaske)` als
  `pInBiasCurrentColorMask` (eigene Textur, die HUD-Maske für den FG-HUD-Schutz bleibt unverändert).
- Fehlversuche: „erster Blend-Draw nach einem 4-Vertex-Quad“ markierte das ganze Bild – im Hangar gibt es pro Frame nur
  ein solches Quad, und es liegt **vor** den Lichtvolumen (Sonnen-/Grundlicht). Lichtvolumen (Blend, keine Tiefe,
  `LESS_EQUAL` auf Rückseiten) fallen erst über den Tiefentest heraus.
- Schwelle: 0,3–0,55 nahm den Hangar-Dunst mit (ganze Decke/Hinterwand), 0,75 lässt Hologramme, Lichtkegel und Dampf.
- Messung 09.11.: Erkennung in 100 % der Frames (Hangar und Flug), 43–156 durchsichtige Draws pro Frame,
  GPU-Kosten 0,04–0,07 ms. Optisch im Test „sieht gut aus“ (kein Aus/An-Vergleich im Log).
- Log alle 700 Frames: `EDAA reactive: strength … | transparents found X/Y frames | snapshot … | mask built … | transparent draws per frame …`

## Leistung (RTX 4070 Ti, 1440p)
- Supersampling 1.0: ~68–69 echte → ~137–140 angezeigte FPS.
- Supersampling 1.25: Stationen ~57 echte; 2.0 (5120×2880): ~3–6 echte – unbrauchbar.
- Checkerboard-Terrain verursacht flimmernde Linienmuster mit TAA/FG → aus.

## Grenzen
- DLSS-G-Presets lassen sich in Streamline 2.14.16 / DLSS-SDK 310.9.16 nicht per API setzen.
- Dynamic Multi Frame Generation nur auf RTX 50.
- Ein generisches FG für beliebige DX11-Spiele braucht pro Spiel Bewegungsvektoren.

## Phase 4: Cockpit-HUD finden und ruhig halten (10.11.)
- **Befund (Trace mit gelesenen Texturen, Hangar, DLSS-Hochskalierung 0,65):** Elite rendert das Cockpit-HUD in eigene
  Panel-Texturen (R8G8B8A8 mit D24S8-Tiefe, z. B. 2048×440, 1466×1200, 682×1022, 1365×853, 410×448) und blendet sie
  **vor dem Resolve** mit der Hauptkamera in die HDR-Szene, alle mit **einem** Pixelshader (`2babfda9d1708376`, Panel
  in t2). EDAA kennt nur `b4523c39a461fd35`; der kommt im Test-Setup nie vor (Vermutung: EDHM ersetzt die HUD-Shader).
  Folge bisher: Resolve „before bloom (HUD included)“, HUD-Maske leer → `taa_before_hud`, DLSS-HUD-Maske und
  FG-HUD-Schutz wirkungslos, Panels bei Bewegung doppelt/verwischt (auch ohne FG).
- `hud_learn` (Overlay „Find the cockpit HUD by its panel textures“, Standard aus): merkt sich pro Bild die
  Panel-Ziele (Farbe R8G8B8A8 + Tiefe R24G8/D24S8); der erste Draw in die Szene, der so eine Textur liest, macht
  seinen Pixelshader zum HUD-Shader (Log „cockpit HUD shader found by its panel textures“). Gesucht wird nur, solange
  im letzten Bild kein HUD-Shader lief. Danach greifen Resolve vor dem HUD, HUD-Maske und FG-HUD-Schutz wieder
  (Maske ~14–17 % des Bildes im Cockpit).
- `hud_motion` (Overlay „Cockpit HUD moves with the cockpit“, Standard aus, nur beim Hochskalieren): Die Panels
  schreiben keine Tiefe und bekamen die Bewegung der Welt dahinter. `ps_hud_motion` schreibt vor DLSS auf
  HUD-Masken-Pixeln die Fahrzeugbewegung (wie `ps_motion` für Cockpit-Pixel, Abstand 1,5 m); FG übernimmt sie
  (`ps_fg_motion_keep` statt Nullsetzen). Ergebnis im Test: Panels bei Kopf-/Schiffsbewegung scharf.
  Kosten zusammen mit der Maske 0,04–0,06 ms.
- **Verworfen: NVIDIA-UI-Recomposition** (HUD-loses Bild + `UIColorAndAlpha`). Gebaut und getestet, wieder entfernt:
  Ein HUD-loses Bild gibt es in Elite nicht (Panels stecken vor Bloom/Tonemap/DLSS in der Szene), und die Panels
  bewegen sich mit dem Kopf – DLSS-G legt das UI des echten Bildes über das Zwischenbild → Doppelbilder bei
  Bewegung. Kostete 0,3–0,7 ms (ReShade-Filter per `render_effects` vorgezogen).
- Der Elite-Hänger beim Beenden (09.11., `D3D12 side still busy`) trat am 10.11. mit allen Schaltern an nicht auf.
- Frame-Trace (Entwickler-Build): loggt nach dem Resolve alle Draws (mit `display=` und `ds=`), indirekte Draws,
  Kopien und die Pixelshader-Texturen t0–t7 (`tN=handle BxH`).

## Resolve ohne Bloom-Pass (10.11.)

- Befund: Bei Supersampling 1,5 mit `BloomQuality=1` wurde der Bloom-Downsample-Shader `b196cbbdf516e150` nie geladen
  (Log ohne „bloom downsample shader found“; ob die niedrige Bloom-Stufe oder EDHM die Ursache ist, ist offen).
  Mit `taa_before_hud=0` war „vor Bloom“ der einzige Resolve-Punkt: „resolve point not reached … bloom no“, kein TAA/DLSS,
  keine FG-Eingaben, DLSS 5 (Uplift) stand auf „Waiting for DLSS“.
- Rückfall (`check_resolve_point`): Ist die Option aus und im letzten Bild kein Bloom-Pass gezeichnet worden, gelten die
  Punkte vor dem HUD. Punktname „before the HUD pass (no bloom pass found)“, einmal eine Warnung im Log, Hinweis im Overlay
  unter der Option. Kommt der Bloom-Pass wieder, gilt automatisch wieder „vor Bloom“.
- FG-HUD-Maske (`check_hud_mask`): wurde nur am Bloom-Pass gebaut, ohne ihn zeigte das Overlay „HUD protection: no HUD found
  this frame“. Ohne Bloom-Pass im letzten Bild jetzt beim ersten Draw in ein Ziel in Bildschirmgröße nach dem Resolve
  (die Szene enthält dann das HUD). Beim Hochskalieren baut `run_upscale` die Maske wie bisher selbst.
