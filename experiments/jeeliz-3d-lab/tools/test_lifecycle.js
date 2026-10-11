'use strict';
// Isolated DOM and owned fake streams exercise lifetime controls; no camera/WebGL is opened.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const Pose=require('../site/pose.js');
const nodes={},events={},scheduled=new Map();
let clockNow=1000;
let checks=0,frames=0,cameraCalls=0,stopCalls=0,trackerCalls=0,destroyCalls=0,modelLoads=0,lastConstraints=null,lastRigType=null;
let cameraError=null,pendingReady=null,holdReady=false,lastInit=null,renderedVisible=null;
function check(name,fn){fn();checks++;console.log('PASS '+checks+': '+name);}
class Element{
 constructor(id=''){this.id=id;this.value='';this.checked=false;this.disabled=false;this.hidden=false;this.events={};this.children=[];this.width=800;this.height=600;this.textContent='';this.readyState=2;this.videoWidth=800;this.videoHeight=600;this.classList={toggle(){}};}
 addEventListener(name,fn){(this.events[name]||=[]).push(fn);}
 removeEventListener(name,fn){this.events[name]=(this.events[name]||[]).filter(x=>x!==fn);}
 async fire(name){await Promise.all((this.events[name]||[]).map(fn=>fn({preventDefault(){}})));}
 replaceChildren(...children){this.children=children;}
 appendChild(child){this.children.push(child);child.parentElement=this;}
 remove(){if(this.parentElement)this.parentElement.children=this.parentElement.children.filter(x=>x!==this);}
 click(){this.clicked=true;}
 focus(){this.focused=true;}
 select(){this.selected=true;}
 get options(){return this.children.flatMap(child=>child.children.length?child.children:[child]);}
 cloneNode(){return new Element(this.id);}
 replaceWith(next){nodes[this.id]=next;}
 getContext(){return {getExtension:()=>({loseContext(){}})};}
 pause(){}
 play(){setImmediate(()=>this.fire('timeupdate'));return Promise.resolve();}
}
const byId=id=>nodes[id]||=new Element(id);
const ids=['startCamera','stopCamera','demoMode','cameraSelect','refreshCameras','filterSelect','status',
 'confidence','error','placeholder','demoControls','mirror','demoYaw','demoMouth','autoMotion','simulateLoss',
 'sourceVideo','mouthControl','statusDot','trackingCanvas','overlayCanvas','stage','yawValue','mouthValue'];
