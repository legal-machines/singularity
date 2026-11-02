import { prepareArtwork } from './artwork.js';
import { VIEW, OBJECTS_ONBOARDING, ACTORS_ONBOARDING } from './composition.js';
import { PaintingRenderer } from './renderer.js';
import { bindPaintingCursor } from './pointer.js';
import { LOOP, SIZE } from './scene.js';

// The same artwork, rig, layering and pointer physics as the public painting.
// This host has its own lifecycle because it does not use the home scroll scene.
// Taken as it is from the Singularity brand study
// (sites/public/experiments/singularity-brand/living-painting.js), with
// renderer.js, scene.js, environment.js and pointer.js from the site's painting.
// Onboarding plays the painting half as fast again as the site, its camera
// breathing as there: the owner's word, "let the animation play more actively".
const PACE=1.5;

export function mountEntryPainting(panel) {
  const canvas=panel.querySelector('canvas'),style=getComputedStyle(canvas);
  const number=name=>Number.parseFloat(style.getPropertyValue(name));
  const limited=matchMedia('(prefers-reduced-motion: reduce)');
  const phone=matchMedia('(max-width: 699px)');
  const settings={view:VIEW,objects:OBJECTS_ONBOARDING,actors:ACTORS_ONBOARDING,releaseSource:true,shadowColour:[.114,.078,.039],pointerResponse:number('--painting-pointer-response')/1000,pointerReturn:number('--painting-pointer-return')/1000};
  let renderer,controller,loading=false,visible=true,disposed=false,lost=false,paused=false,failed=false;
  let raf=0,previous=0,lastDraw=0,seconds=0;
  const reduced=()=>limited.matches||navigator.connection?.saveData===true;
  const active=()=>renderer&&!disposed&&!lost&&!failed&&!paused&&visible&&!document.hidden&&!reduced()&&!document.querySelector('dialog[open]');
  function measure(){
    const box=panel.getBoundingClientRect(),scale=Math.max(box.width/VIEW[2],box.height/VIEW[3]);
    return {left:box.left+(box.width-VIEW[2]*scale)/2-VIEW[0]*scale,top:box.top+(box.height-VIEW[3]*scale)/2-VIEW[1]*scale,width:SIZE[0]*scale,height:SIZE[1]*scale};
  }
  function draw(delta=0){if(!renderer||lost)return;renderer.draw(seconds,false,delta);canvas.dataset.frame=String(renderer.frames);canvas.dataset.drawCalls=String(renderer.drawCalls);canvas.dataset.time=seconds.toFixed(2);}
  function resize(){if(!renderer||lost)return;const box=panel.getBoundingClientRect();renderer.resize(Math.max(box.width,box.height*VIEW[2]/VIEW[3]),Math.max(devicePixelRatio||1,number('--painting-density')),number('--painting-pixel-cap'));draw();}
  function stop(){cancelAnimationFrame(raf);raf=0;previous=0;}
  function tick(now){
    raf=0;if(!active())return;
    if(previous)seconds=(seconds+Math.min(.1,(now-previous)/1000)*PACE)%LOOP;
    previous=now;const interval=1000/number(phone.matches?'--painting-fps-phone':'--painting-fps');
    if(now-lastDraw>=interval){draw(Math.min(.1,(now-lastDraw)/1000));lastDraw=now-((now-lastDraw)%interval);}
    raf=requestAnimationFrame(tick);
  }
  async function load(){
    if(renderer||loading||disposed||lost||reduced()||failed)return;
    loading=true;const current=new AbortController();controller=current;
    try{
      const artwork=await prepareArtwork(current.signal);
      if(disposed||current.signal.aborted)return;
      renderer=new PaintingRenderer(canvas,artwork,settings);resize();
      canvas.dataset.layers=String(renderer.layers.length);canvas.dataset.actors=ACTORS_ONBOARDING.map(a=>a.id).join(',');canvas.setAttribute('data-ready','');canvas.setAttribute('aria-hidden','false');panel.querySelector('img').setAttribute('aria-hidden','true');
    }catch(error){if(error.name!=='AbortError'){failed=true;canvas.removeAttribute('data-ready');canvas.setAttribute('aria-hidden','true');panel.querySelector('img').setAttribute('aria-hidden','false');canvas.dataset.motion='fallback';}}
    finally{loading=false;if(!disposed)reconcile();}
  }
  function reconcile(){
    stop();if(disposed)return;
    if(reduced()){
      controller?.abort();renderer?.destroy();renderer=undefined;
      canvas.removeAttribute('data-ready');canvas.setAttribute('aria-hidden','true');panel.querySelector('img').setAttribute('aria-hidden','false');canvas.dataset.motion='reduced';return;
    }
    if(visible&&!renderer&&!loading&&!failed&&!lost)void load();
    canvas.dataset.motion=failed||lost?'fallback':loading?'loading':active()?'running':'paused';
    if(active()){lastDraw=performance.now();raf=requestAnimationFrame(tick);}
  }
  function keyboard(event){
    if(event.key===' '){event.preventDefault();paused=!paused;reconcile();}
    if(event.key.toLowerCase()==='r'){renderer?.resetInteraction();seconds=0;paused=false;draw();reconcile();}
  }
  function contextLost(event){event.preventDefault();stop();lost=true;controller?.abort();renderer=undefined;canvas.removeAttribute('data-ready');canvas.setAttribute('aria-hidden','true');panel.querySelector('img').setAttribute('aria-hidden','false');canvas.dataset.motion='fallback';}
  function contextRestored(){lost=false;failed=false;reconcile();}
  const unbind=bindPaintingCursor(canvas,panel,()=>renderer,active,()=>seconds,measure);
  const resizeObserver=new ResizeObserver(resize);resizeObserver.observe(panel);
  const intersection=new IntersectionObserver(([entry])=>{visible=entry.isIntersecting;reconcile();});intersection.observe(panel);
  const dialogs=new MutationObserver(reconcile);document.querySelectorAll('dialog').forEach(dialog=>dialogs.observe(dialog,{attributes:true,attributeFilter:['open']}));
  canvas.addEventListener('keydown',keyboard);canvas.addEventListener('webglcontextlost',contextLost);canvas.addEventListener('webglcontextrestored',contextRestored);
  document.addEventListener('visibilitychange',reconcile);limited.addEventListener('change',reconcile);
  reconcile();
  return ()=>{
    disposed=true;stop();controller?.abort();unbind();resizeObserver.disconnect();intersection.disconnect();dialogs.disconnect();renderer?.destroy();renderer=undefined;
    // The drawing buffer goes with the page: when onboarding hands the window to
    // the app, the painting's pixels leave the GPU at once, not at collection.
    canvas.removeAttribute('data-ready');canvas.width=1;canvas.height=1;
    canvas.removeEventListener('keydown',keyboard);canvas.removeEventListener('webglcontextlost',contextLost);canvas.removeEventListener('webglcontextrestored',contextRestored);
    document.removeEventListener('visibilitychange',reconcile);limited.removeEventListener('change',reconcile);
  };
}
