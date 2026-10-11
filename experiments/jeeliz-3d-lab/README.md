# PrismForge local 3D filter lab

A working local preview with seven 3D filters: Rupy motorcycle helmet, Anonymous mask, dog ears/nose/animated tongue, Tiger mask, Werewolf mask, Orbit astronaut helmet and Prism cyber mask. Orbit and Prism use original mathematical geometry; Tiger and Werewolf use pinned official Jeeliz demo assets. This experiment does not publish frames to the native PrismForge app or its virtual camera. The desktop camera stream is an input to this page.

## Run

Use the already installed Python3. Double-click Start.cmd, then open http://127.0.0.1:8743/ if the browser does not open. Select a system camera or PrismForge Virtual Camera and press Start camera. The camera starts only after that action. If the camera is busy in another lab tab, press Stop there first. Stop on the page releases its owned camera capture; Stop.cmd stops only this lab's server. Source/filter changes require Stop first. Reload after a tracker cleanup/startup timeout before restarting.

The model motion button renders the actual geometry without acquiring a camera. Its motion and mouth controls are synthetic, not face detection. Live tracking uses the pinned upstream helper with one detected face and its mouth-opening coefficient. Dog tongue and Tiger jaw respond to mouth opening. Werewolf is a static textured mesh that turns with your head; it has no expression animation. Mouth controls appear only on the two animated filters. Mirror affects both raw video and overlay together.

No network download is required at runtime. Server binds only127.0.0.1 and serves only site/. It rejects foreign origins/hosts, path escapes and control requests without its per-process token. Local .runtime state is not part of Git or packages. No microphones, other app shutdown, driver registration or global installation is needed.

## Corrections and fitting

Jeeliz sometimes returns a confidence score just above1 (observed1.002). A finite score is normalized in a new object before visibility evaluation; invalid pose/nonfinite fields still hide the rig. Raw camera video continues independently of tracker rendering. Lost/invalid/stale tracking clears the overlay instead of leaving the last mask. This fixes the reported one-or-two-frame filter appearance.

head-attachment.js defines validated attachment parents in the tracked head coordinate system. The dog ear crown parent moves the upstream ears back to the head plane; the muzzle parent positions nose and tongue toward the nose plane. Authored vertex data and uniform scales are unchanged. A depth-only copy of the pinned head proxy hides rear dog surfaces during turns. The helmet retains its own authored calibration. A future asset needs its own measured attachment profile; a default offset cannot fit every model. Jeeliz's rigid pose does not supply dense landmarks, a personal hair/head scan or hand occlusion.

## Upload your own and AI creation brief

Press Stop, then Choose GLB model under Upload your own. Use a static binary glTF2 file with embedded PNG/JPEG textures and ordinary PBR materials. The file stays in this browser session, clears on reload and never goes to a remote service. The imported model becomes Your model in the selector. Model motion and the live camera share the same importer/rig; the former does not open a camera.

Fit your model provides uniform size, XYZ head-local offsets and three rotations in degrees. The importer centres the complete bounding box and uses a uniform face-sized starting fit (1.2head-localunits wide). Exact SHA256 fingerprints of the three supplied cap files select their original forehead-band anchors,180degree facing correction where needed, and a1.4unit width; renamed unrelated files do not inherit those profiles. Reset restores the selected starting fit; this cannot automatically identify the nose bridge or turn a full character into a face accessory. Adjust every new asset while checking its eye/crown seating. The optional approximate head depth proxy hides rear surfaces; it does not reconstruct personal hair or make a model wrap around it. Remove/replacement is available only when stopped. Failed imports retain the previous model, and cancelled loads cannot install a stale model or overlap another import.

Copy AI model prompt copies the full practical brief. It also exposes a readable manual-copy fallback when clipboard permission is unavailable. The creation steps and prompt explain actual mesh generation/export through Blender or a model tool, geometry/material limits, axes and source/provenance delivery. See the parent docs/3D_FILTER_AUTHORING.md for where all seven existing looks came from and a process for new models. A compatible SketchfabGLB can be previewed; licensing/bundling approval remains model-specific.

Hard bounds:25MiB file,2MiB metadata,150k drawn vertices and150k triangles including instances,256primitives,512nodes,16images/textures,4096px image sides/48MiPixels total and6million declared accessor components. Index scans are cached across repeated primitives. External resources, compression, animation, skeletons, morphs and sparse geometry are rejected. Every textured primitive requires material UV0; integer UVs must be normalized. Unused extra UVchannels0-7 are allowed. Cached resources are bounded by one uploaded model; replacing/removing it disposes owned materials, textures and geometry. Import is static head tracking, not expression animation.

The tiger repair restores the original demo's2,3,2calibration: the earlier uniform2shortened the intended silhouette. Smaller smoothly edged frontal eye openings replace the video-blending cutout copied from upstream. The mouth cavity is recessed and rounded. Original raw model/texture bytes stay pinned.

## Verified scope

On2026-10-09 the Codex in-app browser rendered the helmet on the real USB-fed PrismForge source at different head orientations and video times10.96 and44.41seconds, with100% normalized tracking. The owner called the mask nice and confirmed Yes, the fit is better after the dog attachment/depth correction and left/right live check. This is bounded live fit evidence, not a sustained FPS, latency, arbitrary head-pose, hand occlusion or receiving-app benchmark.

Current upload/tiger automated verification:804 direct verifier checks;39 exact pinned assets;52 actual HTTP files;20 child commands all exit0. Children include20 baseline geometry/pose groups,20 wildlife groups,18 original-rig groups,16 collection routing/resource groups,50 mocked lifecycle groups,27 realGLTF-loader groups with fixture image decoding and16 real local-server tests. Lifecycle tests explicitly mark their camera/renderer as fake. The120-frame regression reproduces score1.002 continuously without hiding or mutating the upstream detector object. Dog geometry rays confirm rear occlusion at both yaw signs and9 physical pose transforms preserve model dimensions.

To rerun (existing Node.js needed for verification only):

    python tools/verify_lab.py
    node tools/test_rig.js
    node tools/test_lifecycle.js
    node tools/test_wildlife.js
    node tools/test_original_rigs.js
    node tools/test_collection.js
    node tools/test_local_model.js

A custom --receipt path requires an existing parent directory. The verifier uses a temporary loopback server and does not acquire a camera. Current upload evidence is in the parent docs/JEELIZ_CUSTOM_MODELS.md and docs/JEELIZ_CUSTOM_MODELS_VERIFICATION.json. Earlier expanded-collection evidence stays unchanged in docs/JEELIZ_FILTER_COLLECTION.md and docs/JEELIZ_FILTER_COLLECTION_VERIFICATION.json. The earlier three-filter receipt remains historical.

## Sources and limits

Jeeliz pin6f2695120b992511fd6cb1fd80c600bb957cd08c; Three.jsr97. Exact source URLs, sizes and SHA256 are site/assets-manifest.json. Upstream demo source and full Apache2/MIT notices are preserved. See site/NOTICES.txt. Original Rupy branding remains; no model/brand commercial clearance is claimed. Face swap is deferred. Browser-preview output integration, launch-cleared original3D models, native lifecycle/latency and commercial release verification remain separate work.