ids.forEach(byId);byId('filterSelect').value='helmet';byId('mirror').checked=true;byId('autoMotion').checked=true;
class Option extends Element{constructor(label,value){super();this.textContent=label;this.value=value;}}
class Object3D{constructor(){this.visible=false;this.children=[];}add(object){this.children.push(object);}}
class Renderer{setPixelRatio(){}setSize(){}dispose(){}forceContextLoss(){}render(scene){renderedVisible=scene.children[0].visible;}}
const document={getElementById:byId,createElement:()=>new Element()};
const THREE={WebGLRenderer:Renderer,Scene:Object3D,Object3D,Group:Object3D,PerspectiveCamera:class{}};
const stream={getTracks:()=>[{stop(){stopCalls++;}}]};
const mediaDevices={
 async enumerateDevices(){return [{kind:'videoinput',deviceId:'prism-input',label:'PrismForge Studio Virtual Camera'},
   {kind:'videoinput',deviceId:'system-input',label:'USB camera'}];},
 async getUserMedia(constraints){cameraCalls++;lastConstraints=constraints;if(cameraError)throw cameraError;return stream;}
};
const FilterRigs={async load(){return {data:[],maps:[]};},create(three,type){lastRigType=type;return {group:new Object3D(),hasMouth:type==='dog'||type==='tiger',setMouth(){},dispose(){}};}};
const JEELIZFACEFILTER={
 init(spec){trackerCalls++;lastInit=spec;if(holdReady){pendingReady=()=>spec.callbackReady(false,{});}
 else setImmediate(()=>spec.callbackReady(false,{}));},
 async destroy(){destroyCalls++;}
};
let liveStuff=null,liveRenderCalls=0,backgroundDrawCalls=0;
const JeelizThreeHelper={
 init(){liveStuff={renderer:new Renderer(),faceObject:new Object3D(),scene:new Object3D()};return liveStuff;},
 create_camera(){return {};},
 render(state){liveRenderCalls++;backgroundDrawCalls++;liveStuff.faceObject.visible=Pose.visibility(liveStuff.faceObject.visible,state);}
};
const window={isSecureContext:true,addEventListener:(name,fn)=>(events[name]||=[]).push(fn)};
let importError=null,holdImport=false,resolveImport=null,copiedPrompt=null,copyError=false;
const customAssets=[],fitUpdates=[];
const LocalModel={async loadFile(three,file){
 if(importError)throw importError;
 if(holdImport)await new Promise(resolve=>resolveImport=resolve);
 const asset={name:file.name,stats:{triangles:30,textures:1},initialFit:file.name==='profile-cap.glb'?{scale:.7,x:0,y:.6579875598136216,z:-.2,rx:0,ry:180,rz:0,occlusion:true}:undefined,disposals:0,rigDisposals:0,
  createRig(fit){fitUpdates.push({...fit});lastRigType='custom';return {group:new Object3D(),updateFit(next){fitUpdates.push({...next});},dispose(){asset.rigDisposals++;}};},
  dispose(){this.disposals++;}};customAssets.push(asset);return asset;
}};
const HeadAttachment={profiles:{dogHead:{}},occluder(){return {group:new Object3D(),mesh:{geometry:{dispose(){}},material:{dispose(){}}}};}};
const ModelAuthoring={prompt:'Create an original static embedded GLB model.'};
const clipboard={async writeText(text){if(copyError)throw new Error('blocked');copiedPrompt=text;}};
const context={window,document,navigator:{mediaDevices,clipboard},LocalModel,HeadAttachment,ModelAuthoring,THREE,FilterRigs,JEELIZFACEFILTER,JeelizThreeHelper,
 DogRig:{lights(){}},LabPose:{...Pose,apply:()=>true},Option,Map,performance:{now:()=>clockNow},console,
 fetch:async()=>{modelLoads++;return {ok:true,json:async()=>({})};},
 setTimeout,clearTimeout,setInterval,clearInterval,requestAnimationFrame:fn=>{scheduled.set(++frames,fn);return frames;},
 cancelAnimationFrame:id=>scheduled.delete(id)};
