import { ACTORS, SIZE, LOOP, poses, weights, transformPoint, doorPose } from './scene.js';
import { EnvironmentMotion } from './environment.js';

const VERTEX = `
attribute vec2 a_uv;
attribute vec3 a_weights;
uniform vec4 u_bounds;
uniform vec2 u_pivot;
uniform vec2 u_neck;
uniform vec2 u_joint;
uniform vec3 u_pose;
uniform vec2 u_angles;
uniform vec3 u_camera;
uniform vec2 u_drift;
uniform float u_depth;
uniform vec2 u_door;
uniform vec3 u_paper;
uniform vec4 u_view;
varying vec2 v_uv;
vec2 turn(vec2 p, vec2 pivot, float angle) {
  vec2 q=p-pivot; float c=cos(angle),s=sin(angle);
  return pivot+vec2(c*q.x-s*q.y,s*q.x+c*q.y);
}
void main() {
  vec2 p=u_bounds.xy+a_uv*u_bounds.zw;
  float paperCentre=u_bounds.y+u_bounds.w*.5;
  p.y=paperCentre+(p.y-paperCentre)*cos(u_paper.y)-u_paper.x*a_uv.y*a_uv.y;
  p.y-=u_paper.z*sin(a_uv.x*3.14159265)*a_uv.y*a_uv.y;
  p=mix(p,turn(p,u_joint,u_angles.y),a_weights.z);
  p=mix(p,turn(p,u_neck,u_angles.x),a_weights.y);
  p=mix(p,turn(p,u_pivot,u_pose.x)+u_pose.yz,a_weights.x);
  p+=a_weights.x*u_depth*u_drift;
  float doorDistance=(p.x-u_bounds.x-u_bounds.z);
  p.x=u_bounds.x+u_bounds.z+doorDistance*u_door.x;
  p.y+=doorDistance*u_door.y*(a_uv.y-.5);
  p=(p-vec2(1476.5,896.5))*u_camera.x+vec2(1476.5,896.5)+u_camera.yz;
  gl_Position=vec4((p.x-u_view.x)/u_view.z*2.0-1.0,1.0-(p.y-u_view.y)/u_view.w*2.0,0.0,1.0);
  v_uv=a_uv;
}`;
const FRAGMENT = `
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_image;
uniform vec3 u_shadow;
uniform vec3 u_shadowColour;
varying vec2 v_uv;
void main() {
  if(u_shadow.x>0.0) {
    vec2 d=u_shadow.yz;
    float a=texture2D(u_image,v_uv).a*2.0;
    a+=texture2D(u_image,v_uv+vec2(d.x,0.0)).a+texture2D(u_image,v_uv-vec2(d.x,0.0)).a;
    a+=texture2D(u_image,v_uv+vec2(0.0,d.y)).a+texture2D(u_image,v_uv-vec2(0.0,d.y)).a;
    a=a/6.0*u_shadow.x;
    gl_FragColor=vec4(u_shadowColour*a,a);
    return;
  }
  gl_FragColor=texture2D(u_image,v_uv);
}`;
function compile(gl,type,source) {
  const shader=gl.createShader(type); gl.shaderSource(shader,source); gl.compileShader(shader);
  if(!gl.getShaderParameter(shader,gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
  return shader;
}
function mesh(actor) {
  if(actor.environment==='paper') {
    const vertices=[];
    for(let y=0;y<6;y++)for(let x=0;x<8;x++)for(const [u,v] of [[x/8,y/6],[(x+1)/8,y/6],[x/8,(y+1)/6],[x/8,(y+1)/6],[(x+1)/8,y/6],[(x+1)/8,(y+1)/6]]) vertices.push(u,v,1,0,0);
    return new Float32Array(vertices);
  }
  if(actor.environment) return new Float32Array([0,0,1,0,0, 1,0,1,0,0, 0,1,1,0,0, 0,1,1,0,0, 1,0,1,0,0, 1,1,1,0,0]);
  const xs=actor.pivot?[...new Set([0,1,...Array.from({length:23},(_,i)=>(i+1)/24),...(actor.head??[]).slice(0,2)])].sort((a,b)=>a-b):[0,1];
  const ys=actor.pivot?[...new Set([0,1,...Array.from({length:39},(_,i)=>(i+1)/40),...(actor.bend??[]),...(actor.head??[]).slice(2)])].sort((a,b)=>a-b):[0,1];
  const vertices=[];
  for(let y=0;y<ys.length-1;y++)for(let x=0;x<xs.length-1;x++) {
    for(const [u,v] of [[xs[x],ys[y]],[xs[x+1],ys[y]],[xs[x],ys[y+1]],[xs[x],ys[y+1]],[xs[x+1],ys[y]],[xs[x+1],ys[y+1]]]) {
      const skin=actor.skin;
      vertices.push(u,v,...(skin?[1,0,0]:actor.pivot?weights(actor,u,v):[0,0,0]));
    }
  }
  return new Float32Array(vertices);
}
export class PaintingRenderer {
  constructor(canvas,images,settings={}) {
    this.canvas=canvas;
    this.view=settings.view??[0,0,...SIZE];
    this.poseObjects=settings.objects;
    this.actors=settings.actors??ACTORS;
    this.staticCamera=settings.staticCamera===true;
    const gl=canvas.getContext('webgl',{alpha:false,antialias:false,depth:false,stencil:false,powerPreference:'low-power'});
    if(!gl) throw new Error('WebGL unavailable');
    this.gl=gl;
    gl.clearColor(0,0,0,1);
    const vertex=compile(gl,gl.VERTEX_SHADER,VERTEX),fragment=compile(gl,gl.FRAGMENT_SHADER,FRAGMENT);
    this.program=gl.createProgram();gl.attachShader(this.program,vertex);gl.attachShader(this.program,fragment);gl.linkProgram(this.program);
    gl.deleteShader(vertex);gl.deleteShader(fragment);
    if(!gl.getProgramParameter(this.program,gl.LINK_STATUS))throw new Error(gl.getProgramInfoLog(this.program));
    gl.useProgram(this.program);
    this.uv=gl.getAttribLocation(this.program,'a_uv');this.weight=gl.getAttribLocation(this.program,'a_weights');
    this.uniforms=Object.fromEntries(['bounds','pivot','neck','joint','pose','angles','camera','drift','depth','image','door','paper','shadow','shadowColour','view'].map(n=>[n,gl.getUniformLocation(this.program,'u_'+n)]));
    gl.uniform4fv(this.uniforms.view,this.view);
    gl.uniform3fv(this.uniforms.shadowColour,settings.shadowColour??[.115,.08,.04]);
    gl.uniform1i(this.uniforms.image,0);gl.activeTexture(gl.TEXTURE0);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL,true);gl.enable(gl.BLEND);gl.blendFunc(gl.ONE,gl.ONE_MINUS_SRC_ALPHA);
    this.releaseSource=settings.releaseSource===true;
    this.layers=images.layers.map(actor=>this.layer({...actor,skin:actor.parent?images.layers.find(a=>a.id===actor.parent):undefined}));
    const error=gl.getError();
    if(error!==gl.NO_ERROR){this.destroy();throw new Error(`Painting texture allocation failed (${error})`);}
    this.frames=0;this.costs=[];this.reactions={};this.pointerTarget=null;
    this.environment=new EnvironmentMotion(this.layers.map(l=>l.actor));
    const style=getComputedStyle(canvas);
    this.pointerResponse=settings.pointerResponse??Number.parseFloat(style.getPropertyValue('--study-pointer-response'));
    this.pointerReturn=settings.pointerReturn??Number.parseFloat(style.getPropertyValue('--study-pointer-return'));
  }
  layer(actor) {
    const {image}=actor,gl=this.gl;
    const texture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,texture);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
    // Native non-power-of-two pixels, one GPU sampling step. No intermediate
    // stretched canvas, mipmap blur, colour grade, face filter or sharpening.
    gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,image);
    const vertices=mesh(actor),buffer=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,buffer);gl.bufferData(gl.ARRAY_BUFFER,vertices,gl.STATIC_DRAW);
    const pixel=image.getContext('2d').getImageData(0,0,image.width,image.height).data;
    const alpha=new Uint8Array(image.width*image.height);for(let i=0;i<alpha.length;i++)alpha[i]=pixel[i*4+3];
    const source=this.releaseSource?{width:image.width,height:image.height}:image;
    if(this.releaseSource){image.width=1;image.height=1;}
    return {id:actor.id,actor,image:source,bounds:[actor.x,actor.y,actor.width,actor.height],texture,buffer,alpha,count:vertices.length/5};
  }
  resize(width,density,cap) {
    const w=Math.max(1,Math.min(cap,Math.round(width*density))),h=Math.round(w*this.view[3]/this.view[2]);
    if(this.canvas.width===w&&this.canvas.height===h)return;
    this.canvas.width=w;this.canvas.height=h;this.gl.viewport(0,0,w,h);
  }
  clearPointer() {this.pointerTarget=null;}
  resetInteraction() {this.clearPointer();this.reactions={};this.environment.reset();}
  nudge(amount) {if(!this.staticCamera)this.environment.impulse(amount,this.seconds??0);}
  activate() {
    if(!this.pointerTarget)return;
    const {id,x}=this.pointerTarget;
    if(!this.environment.release(id,this.seconds??0,x||1))this.environment.touch(id,x||.7);
  }
  setPointer(x,y,seconds) {
    const phase=this.staticCamera?0:seconds/LOOP*Math.PI*2, depth=1-Math.cos(phase),zoom=1-depth*.0015;
    const world=[(x-SIZE[0]/2-depth*.65)/zoom+SIZE[0]/2,(y-SIZE[1]/2-depth*.35)/zoom+SIZE[1]/2];
    const state=poses(seconds,this.reactions,false,this.poseObjects);
    for(const a of this.environment.objects) if(!a.hinge) state[a.id]=this.environment.pose(a,seconds);
    for(const layer of [...this.layers].reverse()) {
      const a=layer.actor,[bx,by,bw,bh]=layer.bounds;
      const p=state[a.parent??a.poseId];
      let q=world.slice();
      if(a.hinge) {
        const door=doorPose(seconds,false,this.environment.door());
        q[0]=bx+bw+(world[0]-bx-bw)/door[0];
        const distance=q[0]-bx-bw;
        q[1]=by+((world[1]-by)+distance*door[1]*.5)/(1+distance*door[1]/bh);
      }
      if(a.parent) {
        const parent=layer.actor.skin,origin=[parent.x+parent.width*parent.pivot[0],parent.y+parent.height*parent.pivot[1]];
        const dx=world[0]-p[1]-a.depth*3*Math.sin(phase)-origin[0],dy=world[1]-p[2]-a.depth*.5*Math.sin(phase)-origin[1];
        q=[origin[0]+dx*Math.cos(p[0])+dy*Math.sin(p[0]),origin[1]-dx*Math.sin(p[0])+dy*Math.cos(p[0])];
      } else if(a.pivot)for(let i=0;i<6;i++) {
        const uv=[(q[0]-bx)/bw,(q[1]-by)/bh];
        const r=transformPoint(uv,a,p),weight=weights(a,...uv)[0];
        q=[world[0]-(r[0]+bx-q[0]+weight*a.depth*3*Math.sin(phase)),world[1]-(r[1]+by-q[1]+weight*a.depth*.5*Math.sin(phase))];
      }
      const u=(q[0]-bx)/bw,v=(q[1]-by)/bh;
      if(u<0||v<0||u>=1||v>=1)continue;
      if(layer.alpha[Math.floor(v*layer.image.height)*layer.image.width+Math.floor(u*layer.image.width)]<100)continue;
      if(!a.head&&!a.environment&&!a.parent)break;
      const clamp=n=>Math.max(-1,Math.min(1,n));
      if(a.parent) {
        const parent=a.skin;
        this.pointerTarget={id:a.parent,x:clamp((q[0]-parent.x)/parent.width*2-1),y:clamp(((q[1]-parent.y)/parent.height-.3)*2)};
        return a.parent;
      }
      if(a.environment&&this.pointerTarget?.id!==a.id) this.environment.touch(a.id,clamp(u*2-1));
      if(a.environment==='paper'&&this.pointerTarget?.id===a.id) this.environment.brush(a.id,clamp(u*2-1)-this.pointerTarget.x,clamp((v-.3)*2)-this.pointerTarget.y);
      this.pointerTarget={id:a.poseId,x:clamp(u*2-1),y:clamp((v-.3)*2)};
      return a.poseId;
    }
    this.clearPointer();return '';
  }
  respond(delta,still) {
    if(still){this.resetInteraction();return;}
    if(!delta)return;
    for(const a of this.actors) {
      const target=this.pointerTarget?.id===a.id?this.pointerTarget:null;
      const wanted=target?[Math.max(-2.6,Math.min(2.6,1.2+1.8*target.x)),5*target.x]:[0,0];
      const previous=this.reactions[a.id]??[0,0],ease=1-Math.exp(-Math.min(delta,.1)/(target?this.pointerResponse:this.pointerReturn));
      this.reactions[a.id]=previous.map((n,i)=>n+(wanted[i]-n)*ease);
    }
  }
  draw(seconds,still=false,delta=0) {
    this.seconds=seconds;
    const start=performance.now(),gl=this.gl;
    this.respond(delta,still);const state=poses(seconds,this.reactions,still,this.poseObjects);
    this.environment.step(delta,this.pointerTarget,still);
    this.canvas.dataset.impulse=this.environment.shake.toFixed(3);
    const bodyResponse=this.reactions[this.pointerTarget?.id];
    const response=this.environment.state[this.pointerTarget?.id]??(bodyResponse?{angle:bodyResponse[0],x:bodyResponse[1]}:null);
    this.canvas.dataset.response=response?`${response.angle.toFixed(2)},${response.x.toFixed(2)}`:'';
    this.canvas.dataset.fallen=Object.keys(this.environment.falls).join(',');
    for(const a of this.environment.objects) if(!a.hinge) state[a.id]=this.environment.pose(a,seconds,still);
    this.drawCalls=0;
    gl.useProgram(this.program);gl.clear(gl.COLOR_BUFFER_BIT);gl.enableVertexAttribArray(this.uv);gl.enableVertexAttribArray(this.weight);
    const phase=this.staticCamera?0:seconds/LOOP*Math.PI*2,depth=still?0:1-Math.cos(phase);
    gl.uniform3fv(this.uniforms.camera,this.staticCamera?[1,0,0]:[1-depth*.0015,depth*.65,depth*.35+this.environment.shake]);
    gl.uniform2fv(this.uniforms.drift,still||this.staticCamera?[0,0]:[3*Math.sin(phase),.5*Math.sin(phase)]);
    for(const layer of this.layers) {
      const a=layer.actor,origin=[a.x,a.y],pivot=[...(a.pivot??[0,0])];
      let p=state[a.poseId];
      if(a.parent) {const parent=this.layers.find(l=>l.id===a.parent).actor;p=state[a.parent];origin[0]=parent.x;origin[1]=parent.y;pivot[0]=parent.pivot[0]*parent.width/a.width;pivot[1]=parent.pivot[1]*parent.height/a.height;}
      gl.bindBuffer(gl.ARRAY_BUFFER,layer.buffer);gl.vertexAttribPointer(this.uv,2,gl.FLOAT,false,20,0);gl.vertexAttribPointer(this.weight,3,gl.FLOAT,false,20,8);
      gl.bindTexture(gl.TEXTURE_2D,layer.texture);gl.uniform4fv(this.uniforms.bounds,layer.bounds);
      gl.uniform2fv(this.uniforms.pivot,[origin[0]+pivot[0]*a.width,origin[1]+pivot[1]*a.height]);
      const neck=a.neck??[0,0],joint=a.joint??[0,0];
      gl.uniform2fv(this.uniforms.neck,[a.x+neck[0]*a.width,a.y+neck[1]*a.height]);gl.uniform2fv(this.uniforms.joint,[a.x+joint[0]*a.width,a.y+joint[1]*a.height]);
      gl.uniform3fv(this.uniforms.pose,p.slice(0,3));gl.uniform2fv(this.uniforms.angles,p.slice(3));gl.uniform1f(this.uniforms.depth,a.depth);
      gl.uniform2fv(this.uniforms.door,a.hinge?doorPose(seconds,still,this.environment.door()):[1,0]);
      const paper=this.environment.paper(a,seconds,still);
      if(a.environment==='paper') {
        gl.uniform3fv(this.uniforms.paper,[0,0,0]);
        gl.uniform3fv(this.uniforms.pose,[p[0],p[1]+1,p[2]+1.5]);
        gl.uniform3fv(this.uniforms.shadow,[.14-paper[0]*.008,2/layer.image.width,2/layer.image.height]);
        gl.drawArrays(gl.TRIANGLES,0,layer.count);
        this.drawCalls++;
        gl.uniform3fv(this.uniforms.pose,p.slice(0,3));
      }
      gl.uniform3fv(this.uniforms.shadow,[0,0,0]);
      gl.uniform3fv(this.uniforms.paper,paper);
      gl.drawArrays(gl.TRIANGLES,0,layer.count);
      this.drawCalls++;
    }
    this.frames++;this.costs.push(performance.now()-start);if(this.costs.length>120)this.costs.shift();
  }
  destroy() {
    if(this.gl.isContextLost())return;
    for(const {texture,buffer} of this.layers){this.gl.deleteTexture(texture);this.gl.deleteBuffer(buffer);}this.gl.deleteProgram(this.program);
  }
}
