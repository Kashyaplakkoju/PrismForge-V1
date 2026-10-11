(function() {
  'use strict';
  const byId=function(id){return document.getElementById(id);};
  const ui={start:byId('startCamera'),stop:byId('stopCamera'),demo:byId('demoMode'),
    source:byId('cameraSelect'),refresh:byId('refreshCameras'),filter:byId('filterSelect'),
    status:byId('status'),confidence:byId('confidence'),error:byId('error'),
    placeholder:byId('placeholder'),demoControls:byId('demoControls'),
    mirror:byId('mirror'),yaw:byId('demoYaw'),mouth:byId('demoMouth'),
    auto:byId('autoMotion'),loss:byId('simulateLoss'),video:byId('sourceVideo')};
  let generation=0,mode='idle',stream=null,sceneStuff=null,rig=null,camera=null;
  let animationFrame=0,trackerStarted=false,trackerReady=false,lastStatusAt=0,restartBlocked=false,lastTrackingValid=true;
  let modelPromise=null,trackingWatchdog=0,lastTrackAt=0;
  const rigPromises=new Map();
  let customAsset=null,customHead=null,importBusy=false;
  const modelUI={file:byId('modelFile'),upload:byId('uploadModel'),remove:byId('removeModel'),
    info:byId('modelInfo'),fit:byId('modelFit'),reset:byId('resetFit'),occlusion:byId('fitOcclusion')};
  const fitFields={scale:byId('fitScale'),x:byId('fitX'),y:byId('fitY'),z:byId('fitZ'),
    rx:byId('fitRX'),ry:byId('fitRY'),rz:byId('fitRZ')};
  const defaultFit={scale:1,x:0,y:0,z:0,rx:0,ry:0,rz:0,occlusion:true};
  let modelFit=Object.assign({},defaultFit);
  function modelControls(idle) {
    modelUI.upload.disabled=!idle;modelUI.file.disabled=!idle;
    modelUI.remove.disabled=!idle||!customAsset;
    modelUI.fit.hidden=!customAsset||ui.filter.value!=='custom';
  }
  function applyFit() {
    const next=Object.assign({},modelFit);
    Object.keys(fitFields).forEach(function(key){
      const input=fitFields[key];
      if(input.value.trim()==='')return;
      const number=Number(input.value),min=key==='scale'?10:key.length===2?-180:-3;
      const max=key==='scale'?300:key.length===2?180:3;
      if(Number.isFinite(number))next[key]=Math.max(min,Math.min(max,number))/(key==='scale'?100:1);
    });
    next.occlusion=modelUI.occlusion.checked;modelFit=next;
    if(rig&&ui.filter.value==='custom'&&rig.updateFit)rig.updateFit(modelFit);
  }
  function resetFit() {
    modelFit=Object.assign({},defaultFit,customAsset&&customAsset.initialFit);
    Object.keys(fitFields).forEach(function(key){const value=modelFit[key]*(key==='scale'?100:1);fitFields[key].value=String(Math.round(value*1000)/1000);});
    modelUI.occlusion.checked=!!modelFit.occlusion;
    if(rig&&ui.filter.value==='custom'&&rig.updateFit)rig.updateFit(modelFit);
  }
  function createRig(type,loaded,reference) {
    if(type!=='custom')return FilterRigs.create(THREE,type,loaded.data,loaded.maps,reference);
    const imported=loaded.custom.createRig(modelFit),outer=new THREE.Group();
    const proxy=HeadAttachment.occluder(THREE,customHead,HeadAttachment.profiles.dogHead);
    outer.add(proxy.group);outer.add(imported.group);proxy.group.visible=modelFit.occlusion;
    let disposed=false;
    return {group:outer,hasMouth:false,setMouth:function(){},updateFit:function(fit){
      imported.updateFit(fit);proxy.group.visible=!!fit.occlusion;
    },dispose:function(){if(disposed)return;disposed=true;imported.dispose();
      proxy.mesh.geometry.dispose();proxy.mesh.material.dispose();}};
  }
  function customOption() {
    return Array.from(ui.filter.options).find(function(option){return option.value==='custom';});
  }
  async function importModel() {
    if((mode!=='idle'&&mode!=='error')||importBusy)return;
    const file=modelUI.file.files&&modelUI.file.files[0];if(!file)return;
    const current=++generation;mode='importing';importBusy=true;clearError();controls();
    modelUI.info.textContent='Checking and loading '+file.name+'...';status('Loading your local 3D model...',false);
    let loaded=null;
    try{
      loaded=await LocalModel.loadFile(THREE,file);
      if(!customHead){
        const response=await fetch('assets/helmet/face.json');
        if(!response.ok)throw new Error('The local head occlusion model is missing.');
        customHead=await response.json();
      }
      if(current!==generation){loaded.dispose();return;}
      if(customAsset)customAsset.dispose();customAsset=loaded;loaded=null;
      let option=customOption();
      if(!option){option=new Option('Your model','custom');ui.filter.appendChild(option);}
      option.textContent='Your model: '+customAsset.name;ui.filter.value='custom';resetFit();
      const stats=customAsset.stats;
      modelUI.info.textContent=customAsset.name+' / '+stats.triangles.toLocaleString()+' triangles / '+stats.textures+' textures. '+(customAsset.fitName||'Adjust the starting fit')+'. Local preview only.';
      mode='idle';status('Model ready. Try model motion, or start your camera.',false);
    }catch(error){
      if(loaded)loaded.dispose();
      if(current===generation){
        mode='idle';ui.error.textContent=describe(error);ui.error.hidden=false;
        modelUI.info.textContent=customAsset?'Kept your previous model: '+customAsset.name:'No custom model loaded.';
        status('Model could not be imported. '+describe(error),false);
      }
    }finally{
      importBusy=false;modelUI.file.value='';controls();
    }
  }
  function removeModel() {
    if((mode!=='idle'&&mode!=='error')||importBusy)return;
    if(customAsset){customAsset.dispose();customAsset=null;}
    const option=customOption();if(option)option.remove();
    if(ui.filter.value==='custom'||!ui.filter.value)ui.filter.value='helmet';
    modelUI.file.value='';modelUI.info.textContent='No custom model loaded.';resetFit();controls();
    status('Custom model removed. Camera is off.',false);
  }
  function status(message,isRunning) {
    ui.status.textContent=message;
    byId('statusDot').classList.toggle('running',!!isRunning);
  }
  function controls() {
    const idle=(mode==='idle'||mode==='error')&&!restartBlocked&&!importBusy;
    ui.start.disabled=!idle;ui.demo.disabled=!idle;
    ui.source.disabled=!idle;ui.filter.disabled=!idle;
    ui.refresh.disabled=!idle;ui.stop.disabled=idle||mode==='stopping'||restartBlocked;
    ui.demoControls.hidden=mode!=='demo';modelControls(idle);
  }
  function clearError(){ui.error.hidden=true;ui.error.textContent='';}
  function describe(error) {
    const name=error&&error.name||'';
    const message=error&&error.message||String(error);
    if(name==='NotAllowedError'||name==='PermissionDeniedError')
      return 'Camera permission was denied. Allow camera access for this local page, then try Start again.';
    if(name==='NotReadableError'||name==='TrackStartError')
      return 'The camera could not be opened. It may be busy in another app. Stop that capture or choose another camera.';
    if(name==='NotFoundError'||name==='DevicesNotFoundError')
      return 'No usable camera was found. Connect a camera and press Refresh.';
    if(name==='OverconstrainedError')
      return 'The selected camera or its requested format is unavailable. Refresh the list or choose another camera.';
    if(name==='SecurityError')
      return 'Camera access is blocked by the browser. Open the lab using its localhost launcher.';
    const codes={
      GL_INCOMPATIBLE:'This browser or graphics driver does not provide the WebGL features Jeeliz needs.',
      GLCONTEXT_LOST:'The graphics context was lost. Stop and restart; reload the page if it repeats.',
      WEBCAM_UNAVAILABLE:'Jeeliz could not read the camera. Check camera permission and whether another app is using it.',
      ALREADY_INITIALIZED:'A previous tracker is still active. Stop or reload this page before trying again.'
    };
    return codes[message]||message;
  }
  function showError(error) {
    ui.error.textContent=describe(error);ui.error.hidden=false;
    mode='error';status('Preview stopped. '+describe(error),false);controls();
  }
  function releaseStream() {
    clearInterval(trackingWatchdog);trackingWatchdog=0;
    if(stream){stream.getTracks().forEach(function(track){track.stop();});stream=null;}
    ui.video.pause();ui.video.srcObject=null;ui.video.hidden=true;
  }
  function resetCanvases() {
    ['trackingCanvas','overlayCanvas'].forEach(function(id) {
      const previous=byId(id),fresh=previous.cloneNode(false);
      fresh.width=800;fresh.height=600;fresh.hidden=false;
      previous.replaceWith(fresh);
      fresh.addEventListener('webglcontextlost',function(event) {
        event.preventDefault();
        if(mode==='live'||mode==='demo') fail(new Error('GLCONTEXT_LOST'));
      });
    });
  }
  function disposeScene() {
    if(rig){rig.dispose();rig=null;}
    if(sceneStuff){
      sceneStuff.renderer.dispose();
      if(sceneStuff.renderer.forceContextLoss)sceneStuff.renderer.forceContextLoss();
      sceneStuff=null;
    }
    camera=null;
  }
  async function timeout(promise,milliseconds,message) {
    let timer;
    try { return await Promise.race([promise,new Promise(function(_,reject) {
      timer=setTimeout(function(){reject(new Error(message));},milliseconds);
    })]); } finally { clearTimeout(timer); }
  }
  async function stop() {
    if(mode==='stopping')return;
    const pendingTracker=trackerStarted&&!trackerReady;
    ++generation;mode='stopping';controls();
    cancelAnimationFrame(animationFrame);animationFrame=0;
    releaseStream();
    if(trackerStarted){
      try { await timeout(JEELIZFACEFILTER.destroy(),5000,
        'Tracker cleanup did not finish. Reload this page before restarting.'); }
      catch(error){restartBlocked=true;disposeScene();trackerStarted=false;trackerReady=false;resetCanvases();
        ui.placeholder.hidden=false;showError(error);return;}
    }
    trackerStarted=false;trackerReady=false;disposeScene();resetCanvases();
    mode='idle';ui.placeholder.hidden=false;ui.confidence.textContent='No face tracking is running.';
    if(pendingTracker){restartBlocked=true;showError(new Error('Camera stopped during tracker startup. Reload this page before starting again.'));return;}
    status('Stopped. Camera is off.',false);controls();
  }
  async function fail(error) {
    const expected=++generation;
    cancelAnimationFrame(animationFrame);releaseStream();
    if(trackerStarted){
      try{await timeout(JEELIZFACEFILTER.destroy(),5000,'Tracker cleanup timed out. Reload the page.');}
      catch(cleanup){restartBlocked=true;console.error(cleanup);}
    }
    if(generation!==expected)return;
    trackerStarted=false;trackerReady=false;disposeScene();resetCanvases();
    ui.placeholder.hidden=false;showError(error);
  }
  function webglSupported() {
    const canvas=document.createElement('canvas');
    const gl=canvas.getContext('webgl2')||canvas.getContext('webgl');
    if(!gl)throw new Error('WebGL is unavailable. Try a browser with hardware acceleration.');
    const extension=gl.getExtension('WEBGL_lose_context');if(extension)extension.loseContext();
  }
  async function assets(type) {
    if(type==='custom'){if(!customAsset)throw new Error('Choose a GLB model first.');return {custom:customAsset};}
    if(!rigPromises.has(type)){
      const promise=FilterRigs.load(THREE,type).catch(function(error){rigPromises.delete(type);throw error;});
      rigPromises.set(type,promise);
    }
    return rigPromises.get(type);
  }
  async function neuralModel() {
    if(!modelPromise)modelPromise=fetch('vendor/NN_DEFAULT.json').then(function(response){
      if(!response.ok)throw new Error('The local Jeeliz model is missing. Verify the lab files.');
      return response.json();
    }).catch(function(error){modelPromise=null;throw error;});
    return modelPromise;
  }
  async function refreshDevices() {
    if(!navigator.mediaDevices||!navigator.mediaDevices.enumerateDevices){
      status('Camera listing is unavailable. Use a supported browser and the localhost launcher.',false);return;
    }
    try {
      const devices=await navigator.mediaDevices.enumerateDevices();
      const selected=ui.source.value;
      ui.source.replaceChildren(new Option('Default camera',''));
      const system=document.createElement('optgroup');system.label='System cameras';
      const prism=document.createElement('optgroup');prism.label='PrismForge virtual inputs';
      devices.filter(function(device){return device.kind==='videoinput';}).forEach(function(device,index){
        const label=device.label||'Camera '+(index+1)+' (name available after permission)';
        const option=new Option(label,device.deviceId);
        (/prismforge/i.test(label)?prism:system).appendChild(option);
      });
      if(prism.children.length)ui.source.appendChild(prism);
      if(system.children.length)ui.source.appendChild(system);
      if(Array.from(ui.source.options).some(function(option){return option.value===selected;}))
        ui.source.value=selected;
    } catch(error){ui.error.textContent='Could not list cameras: '+describe(error);ui.error.hidden=false;}
  }
  function waitVideoEvent(name) {
    return timeout(new Promise(function(resolve,reject){
      function ready(){clean();resolve();}
      function error(){clean();reject(new Error('The selected camera video could not start.'));}
      function clean(){ui.video.removeEventListener(name,ready);ui.video.removeEventListener('error',error);}
      ui.video.addEventListener(name,ready,{once:true});ui.video.addEventListener('error',error,{once:true});
    }),15000,'The camera opened but did not deliver a frame. Check the selected input.');
  }
  async function startCamera() {
    clearError();mode='loading';controls();status('Loading local 3D assets...',false);
    const current=++generation,type=ui.filter.value;
    try {
      if(!window.isSecureContext||!navigator.mediaDevices||!navigator.mediaDevices.getUserMedia)
        throw new Error('Camera access needs a secure browser context. Open this lab at http://127.0.0.1:8743 using Start.cmd.');
      webglSupported();
      const loaded=await assets(type),nn=await neuralModel();
      if(current!==generation)return;
      status('Waiting for camera permission...',false);
      const constraints={audio:false,video:{width:{ideal:800},height:{ideal:600},frameRate:{ideal:30}}};
      if(ui.source.value)constraints.video.deviceId={exact:ui.source.value};
      const acquired=await navigator.mediaDevices.getUserMedia(constraints);
      if(current!==generation){acquired.getTracks().forEach(function(track){track.stop();});return;}
      stream=acquired;ui.video.srcObject=stream;
      const firstData=ui.video.readyState>=2?Promise.resolve():waitVideoEvent('loadeddata');
      await ui.video.play();await firstData;await waitVideoEvent('timeupdate');
      if(current!==generation)return;
      if(!ui.video.videoWidth||!ui.video.videoHeight)throw new Error('The camera delivered no usable video dimensions.');
      status('Starting local face tracking...',false);
      trackerStarted=true;
      await timeout(new Promise(function(resolve,reject){
        JEELIZFACEFILTER.init({
          canvasId:'trackingCanvas',NNC:nn,maxFacesDetected:1,followZRot:true,isKeepRunningOnWinFocusLost:true,
          videoSettings:{videoElement:ui.video},
          callbackReady:function(code,spec) {
            if(current!==generation){
              if(!code)JEELIZFACEFILTER.destroy().catch(function(error){console.error(error);});
              return;
            }
            if(code){if(trackerReady)fail(new Error(code));else reject(new Error(code));return;}
            try{
              trackerReady=true;spec.threeCanvasId='overlayCanvas';
              sceneStuff=JeelizThreeHelper.init(spec,function(){});
              sceneStuff.renderer.gammaOutput=true;
              camera=JeelizThreeHelper.create_camera();
              rig=createRig(type,loaded,false);
              sceneStuff.faceObject.add(rig.group);DogRig.lights(THREE,sceneStuff.scene);
              mode='live';ui.video.hidden=false;byId('trackingCanvas').hidden=true;ui.placeholder.hidden=true;status('Looking for a face...',true);
              lastTrackAt=performance.now();lastTrackingValid=true;
              trackingWatchdog=setInterval(function(){
                if(current!==generation||mode!=='live'||!sceneStuff)return;
                if(performance.now()-lastTrackAt>1000&&sceneStuff.faceObject.visible){
                  try{JeelizThreeHelper.render({detected:0,x:0,y:0,s:0.3,rx:0,ry:0,rz:0,expressions:[0]},camera);}
                  catch(error){fail(error);return;}
                  lastTrackingValid=false;
                  status('No fresh tracking frame. Filter hidden.',true);
                  ui.confidence.textContent='Camera video continues independently of face tracking.';
                }
              },250);
              controls();refreshDevices();resolve();
            }catch(error){reject(error);}
          },
          callbackTrack:function(rawState){
            const state=LabPose.normalize(rawState);
            if(current!==generation||mode!=='live'||!sceneStuff||!camera||!rig)return;
            lastTrackAt=performance.now();
            if(!LabPose.valid(state)){
              try{JeelizThreeHelper.render({detected:0,x:0,y:0,s:0.3,rx:0,ry:0,rz:0,expressions:[0]},camera);}
              catch(error){fail(error);return;}
              const now=performance.now();
              if(lastTrackingValid||now-lastStatusAt>250){
                lastStatusAt=now;
                const reason=LabPose.problem(state);
                ui.confidence.textContent=reason+'. Filter hidden.';
                status('Tracking state unavailable. Filter hidden.',true);
              }
              lastTrackingValid=false;
              return;
            }
            rig.setMouth(state.expressions&&state.expressions[0]||0);
            try{JeelizThreeHelper.render(state,camera);}catch(error){fail(error);return;}
            const now=performance.now();
            if(!lastTrackingValid||now-lastStatusAt>250){
              lastStatusAt=now;
              const visible=sceneStuff.faceObject.visible;
              status(visible?'Face found. Turn your head to inspect the 3D filter.':'Looking for a face. Filter hidden.',true);
              ui.confidence.textContent='Tracking confidence: '+Math.round(state.detected*100)+'%';
            }
            lastTrackingValid=true;
          }
        });
      }),30000,'Face tracker did not become ready. Stop and reload the page before retrying.');
    }catch(error){if(current===generation)await fail(error);}
  }
  async function startDemo() {
    clearError();mode='loading';controls();status('Loading the real 3D model...',false);
    const current=++generation,type=ui.filter.value;
    try{
      webglSupported();
      const loaded=await assets(type);if(current!==generation)return;
      resetCanvases();byId('trackingCanvas').hidden=true;
      const renderer=new THREE.WebGLRenderer({canvas:byId('overlayCanvas'),alpha:true,antialias:true,preserveDrawingBuffer:true});
      renderer.setPixelRatio(1);renderer.setSize(800,600,false);renderer.gammaOutput=true;
      const scene=new THREE.Scene(),faceObject=new THREE.Object3D();scene.add(faceObject);
      camera=new THREE.PerspectiveCamera(35,800/600,0.1,100);
      sceneStuff={renderer:renderer,scene:scene,faceObject:faceObject};
      rig=createRig(type,loaded,true);faceObject.add(rig.group);
      DogRig.lights(THREE,scene);
      mode='demo';ui.placeholder.hidden=true;controls();byId('mouthControl').hidden=!rig.hasMouth;
      const began=performance.now();
      function render(now){
        if(current!==generation||mode!=='demo')return;
        const time=(now-began)/1000,auto=ui.auto.checked;
        const yaw=auto?Math.sin(time*0.55)*0.65:Number(ui.yaw.value)*Math.PI/180;
        const mouth=auto?(Math.sin(time*1.15)+1)/2:Number(ui.mouth.value)/100;
        const state={detected:ui.loss.checked?0:1,x:0,y:0,s:0.21,rx:auto?Math.sin(time*0.4)*0.1:0,ry:yaw,rz:0};
        faceObject.visible=LabPose.visibility(faceObject.visible,state);
        if(faceObject.visible)LabPose.apply(THREE,faceObject,state,camera);
        rig.setMouth(mouth);renderer.render(scene,camera);
        byId('yawValue').textContent=Math.round(yaw*180/Math.PI)+' deg';
        byId('mouthValue').textContent=Math.round(mouth*100)+'%';
        status(ui.loss.checked?'Model hidden: simulated tracking loss. Camera is off.':'Model motion preview. Camera is off.',false);
        ui.confidence.textContent='Synthetic pose only. No camera or face detector is running.';
        animationFrame=requestAnimationFrame(render);
      }
      animationFrame=requestAnimationFrame(render);
    }catch(error){if(current===generation)await fail(error);}
  }
  modelUI.upload.addEventListener('click',function(){if(!modelUI.upload.disabled)modelUI.file.click();});
  modelUI.file.addEventListener('change',importModel);
  modelUI.remove.addEventListener('click',removeModel);
  modelUI.reset.addEventListener('click',resetFit);
  Object.keys(fitFields).forEach(function(key){
    fitFields[key].addEventListener('input',applyFit);
    fitFields[key].addEventListener('change',function(){applyFit();fitFields[key].value=String(modelFit[key]*(key==='scale'?100:1));});
  });
  modelUI.occlusion.addEventListener('change',applyFit);
  ui.filter.addEventListener('change',function(){modelControls((mode==='idle'||mode==='error')&&!restartBlocked&&!importBusy);});
  const prompt=byId('modelPrompt');prompt.value=ModelAuthoring.prompt;
  byId('copyModelPrompt').addEventListener('click',async function(){
    const button=byId('copyModelPrompt'),message=byId('promptStatus');button.disabled=true;
    try{await navigator.clipboard.writeText(ModelAuthoring.prompt);message.textContent='Prompt copied. Paste it into your AI tool.';}
    catch(error){byId('promptDetails').open=true;prompt.focus();prompt.select();
      message.textContent='Automatic copying is unavailable. The prompt is selected below; press Ctrl+C.';}
    finally{button.disabled=false;}
  });
  ui.start.addEventListener('click',startCamera);
  ui.stop.addEventListener('click',stop);
  ui.demo.addEventListener('click',startDemo);
  ui.refresh.addEventListener('click',refreshDevices);
  ui.mirror.addEventListener('change',function(){byId('stage').classList.toggle('mirrored',ui.mirror.checked);});
  window.addEventListener('pagehide',function(){
    ++generation;cancelAnimationFrame(animationFrame);releaseStream();disposeScene();
    if(customAsset){customAsset.dispose();customAsset=null;}
    if(trackerStarted)JEELIZFACEFILTER.destroy().catch(function(){});
  });
  resetCanvases();controls();
  // enumerateDevices does not request a camera or a permission prompt.
  refreshDevices();
})();
