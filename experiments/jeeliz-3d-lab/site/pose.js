(function(root, factory) {
  'use strict';
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.LabPose = api;
})(typeof window !== 'undefined' ? window : globalThis, function() {
  'use strict';
  const THRESHOLD_ON = 0.82;
  const THRESHOLD_OFF = 0.78;
  function problem(state) {
    if (!state || typeof state !== 'object' || Array.isArray(state)) return 'No single-face tracker object';
    for (const key of ['detected','x','y','s','rx','ry','rz']) {
      if (typeof state[key] !== 'number') return 'Tracker field '+key+' is '+typeof state[key];
      if (!Number.isFinite(state[key])) return 'Tracker field '+key+' is not finite';
    }
    if (state.s <= 0.01 || state.s > 1) return 'Tracker scale s='+state.s.toFixed(3)+' is outside 0.01..1';
    if (state.detected < 0 || state.detected > 1) return 'Tracker confidence='+state.detected.toFixed(3)+' is outside 0..1';
    return '';
  }
  // Jeeliz's neural-network confidence is a score and can slightly overshoot
  // [0, 1] (observed 1.002 on the live input). Clamp only a finite score in a
  // fresh object; never change the detector's reused pose object or repair NaN.
  function normalize(rawState) {
    const state = Array.isArray(rawState) && rawState.length === 1 ? rawState[0] : rawState;
    if (!state || typeof state !== 'object' || Array.isArray(state) ||
        typeof state.detected !== 'number' || !Number.isFinite(state.detected)) return state;
    return Object.assign({}, state, {detected: Math.max(0, Math.min(1, state.detected))});
  }
  function valid(state) { return problem(state) === ''; }
  function visibility(wasVisible, state) {
    if (!valid(state)) return false;
    return wasVisible ? state.detected >= THRESHOLD_OFF : state.detected > THRESHOLD_ON;
  }
  // Same camera/pivot geometry as the pinned JeelizThreeHelper for an uncropped 4:3 video.
  // Render-only pose checks use this path; live tracking uses the upstream helper itself.
  function apply(THREE, object, state, camera) {
    if (!valid(state)) { object.visible = false; return false; }
    const tangent = Math.tan(camera.aspect * camera.fov * Math.PI / 360);
    const distance = 1 / (2 * state.s * tangent) + 0.5;
    object.rotation.set(state.rx, state.ry, state.rz, 'ZYX');
    object.position.set(-Math.sin(state.rz) * 0.2, -Math.cos(state.rz) * 0.2, -0.6);
    object.position.applyEuler(object.rotation);
    object.position.add(new THREE.Vector3(state.x * distance * tangent,
      state.y * distance * tangent / camera.aspect + 0.2, -distance + 0.6));
    object.updateMatrixWorld(true);
    return true;
  }
  return Object.freeze({valid:valid,normalize:normalize,problem:problem,visibility:visibility,apply:apply,
    detectionThresholdOn:THRESHOLD_ON,detectionThresholdOff:THRESHOLD_OFF});
});
