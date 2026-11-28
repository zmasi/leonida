# Map viewer

Local WebGL / Three.js map viewer for exported world chunks (`world.json` + `.glb`) and
synthetic test terrain. No game data is stored in this directory.

## Files

| File | Role |
|---|---|
| `viewer/serve.py` | Standard-library local HTTP server serving `viewer/index.html` and an optional `--world` folder at `/world/` |
| `viewer/index.html` | Free-camera chunk-streaming map viewer with synthetic test-terrain fallback |
| `viewer/studio.html` | Standalone 3D model and texture gallery page served by `tools/gallery.py` (not by `viewer/serve.py`) |

## Run the map viewer

Start on the default port (`8765`) with synthetic test terrain (no exported world or game data
needed):

```powershell
python -I viewer/serve.py
```

Serve an exported world directory built outside the repository by `tools/world_build.py`:

```powershell
python -I viewer/serve.py --world <work>\world
```

Pass `--port 0` to ask the OS for a free port (any port in `0`–`65535` is accepted; the bound
URL is printed on stdout):

```powershell
python -I viewer/serve.py --port 0
```

Stop the server with `Ctrl+C`.

### Loopback binding and request guards

`viewer/serve.py` binds exclusively to `127.0.0.1` (`ViewerServer`, with `allow_reuse_address`
disabled on Windows so a second instance fails instead of shadowing an active server). Every
request checks that:

- the `Host` header is absent or names a loopback host (`127.0.0.1`, `localhost`, or `[::1]`
  with an optional valid port; foreign hosts return `403` to block DNS rebinding, and duplicate
  `Host` headers return `400`);
- outside `/world/`, only `/` and `/index.html` are served (`404` for everything else);
- `/world/...` paths resolve strictly inside `--world` via `safe_world_path`, rejecting
  traversal (`..`), escaping symlinks, directories, drive/stream colons, NUL bytes, and
  Windows reserved device names.

Responses set `Cache-Control: no-store` and map `.glb` to `model/gltf-binary`.

### Synthetic test terrain and controls

When `--world` is omitted, `world/world.json` is missing, malformed, or times out (10 s), or
`?test` is added to the URL, `viewer/index.html` falls back to a deterministic 24×24 grid of
256 m procedural terrain chunks generated entirely in the browser.

Click the canvas to capture the pointer (falling back to left-drag look when pointer lock is
unavailable). The on-screen help overlay lists movement, speed modifiers, scroll-wheel speed
adjustment, and view toggles (`F` wireframe, `T` textures/clay, `G` chunk bounds, `R` return
to spawn, `P` copy the `#cam=...` view URL, `Esc` release mouse).

## Three.js CDN dependency

`viewer/index.html` loads Three.js (`three@0.160.0`: `three.module.js`, `GLTFLoader.js`, and
`BufferGeometryUtils.js`) from `cdn.jsdelivr.net` via an import map and matching
`modulepreload` links. Every remote URL is pinned to exact version `0.160.0` and carries a
`sha384` Subresource Integrity hash (`crossorigin="anonymous"`), so the browser needs network
access to `cdn.jsdelivr.net` on first load while world files stay local to `127.0.0.1`.
Bumping the version requires updating the URLs and `sha384` digests in both the import map and
the `modulepreload` tags; `tests/test_viewer_cdn_integrity.py` checks that pairing.

## Tests

All viewer tests run without a browser, network access, or game data:

```powershell
node --test tests/test_viewer_streaming.cjs
node --test tests/test_viewer_studio.cjs
python -I tests/test_viewer_serve.py
python -I tests/test_viewer_cdn_integrity.py
```

`tests/test_viewer_streaming.cjs` (Node.js 22+) exercises manifest validation, test-terrain
fallback, LOD band selection, chunk streaming/retries, and GPU resource disposal directly
against `viewer/index.html`.