vm.createContext(context);
vm.runInContext(fs.readFileSync(path.join(__dirname,'../site/lab.js'),'utf8'),context,{timeout:1000});
async function tick(){await new Promise(resolve=>setImmediate(resolve));}
async function run(){
 await tick();
 check('page load only enumerates; never acquires camera, tracker or neural model',()=>{
  assert.equal(cameraCalls,0);assert.equal(trackerCalls,0);assert.equal(modelLoads,0);
 });
 check('real PrismForge and system sources appear as distinct selectable groups',()=>{
  assert(byId('cameraSelect').options.some(x=>x.value==='prism-input'));
  assert(byId('cameraSelect').children.some(x=>x.label==='PrismForge virtual inputs'));
 });
 await byId('demoMode').fire('click');
 const first=scheduled.entries().next().value;scheduled.delete(first[0]);first[1](1000);
 check('render-only button renders model without any camera or tracker',()=>{
  assert.equal(cameraCalls,0);assert.equal(trackerCalls,0);assert.equal(modelLoads,0);
  assert.equal(renderedVisible,true);assert(byId('status').textContent.includes('Camera is off'));assert.equal(byId('mouthControl').hidden,true);
 });
 byId('simulateLoss').checked=true;
 const lost=scheduled.entries().next().value;scheduled.delete(lost[0]);lost[1](1100);
 check('synthetic tracking loss hides rig on the next actual render call',()=>assert.equal(renderedVisible,false));
 await byId('stopCamera').fire('click');
 check('Stop cancels synthetic render loop and permits changing input/filter',()=>{
  assert.equal(scheduled.size,0);assert.equal(byId('filterSelect').disabled,false);assert.equal(byId('cameraSelect').disabled,false);
 });
 cameraError=Object.assign(new Error('busy'),{name:'NotReadableError'});
 await byId('startCamera').fire('click');
 check('busy camera has meaningful error and no tracker startup',()=>{
  assert.equal(trackerCalls,0);assert(byId('error').textContent.includes('busy'));
 });
 cameraError=null;byId('cameraSelect').value='prism-input';
 await byId('startCamera').fire('click');
 check('explicit Start uses selected exact device and never requests audio',()=>{
  assert.equal(lastConstraints.video.deviceId.exact,'prism-input');assert.equal(lastConstraints.audio,false);
  assert.equal(trackerCalls,1);assert.equal(lastInit.isKeepRunningOnWinFocusLost,true);
 });
 check('active capture locks source/filter selection and keeps Stop available',()=>{
  assert.equal(byId('filterSelect').disabled,true);assert.equal(byId('cameraSelect').disabled,true);
  assert.equal(byId('stopCamera').disabled,false);
 });
 lastInit.callbackTrack({detected:0.99,x:0,y:0,s:0.3,rx:0,ry:0,rz:0,expressions:[0.6]});
 check('valid live pose shows rig and authentic confidence',()=>{
  assert.equal(liveStuff.faceObject.visible,true);assert(byId('confidence').textContent.includes('99%'));
 });
 const scoreOvershoot={detected:1.002,x:0,y:0,s:0.3,rx:0,ry:0,rz:0,expressions:[0.6]};
 const beforeOvershoot=liveRenderCalls;
 for(let frame=0;frame<120;frame++){
  clockNow+=34;
  scoreOvershoot.ry=Math.sin(frame/20)*0.4;
  lastInit.callbackTrack(scoreOvershoot);
  assert.equal(liveStuff.faceObject.visible,true);
 }
 check('120 consecutive real bug score1.002 frames keep rendering without mutating tracker input',()=>{
  assert.equal(liveRenderCalls,beforeOvershoot+120);assert.equal(scoreOvershoot.detected,1.002);
  assert(byId('confidence').textContent.includes('100%'));
  assert(!byId('status').textContent.includes('unavailable'));
 });
 lastInit.callbackTrack({...scoreOvershoot,detected:-0.002});
 check('finite confidence undershoot hides filter without rejecting raw video',()=>{
  assert.equal(liveStuff.faceObject.visible,false);assert.equal(byId('sourceVideo').hidden,false);
 });
 lastInit.callbackTrack({...scoreOvershoot,ry:NaN});
 check('score normalization never admits invalid pose or nonfinite confidence',()=>{
  assert.equal(liveStuff.faceObject.visible,false);
  assert.equal(Pose.valid(Pose.normalize({...scoreOvershoot,detected:NaN})),false);
  assert.equal(Pose.valid(Pose.normalize({...scoreOvershoot,detected:Infinity})),false);
  assert.equal(Pose.valid(Pose.normalize({...scoreOvershoot,s:0})),false);
 });
 lastInit.callbackTrack({detected:0.2,x:0,y:0,s:0.3,rx:0,ry:0,rz:0,expressions:[0]});
 check('lost live detection hides actual shared rig immediately',()=>assert.equal(liveStuff.faceObject.visible,false));
 lastInit.callbackTrack([{detected:0.96,x:0,y:0,s:0.3,rx:0,ry:0,rz:0,expressions:[0]}]);
 check('single-face array payload uses genuine tracker object without fabricated pose',()=>assert.equal(liveStuff.faceObject.visible,true));
 const beforeInvalidRenders=liveRenderCalls,beforeInvalidBackground=backgroundDrawCalls;
 lastInit.callbackTrack({detected:NaN,x:0,y:0,s:0,rx:0,ry:0,rz:0,expressions:[0]});
 check('invalid tracker state also renders cleared rig instead of stale overlay',()=>{
  assert.equal(liveStuff.faceObject.visible,false);assert(byId('confidence').textContent.includes('not finite'));
  assert.equal(liveRenderCalls,beforeInvalidRenders+1);assert.equal(backgroundDrawCalls,beforeInvalidBackground+1);
  assert.equal(byId('sourceVideo').hidden,false);assert.equal(byId('sourceVideo').srcObject,stream);
 });
 await byId('stopCamera').fire('click');
 check('Stop releases owned stream and destroys tracker before restart',()=>{
  assert.equal(stopCalls,1);assert.equal(destroyCalls,1);assert.equal(byId('sourceVideo').srcObject,null);
  assert.equal(byId('startCamera').disabled,false);
 });
 cameraError=Object.assign(new Error('denied'),{name:'NotAllowedError'});
 await byId('startCamera').fire('click');
 check('permission denial is surfaced without starting tracker',()=>{
  assert(byId('error').textContent.includes('permission was denied'));assert.equal(trackerCalls,1);
 });
 cameraError=null;byId('simulateLoss').checked=false;
 for(const type of ['helmet','mask','dog','tiger','werewolf','astronaut','cyber']) {
  byId('filterSelect').value=type;
  await byId('demoMode').fire('click');
  const frame=scheduled.entries().next().value;scheduled.delete(frame[0]);frame[1](clockNow);
  check(type+' model mode uses its selected rig and mouth capability',()=>{
   assert.equal(lastRigType,type);assert.equal(byId('mouthControl').hidden,!(type==='dog'||type==='tiger'));
   assert.equal(renderedVisible,true);
  });
  await byId('stopCamera').fire('click');
  await byId('startCamera').fire('click');
  clockNow+=34;lastInit.callbackTrack({...scoreOvershoot,ry:0.4});
  check(type+' live mode retains selected rig and handles overshoot/loss/recovery',()=>{
   assert.equal(lastRigType,type);assert.equal(liveStuff.faceObject.visible,true);
   assert.equal(byId('filterSelect').disabled,true);
   lastInit.callbackTrack({...scoreOvershoot,detected:0.1});assert.equal(liveStuff.faceObject.visible,false);
   lastInit.callbackTrack({...scoreOvershoot,detected:0.99});assert.equal(liveStuff.faceObject.visible,true);
  });
  await byId('stopCamera').fire('click');
 }
 byId('fitScale').value='100';for(const key of ['X','Y','Z','RX','RY','RZ'])byId('fit'+key).value='0';
 byId('fitOcclusion').checked=true;
 await byId('copyModelPrompt').fire('click');
 check('Copy button sends exactly the model creation prompt',()=>{
  assert.equal(copiedPrompt,ModelAuthoring.prompt);assert(byId('promptStatus').textContent.includes('copied'));
 });
 copyError=true;await byId('copyModelPrompt').fire('click');
 check('Clipboard denial exposes and selects readable manual-copy prompt',()=>{
  assert.equal(byId('promptDetails').open,true);assert.equal(byId('modelPrompt').selected,true);
  assert.equal(byId('modelPrompt').value,ModelAuthoring.prompt);assert.equal(byId('copyModelPrompt').disabled,false);
 });
 await byId('uploadModel').fire('click');
 check('Upload button opens only its local file chooser',()=>assert.equal(byId('modelFile').clicked,true));
 byId('modelFile').files=[{name:'my-mask.glb'}];await byId('modelFile').fire('change');
 check('Successful import adds and selects custom model with visible fitting controls',()=>{
  assert.equal(byId('filterSelect').value,'custom');assert.equal(byId('modelFit').hidden,false);
  assert(byId('modelInfo').textContent.includes('my-mask.glb'));assert.equal(customAssets[0].disposals,0);
 });
 await byId('demoMode').fire('click');
 const customFrame=scheduled.entries().next().value;scheduled.delete(customFrame[0]);customFrame[1](clockNow);
 check('Uploaded model uses the same demo route with owned lifetime and no mouth animation',()=>{
  assert.equal(lastRigType,'custom');assert.equal(byId('mouthControl').hidden,true);assert.equal(renderedVisible,true);
  assert.equal(byId('uploadModel').disabled,true);assert.equal(byId('removeModel').disabled,true);
 });
 byId('fitScale').value='150';byId('fitRX').value='35';await byId('fitScale').fire('input');
 check('Live model fit passes bounded uniform scale and degree rotation to its rig',()=>{
  assert.equal(fitUpdates.at(-1).scale,1.5);assert.equal(fitUpdates.at(-1).rx,35);
 });
 byId('fitX').value='NaN';await byId('fitX').fire('input');
 byId('fitZ').value='99';await byId('fitZ').fire('input');
 check('Invalid and oversized UI fit values never produce nonfinite rig transforms',()=>{
  assert.equal(fitUpdates.at(-1).x,0);assert.equal(fitUpdates.at(-1).z,3);
 });
 await byId('resetFit').fire('click');
 check('Reset restores the complete model fit without restarting capture',()=>{
  assert.equal(fitUpdates.at(-1).scale,1);assert.equal(fitUpdates.at(-1).rx,0);assert.equal(fitUpdates.at(-1).z,0);
 });
 await byId('stopCamera').fire('click');
 check('Stop disposes uploaded rig but preserves model for restart',()=>{
  assert.equal(customAssets[0].rigDisposals,1);assert.equal(customAssets[0].disposals,0);
 });
 importError=new Error('Only embedded static GLB files are supported.');
 byId('modelFile').files=[{name:'bad.glb'}];await byId('modelFile').fire('change');
 check('Rejected upload preserves prior model, fit and usable controls',()=>{
  assert.equal(customAssets[0].disposals,0);assert.equal(byId('filterSelect').value,'custom');
  assert(byId('modelInfo').textContent.includes('Kept'));assert(byId('error').textContent.includes('embedded'));
  assert.equal(byId('startCamera').disabled,false);
 });
 importError=null;byId('modelFile').files=[{name:'second.glb'}];await byId('modelFile').fire('change');
 check('Replacing stopped custom model disposes previous GPU asset exactly once',()=>{
  assert.equal(customAssets[0].disposals,1);assert.equal(customAssets[1].disposals,0);
  assert.equal(byId('filterSelect').options.filter(x=>x.value==='custom').length,1);
 });
 await byId('startCamera').fire('click');lastInit.callbackTrack({...scoreOvershoot,ry:0.1});
 check('Custom model live route uses genuine pose, loss and recovery gates',()=>{
  assert.equal(lastRigType,'custom');assert.equal(liveStuff.faceObject.visible,true);
  lastInit.callbackTrack({...scoreOvershoot,detected:0});assert.equal(liveStuff.faceObject.visible,false);
  lastInit.callbackTrack({...scoreOvershoot,detected:0.99});assert.equal(liveStuff.faceObject.visible,true);
 });
 await byId('stopCamera').fire('click');await byId('removeModel').fire('click');
 check('Remove clears session asset and selector while leaving camera off',()=>{
  assert.equal(customAssets[1].disposals,1);assert.equal(byId('modelFit').hidden,true);
  assert.equal(byId('filterSelect').value,'helmet');assert.equal(byId('removeModel').disabled,true);
 });
 holdImport=true;byId('modelFile').files=[{name:'cancelled.glb'}];const importing=byId('modelFile').fire('change');
 await tick();await byId('stopCamera').fire('click');
 check('Cancelled import stays serialized until its pending resources finish loading',()=>{
  assert.equal(byId('uploadModel').disabled,true);assert.equal(byId('startCamera').disabled,true);
 });resolveImport();await importing;
 check('Stop during model load disposes late asset and never installs stale selection',()=>{
  assert.equal(customAssets[2].disposals,1);assert.equal(byId('filterSelect').value,'helmet');
  assert.equal(byId('startCamera').disabled,false);assert.equal(byId('uploadModel').disabled,false);
 });holdImport=false;
 byId('modelFile').files=[{name:'profile-cap.glb'}];await byId('modelFile').fire('change');
 check('reviewed asset starting fit populates all UI fields including rotation',()=>{
  assert.equal(byId('fitScale').value,'70');assert.equal(byId('fitY').value,'0.658');assert.equal(byId('fitRY').value,'180');
 });
 await byId('demoMode').fire('click');byId('fitY').value='2';await byId('fitY').fire('input');
 await byId('resetFit').fire('click');
 check('Reset restores the selected model profile rather than flattening it to generic zeros',()=>{
  assert.equal(fitUpdates.at(-1).scale,.7);assert.equal(fitUpdates.at(-1).y,.6579875598136216);assert.equal(fitUpdates.at(-1).ry,180);
 });
 await byId('stopCamera').fire('click');
 const stopsBeforeCancellation=stopCalls;
 cameraError=null;holdReady=true;
 const starting=byId('startCamera').fire('click');
 await tick();await tick();
 await byId('stopCamera').fire('click');
 check('cancellation during tracker startup stops camera and blocks unsafe overlapping restart',()=>{
  assert.equal(stopCalls,stopsBeforeCancellation+1);assert.equal(byId('startCamera').disabled,true);
  assert(byId('error').textContent.includes('Reload'));
 });
 if(pendingReady)pendingReady();
 // Stale callback leaves the start promise's timeout pending; page-close cancels the process,
 // so we assert the stale cleanup path synchronously, then finish this isolated test.
 await tick();
 check('late cancelled tracker callback cleans up rather than attaching a new scene',()=>{
  assert(destroyCalls>=3);assert.equal(byId('startCamera').disabled,true);
 });
 console.log(JSON.stringify({checksPassed:checks,cameraWasFake:true,browserWebglExecuted:false},null,2));
 process.exit(0);
}
run().catch(error=>{console.error(error);process.exit(1);});
