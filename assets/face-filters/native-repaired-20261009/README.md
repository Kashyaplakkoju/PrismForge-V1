# Native revised owl asset

Original frozen GLB and procedural sources remain in ../original-3d-filters-20261009.
This native revision corrects the artwork calibration: source UV maps place actual
apertures at X +/-0.038m,Y +0.008m, near atlas pixels739/1309,Y1374. The original
texture painted extra dark eye cavities at640/1408,Y1000, away from real apertures.
Both base and linear metallic-roughness maps now use the correct pupil centres.
Closed horn-base triangle fans also prevent hollow underside views. No anisotropic
deformation or bound-centering is applied. Rebuild textures then the Blender source.

The fox source uses original unchanged geometry but corrected artwork centres:
eye atlas726/1322,Y1617; forehead crestY1152; nosepadY1895-2005. The eye-to-UV
calibration follows original source-mesh X/Y bounds and original exported UVs.
Native loader never recentres the mesh or deforms its authored proportions.
The original mannequin's eye socket positions differ from its claimed bridge
anchor; these front renders validate artwork alignment, not live wearer fitting.
