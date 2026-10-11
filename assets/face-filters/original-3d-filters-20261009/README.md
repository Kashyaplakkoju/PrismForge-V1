# PrismForge Original 3D Face Filters Collection (2026-10-09)

This delivery provides three original, polished 3D accessory filters authored specifically for the PrismForge local AR face filter lab and native accessory pipeline:

1. **Aurora fox mask** (`models/aurora-fox-mask.glb`)
2. **Nocturne owl mask** (`models/nocturne-owl-mask.glb`)
3. **Atlas explorer helmet** (`models/atlas-explorer-helmet.glb`)

All models are static, self-contained binary glTF 2.0 (`.glb`) triangle meshes with embedded PBR metallic/roughness textures, clean material UV0 coordinates, and calibrated fitting profiles.

---

## 1. Specifications & Budget Summary

| Model | Drawn Triangles | Drawn Vertices | Primitives | Materials | Embedded Textures | File Size | Bounding Dimensions (W x H x D) |
|---|---|---|---|---|---|---|---|
| **Aurora fox mask** | 22,800 | 11,418 | 1 | 1 (Opaque PBR) | 2 (2048x2048 PNG) | 709.5 KiB | 207.1 x 228.9 x 92.2 mm |
| **Nocturne owl mask** | 21,656 | 10,854 | 1 | 1 (Opaque PBR) | 2 (2048x2048 PNG) | 1,126.4 KiB | 257.3 x 236.0 x 88.4 mm |
| **Atlas explorer helmet** | 38,328 | 19,279 | 2 | 2 (Opaque + Blend) | 2 (2048x2048 PNG) | 905.2 KiB | 292.2 x 280.3 x 268.2 mm |

- **Authoring Budget Ceiling**: <= 100,000 vertices & triangles, <= 25 MiB, <= 2 texture atlases <= 2048px, <= 256 primitives, <= 512 nodes, <= 16 textures. (All models stay well within the recommended 20k–50k target).
- **Importer Hard Limit**: 150,000 drawn vertices & triangles, 4096px / 48 MiPixels max.
- **Extensions**: Zero custom or unsupported extensions. (Strictly glTF core 2.0).
- **Compression**: No Draco, meshopt, Basis, or KTX compression.
- **Animation / Bones**: Zero animations, skins, or morph targets (rigid head pose).

---

## 2. Design & Modeling Details

### 1. Aurora Fox Mask (`aurora-fox-mask.glb`)
- **Aesthetic**: Traditional mythical kitsune / arctic aurora spirit mask.
- **Cheeks**: Sculpted curved cheek guards with elegant outward aerodynamic flare (expanding to X = ±103.6 mm).
- **Muzzle & Nose**: Shaped snout originating at the nose bridge, tapering forward to a defined triangular charcoal leather nose pad with nostril indentations.
- **Eyes**: Clean, symmetric almond eye cutouts (centered at X = ±32 mm, Z = +8 mm) matching human pupil spacing, framed by beveled border rims.
- **Ears**: Tall pointed triangular fox ears anchored at the lateral cranium sides, rising to Z = +188.7 mm, featuring sculpted 3D inner fur tuft geometry and beveled outer rims.
- **Shell**: Solid ceramic shell thickness of 3.5 mm throughout, preventing paper-thin or inside-out artifacts in profile.
- **Materials**: Glazed porcelain white base (`#F6F8FB`), traditional ceremonial vermilion red painted markings (`#C41A1A`), gold leaf trim (`#D4AF37`), and charcoal nose pad (`#202126`).

### 2. Nocturne Owl Mask (`nocturne-owl-mask.glb`)
- **Aesthetic**: Nocturnal raptor / horned owl face mask.
- **Facial Discs**: Twin concentric feather disc shells radiating outward from each eye cavity with 6 stepped plumage tiers.
- **Brow & Plumicorns**: Sweeping curved raptor brow ridge arching over each orbital cavity and transitioning into tall horned ear tufts (plumicorns) extending to Z = +166.3 mm.
- **Beak**: Pronounced hooked raptor beak anchored below the brow, projecting strongly forward and curving downward past the philtrum (Z = -69.7 mm), distinctly separated from eye openings with deep undercuts.
- **Eyes**: Modest circular-almond eye openings surrounded by radiating feather rings.
- **Shell**: Solid 3.5 mm shell thickness with beveled perimeter rim.
- **Materials**: Midnight obsidian slate base (`#1C1F26`), warm bronze feather shafts (`#7D623C`), amber-gold stippling (`#BFA054`), and polished metallic bronze beak (`#544432`).

