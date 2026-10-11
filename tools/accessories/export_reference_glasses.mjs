// Reproduce the user's actual wired Classic Wayfarer BufferGeometry as native arrays.
// Run with the already installed reference checkout; no packages are installed.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '../..');
const reference = path.resolve(process.argv[2] ?? 'D:/glasses-tryon-v2');
const output = path.join(repo, 'windows/src/effects/overlays/reference_wayfarer_mesh.inc');
const manifestPath = path.join(here, 'reference_wayfarer_provenance.json');
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const THREE = await import(pathToFileURL(path.join(reference, 'node_modules/three/build/three.module.js')).href);
const { GlassesModel } = await import(pathToFileURL(path.join(reference, 'src/glasses/glasses-model.js')).href);
const options = {
  id: 'wayfarer', styleId: 'wayfarer', material: 'glossy_acetate',
  frameColor: '#18191c', templeColor: '#18191c', lensTint: '#1c2630', lensOpacity: 0.85,
};
const model = new GlassesModel(options);
model.group.updateMatrixWorld(true);
const vertices = [], triangles = [], components = [], normalRepairs = [];
const envelope = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };
const finite = value => {
  if (!Number.isFinite(value)) throw new Error('Non-finite reference mesh value');
  return Math.fround(value);
};
model.group.traverse(child => {
  if (!child.isMesh) return;
  const geometry = child.geometry;
  const position = geometry.getAttribute('position');
  const normal = geometry.getAttribute('normal');
  const uv = geometry.getAttribute('uv');
  if (!position || !normal || position.count !== normal.count) throw new Error('Missing or inconsistent reference geometry');
  const base = vertices.length, firstTriangle = triangles.length;
  const material = child.name === 'Lens' ? 'Lens' : child.name === 'NosePad' ? 'NosePad' : 'Frame';
  const normalMatrix = new THREE.Matrix3().getNormalMatrix(child.matrixWorld);
  for (let i = 0; i < position.count; ++i) {
    const p = new THREE.Vector3().fromBufferAttribute(position, i).applyMatrix4(child.matrixWorld);
    const n = new THREE.Vector3().fromBufferAttribute(normal, i).applyMatrix3(normalMatrix).normalize();
    if (n.lengthSq() < 1e-12) {
      // ShapeGeometry leaves an unconnected duplicate closing lens endpoint with
      // a zero normal. Use the coincident surface vertex normal, preserving the
      // reference positions, topology and visible geometry exactly.
      let coincident = -1;
      for (let j = 0; j < position.count; ++j) {
        if (position.getX(j) !== position.getX(i) || position.getY(j) !== position.getY(i) || position.getZ(j) !== position.getZ(i)) continue;
        const candidate = new THREE.Vector3().fromBufferAttribute(normal, j).applyMatrix3(normalMatrix).normalize();
        if (candidate.lengthSq() > 0.99) { n.copy(candidate); coincident = j; break; }
      }
      if (coincident < 0) throw new Error('Reference zero normal has no coincident valid surface normal');
      normalRepairs.push({component: child.name, componentOrdinal: components.length, vertex: i, copiedNormalFromCoincidentVertex: coincident});
    }
    const values = [p.x, p.y, p.z, n.x, n.y, n.z, uv ? uv.getX(i) : 0, uv ? uv.getY(i) : 0].map(finite);
    for (let j = 0; j < 3; ++j) { envelope.min[j] = Math.min(envelope.min[j], values[j]); envelope.max[j] = Math.max(envelope.max[j], values[j]); }
    vertices.push(values);
  }
  const index = geometry.getIndex();
  const count = index ? index.count : position.count;
  if (count % 3 !== 0) throw new Error('Non-triangular reference geometry');
  for (let i = 0; i < count; i += 3) {
    const indices = [0, 1, 2].map(j => index ? index.getX(i + j) : i + j);
    if (!indices.every(v => Number.isInteger(v) && v >= 0 && v < position.count)) throw new Error('Invalid reference mesh index');
    triangles.push([...indices.map(v => base + v), material]);
  }
  components.push({
    name: child.name, material, firstVertex: base, vertexCount: position.count,
    firstTriangle, triangleCount: triangles.length - firstTriangle,
    materialProperties: {
      color: `#${child.material.color.getHexString()}`, opacity: child.material.opacity,
      roughness: child.material.roughness, metalness: child.material.metalness,
      clearcoat: child.material.clearcoat, transparent: child.material.transparent,
      depthWrite: child.material.depthWrite,
    },
  });
});
model.dispose();
if (vertices.length > 20000 || triangles.length * 3 > 180000) throw new Error('Reference mesh exceeds native bounded geometry contract');
const floatLiteral = v => {
  const value = Object.is(v, -0) ? 0 : v;
  const s = value.toPrecision(9).replace(/(\.\d*?)0+(?=e|$)/, '$1').replace(/\.(?=e|$)/, '.0');
  return `${s}f`;
};
const content = [
  '// Generated by tools/accessories/export_reference_glasses.mjs.',
  '// Exact Classic Wayfarer from the user-provided glasses-tryon-v2 GlassesModel.',
  '// Includes transformed curved rims/lenses, hinges, temples, nose pads and bridge.',
  '// See tools/accessories/reference_wayfarer_provenance.json. Do not edit by hand.',
  'static constexpr MeshVertex kReferenceWayfarerVertices[] = {',
  ...vertices.map(v => ` {${v.map(floatLiteral).join(',')}},`),
  '};',
  'static constexpr MeshTriangle kReferenceWayfarerTriangles[] = {',
  ...triangles.map(t => ` {${t[0]}u,${t[1]}u,${t[2]}u,Material::${t[3]}},`),
  '};', '',
].join('\n');
fs.writeFileSync(output, content, 'utf8');
const sources = [
  'src/glasses/glasses-model.js', 'src/glasses/pose-estimator.js', 'src/processing/storage.js',
  'src/rendering/scene-manager.js', 'src/app.js', 'package.json', 'package-lock.json',
  'node_modules/three/package.json', 'node_modules/three/build/three.module.js',
  'node_modules/three/build/three.core.js',
].map(file => ({ path: file, sha256: digest(path.join(reference, file)) }));
const manifest = {
  schemaVersion: 1, referenceRoot: reference.replaceAll('\\', '/'),
  source: 'User-provided local glasses-tryon-v2; actual default wired Classic Wayfarer GlassesModel',
  exportScript: { path: 'tools/accessories/export_reference_glasses.mjs', sha256: digest(fileURLToPath(import.meta.url)) },
  options, frameScaleCentimeters: 14, actualLensOpacity: 0.8,
  vertices: vertices.length, triangles: triangles.length, envelope, components, normalRepairs, sources,
  output: { path: 'windows/src/effects/overlays/reference_wayfarer_mesh.inc', sha256: digest(output) },
  scope: 'Geometry is exact. Native lighting/material implementation is separate from the original Three.js MeshPhysicalMaterial renderer.',
};
fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + '\n', 'utf8');
console.log(JSON.stringify({ output: manifest.output, vertices: vertices.length, triangles: triangles.length, envelope, components: components.map(c => ({name:c.name,vertices:c.vertexCount,triangles:c.triangleCount})) }, null, 2));