(function(root, factory) {
  'use strict';
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.ModelAuthoring = api;
})(typeof window !== 'undefined' ? window : globalThis, function() {
  'use strict';
  const prompt = `Create an ORIGINAL, polished 3D face mask or helmet for PrismForge's local AR filter lab.

DESIGN: [Replace this with the look, colours and materials you want.]

Deliver a real, usable binary .glb model, not a picture of a model. If you are a coding AI, write and run a reproducible Blender Python script that builds the geometry, creates or bakes materials, and exports GLB. Include the script and .blend source. If you cannot run Blender, provide the script and exact run instructions, and clearly say that the GLB has not yet been generated or tested. Never rename JSON, OBJ or an image to .glb.

Geometry and fitting:
- Use glTF 2.0, +Y up, +Z toward the viewer, +X toward the viewer's right. In Blender's standard axes use +Z up and -Y toward the viewer before the Y-up glTF export.
- Place the authored origin at the nose bridge; centre the face left/right. Use metres. A temporary neutral adult reference head around 0.16 m across the temples is an authoring guide, not part of the exported model and not a promise of universal fit.
- Build real volume and thickness. Preserve the crown, cheek and muzzle depth in front, three-quarter and profile views. Do not flatten geometry or independently stretch X/Y/Z to make it fit.
- For an open-eye mask, model two clean, modest eye openings that align with the reference eyes. Do not cut huge holes through the brows or cheeks, and do not leave disconnected texture fragments floating over the eyes. For a helmet use a separate opening or transparent visor with an opaque rim.
- Keep accessory pieces in the same head-local coordinate system. Remove the reference head, cameras, lights, floor and scenery from the exported scene.
- This import is rigid head tracking: no expression animation, jaw morphs, skeleton, or automatic personal head/hair reconstruction. Make the static design look good without those features. A full-face mesh with opaque eyes will cover the person's real eyes.

Compatibility and budget:
- One self-contained binary GLB, at most 25 MiB. Aim for at most 100,000 exported vertices AND 100,000 triangles in the complete drawn scene, counting repeated mesh instances. Prefer 20,000-50,000 triangles for a single filter.
- Embed all PNG/JPEG images in GLB buffer views. Prefer one or two texture atlases of 2048x2048 or smaller. Do not reference external URLs, files, or data URIs. UV-map textured surfaces.
- Use static triangle meshes and core glTF metallic/roughness PBR materials. Bake complex shader nodes to textures. Set sensible roughness and restrained metalness; avoid making cloth or skin mirror-like. Transparent materials need deliberate depth ordering and double-sided geometry only where necessary.
- No animation, skins/bones, morph targets, sparse accessors, Draco/meshopt compression, KTX/Basis/WebP textures, custom shaders, or glTF extensions. Apply or bake modifiers and transforms before export. Export normals and UVs; triangulate faces. Disable compression and animation on export.
- Keep a single simple scene, fewer than 256 mesh primitives and 512 nodes, and fewer than 16 images/textures. No external dependencies or executable content in the model.

Deliverables:
1. filter-name.glb and its source .blend plus reproducible build/export script.
2. Front, three-quarter, side and looking-down renders on the temporary reference head, proving eye alignment, thickness and full crown volume. Do not include the reference head in the GLB.
3. A short README with model name, creator, original/source asset provenance, exact licence or permission, texture sizes, exported vertex/triangle counts, file size, axes, origin and dimensions. Do not claim clearance for a brand or a copied character.
4. Import the exported GLB into a fresh empty Blender scene and inspect every material and view. Report actual checks and any checks you could not run.

The lab centres the complete model's bounding box and scales it uniformly to a starting width, so a clean authored origin alone does not make it fit. Check the exported GLB in PrismForge's Upload your own preview, adjust its uniform size, position and rotation, then inspect slow left/right and up/down head turns. Report remaining fit or occlusion limits honestly. Browser-preview success does not mean that native app integration or commercial distribution is complete.`;
  return Object.freeze({prompt: prompt});
});
