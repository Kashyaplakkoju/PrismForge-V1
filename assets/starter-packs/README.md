# PrismForge Original Launch Filter Packs (2026-10-09)

## 1. Overview & Provenance
This delivery contains six original downloadable 3D LUT effect packs (version 1.0.0) plus one update demonstration pack (version 1.0.1) designed for the PrismForge Effects Studio runtime.

All assets (3D LUT cubes, test targets, split before/after comparison previews, and manifests) were **synthetically and mathematically generated from scratch** using original closed-form color grading formulas.
- **Zero Third-Party Images**: No copyrighted photography, stock imagery, or external LUTs were used.
- **Master Test Scene**: The included `master_scene.png` is an original synthetic test target containing calibrated color primaries, Fitzpatrick skin-tone approximations (Types I-VI), continuous neutral ramps, and an 11-step density ladder. Previews are explicitly illustrative comparisons demonstrating the transfer function of each grade.
- **License**: Released under the Apache License 2.0 for inclusion and distribution with PrismForge Studio.
- **Author**: PrismForge Effects Studio.

---

## 2. Included Pack Catalog

| Pack ID | Display Name | Version | Archive | Key Characteristics |
| :--- | :--- | :--- | :--- | :--- |
| `com.prismforge.packs.warm_portrait` | **Warm Portrait** | 1.0.0 | `archives/warm_portrait-1.0.0.zip` | Soft golden amber skin-tone enhancement with gentle midtone contrast. |
| `com.prismforge.packs.soft_daylight` | **Soft Daylight** | 1.0.0 | `archives/soft_daylight-1.0.0.zip` | 5600K daylight simulation with open shadow fill and clean neutral whites. |
| `com.prismforge.packs.clean_studio` | **Clean Studio** | 1.0.0 | `archives/clean_studio-1.0.0.zip` | Crisp commercial studio look with high micro-contrast and exact neutral gray tracking. |
| `com.prismforge.packs.cinema_teal` | **Cinema Teal** | 1.0.0 | `archives/cinema_teal-1.0.0.zip` | Cinematic shadow teal separation with warm highlight skin preservation. |
| `com.prismforge.packs.soft_mono` | **Soft Mono** | 1.0.0 | `archives/soft_mono-1.0.0.zip` | Panchromatic monochrome conversion (26% R, 65% G, 9% B) with delicate tonal graduation. |
| `com.prismforge.packs.evening_warmth` | **Evening Warmth** | 1.0.0 | `archives/evening_warmth-1.0.0.zip` | Atmospheric dusk glow with rich sunset midtones and twilight indigo shadow contrast. |

### Update Demonstration
- `update-demo/archives/warm_portrait-1.0.1.zip` (`com.prismforge.packs.warm_portrait` version 1.0.1):
  Refined grade with a +2% deep shadow lift to preserve dark detail on low-light webcams, coupled with a softened highlight shoulder to prevent clipping on pale key-lit complexions. Version 1.0.0 remains intact.

---

## 3. Technical Specifications
- **LUT Dimensions**: 17x17x17 lattice (4,913 rows).
- **Indexing Order**: Red fastest, then green, then blue.
- **Format**: Standard Adobe/Resolve `.cube` format with title, size, and domain bounds.
- **Previews**: Complete 512x512 PNG images (RGB8) with 50/50 vertical split before/after comparison.
- **Manifest**: Strict `schemaVersion` 1 JSON declaring exactly one `CubeLut` and one `PreviewImage`.
- **Integrity**: All archive members match their declared SHA-256 and byte sizes. No extraneous files.
