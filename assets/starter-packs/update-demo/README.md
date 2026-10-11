# Warm Portrait Pack Update Demonstration (v1.0.0 -> v1.0.1)

## Purpose
This folder demonstrates a versioned pack upgrade (`com.prismforge.packs.warm_portrait` updating from version `1.0.0` to `1.0.1`) while maintaining the original 1.0.0 release intact in `deliveries/launch-packs-20261009/archives/`.

## Grade Refinement Details
- **v1.0.0 Baseline**: Established the initial golden amber warmth and gentle S-curve contrast for studio portraiture.
- **v1.0.1 Modifications**:
  1. **Shadow Luminance Lift**: Introduced a calibrated +2.4% luminance lift in the deep shadow band (luminance < 0.25) to avoid crushed shadow detail on budget webcams with limited dynamic range.
  2. **Highlight Shoulder Rolloff**: Softened the compression knee above 0.70 luminance by ~3% to protect high-key specular highlights on fair skin tones.
  3. **Anchoring**: Pure black `(0,0,0)` and pure white `(1,1,1)` remain strictly anchored.

## Files
- `packs/warm_portrait/`: Unpacked updated directory pack.
- `archives/warm_portrait-1.0.1.zip`: Downloadable ZIP archive containing `manifest.json`, `preview.png`, and `luts/warm_portrait.cube`.