### 3. Atlas Explorer Helmet (`atlas-explorer-helmet.glb`)
- **Aesthetic**: Deep space EVA / planetary surface exploration helmet.
- **Crown & Rear Volume**: Full rounded spherical/ovoid outer crown enclosing the entire cranium, ears, and occiput (rear depth extending to Y = -185.5 mm in glTF coords). Head volume is completely preserved at a 35-degree downward tilt.
- **Visor Window**: Wide panoramic viewport framed by a heavy structural rubber gasket with 12 perimeter clamp bolts.
- **Visor Glass**: Tinted protective polycarbonate bubble visor utilizing core glTF `alphaMode: "BLEND"` (opacity 0.35, roughness 0.06), allowing clear view of the wearer's face and eyes while exhibiting realistic glass reflections.
- **Fittings**: Lateral communications pods on temples, dual lower cheek atmospheric rebreather regulator canisters with corrugated ribbed conduits connecting to the neck collar.
- **Collar Ring**: Heavy industrial docking neck flange at the base with locking lugs.
- **Materials**: Matte aerospace white composite (`#E2E6EC`), graphite grey structural panels (`#30343C`), anodized titanium bronze canisters (`#706454`), hazard amber accents (`#D97E24`), and rubber seals (`#18191C`).

---

## 3. Coordinate System & Calibration

- **Convention**: glTF 2.0 standard: `+Y` Up, `+Z` Toward Viewer, `+X` Right.
- **Authored Origin**: Nose bridge at `(0.0, 0.0, 0.0)` meters.
- **Authoring Guide Head**: Neutral adult reference head with ~0.16m (160mm) temple width, 64mm interpupillary distance. (Saved separately in `sources/neutral-reference-head.blend`, never included in accessory GLBs).

### Starting Fit Parameters (Post Lab Bounding-Box Normalization)

The browser lab centers the complete model's bounding box and applies uniform horizontal scaling (`normalize = 2.0 / size.x`). The calibrated starting fit `{scale, x, y, z, rx, ry, rz, occlusion}` restores exact physical alignment:

```json
{
  "aurora-fox-mask": {
    "scale": 0.62, "x": 0.0, "y": 0.28, "z": 0.42,
    "rx": 0.0, "ry": 0.0, "rz": 0.0, "occlusion": true
  },
  "nocturne-owl-mask": {
    "scale": 0.64, "x": 0.0, "y": 0.20, "z": 0.44,
    "rx": 0.0, "ry": 0.0, "rz": 0.0, "occlusion": true
  },
  "atlas-explorer-helmet": {
    "scale": 0.68, "x": 0.0, "y": 0.02, "z": 0.32,
    "rx": 0.0, "ry": 0.0, "rz": 0.0, "occlusion": true
  }
}
```

---

## 4. Visual Verification Proofs

Every model has been verified by reopening the exact exported `.glb` in a fresh Blender scene with the reference head and rendering 5 camera views plus an explanatory fitting diagram:

- `front.png`: Frontal view (0° yaw, 0° pitch) verifying eye cutout alignment.
- `yaw_pos45.png`: +45° yaw (three-quarter view) verifying cheek flare and ear profile.
- `yaw_neg45.png`: -45° yaw (three-quarter view) verifying symmetry and thickness.
- `profile.png`: 90° profile view verifying muzzle/beak projection and crown depth.
- `tilt_down35.png`: 35° downward tilt verifying solid crown and occiput volume.
- `fitting_axes_dimensions.png`: Explanatory diagram showing coordinate axes (+X, +Y, +Z) and millimeter dimensions.

---

## 5. Verification Tools

Run the automated verification suite from the repo root:

```bash
python deliveries/original-3d-filters-20261009/tools/verify_delivery.py
```

This runner automatically performs:
1. Oracle reference checksum verification.
2. Fresh build regeneration in `build/original-3d-filters-20261009/test_build`.
3. Executable compatibility audit using the unchanged `local-model.js` preflight oracle.
4. Independent geometry, index, float finiteness, and bounds checks.
5. Negative rejection testing (verifies `local-model.js` rejects malformed models with unsupported extensions and external URIs).
6. Verification of all 18 rendered proof files.

---

## 6. Attribution & Provenance Records

- **Creator**: Antigravity IDE (PrismForge Studio asset engineering).
- **Provenance**: 100% original procedural geometry and custom PBR textures.
- **Third-Party Clearances**: No external third-party models, downloaded meshes, trademarked brands, or copyrighted characters were used or recreated.
- **Licence**: Proprietary - PrismForge Studio internal production asset collection.
