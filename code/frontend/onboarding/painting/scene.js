export const SIZE = [2953, 1793];
export const LOOP = 22;
const rad = n => n * Math.PI / 180;
export const smooth = (a, b, x) => { const t = Math.max(0, Math.min(1, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
const ease = (a,b,x) => { const t=Math.max(0,Math.min(1,(x-a)/(b-a))); return t*t*t*(10+t*(-15+6*t)); };
const wave = (t, start, rise, hold, fall) => ease(start, start + rise, t) * (1 - ease(start + rise + hold, start + rise + hold + fall, t));

// All bounds use the original canvas. Sprite sizes use ONE scalar, never
// independent x/y scales. Faces and feet therefore retain native proportions.
export const OBJECTS = [
  { id: 'background', asset: 'interiorInteractive', box: [0, 0, ...SIZE], depth: 0 },
  { id: 'entranceBundles', x: -8, y: 115, height: 397, depth: .04, pivot: [.58,.03], environment: 'hanging', phase: .4 },
  { id: 'wallBag', x: 1304, y: 286, height: 196, depth: .04, pivot: [.5,.03], environment: 'hanging', phase: 1.6, fallFloor: 1030 },
  { id: 'wallPapers', x: 2590, y: 194, height: 175, depth: .04, pivot: [.5,.03], environment: 'hanging', phase: 2.5 },
  { id: 'floorPaperLeft', asset: 'loosePaper', x: 475, y: 1485, width: 94, depth: .1, pivot: [.5,.5], environment: 'paper', phase: .5, direction: [1,.28] },
  { id: 'floorPaperRight', asset: 'floorPaperCurled', x: 1870, y: 1650, width: 103, depth: .1, pivot: [.5,.5], environment: 'paper', phase: 2.1, direction: [-.85,.45] },
  { id: 'floorPaperMiddle', asset: 'floorPaperFolded', x: 650, y: 1680, width: 116, depth: .1, pivot: [.5,.5], environment: 'paper', phase: 4.3, direction: [.6,-.8] },
  { id: 'chairs', x: 1785, y: 648, width: 912, depth: .18 },
  { id: 'leftRear', x: 234, y: 422, height: 952, depth: .2, pivot: [.5,.57], bend: [.51,.77], head: [.12,1,.23,.3], neck: [.6,.26], phase: .7 },
  { id: 'door', x: 425, y: 250, height: 1015, depth: 0, hinge: [1,.5], environment: 'door', phase: .3 },
  { id: 'leftFront', x: 0, y: 522, height: 1087, depth: .45, pivot: [.5,.62], bend: [.54,.78], head: [.24,.92,.27,.34], neck: [.6,.3], phase: 1.4 },
  { id: 'clerk', x: 580, y: 480, height: 650, depth: .2, pivot: [.48,.55], bend: [.61,.88], head: [.2,.77,.27,.35], neck: [.46,.32], phase: .1 },
  { id: 'leftDesk', x: 525, y: 705, width: 700, depth: .22 },
  { id: 'waitingRear', x: 963, y: 584, height: 975, depth: .4, pivot: [.51,.53], bend: [.48,.78], head: [.56,.99,.20,.27], neck: [.77,.24], phase: 1.1 },
  { id: 'waitingFront', x: 1234, y: 569, height: 1050, depth: .55, pivot: [.51,.49], bend: [.68,.88], head: [.44,1,.19,.26], neck: [.73,.23], phase: 2.1 },
  { id: 'assistant', x: 1798, y: 408, height: 780, depth: .3, pivot: [.37,.57], bend: [.56,.83], head: [.56,1,.28,.36], neck: [.77,.33], arm: [.56,1,.34,.48], joint: [.55,.39], phase: 1.7 },
  { id: 'lawyer', x: 2195, y: 474, height: 870, depth: .45, pivot: [.52,.49], bend: [.54,.82], head: [.23,.64,.27,.35], neck: [.44,.33], phase: .5 },
  { id: 'assistantArm', asset: 'assistant', overlay: 'assistant', clip: [[.31,.38],[.50,.34],[.61,.30],[.76,.29],[.82,.245],[.90,.243],[1,.27],[1,.355],[.95,.36],[.89,.345],[.86,.39],[.78,.395],[.67,.365],[.57,.42],[.38,.46],[.31,.44]] },
  { id: 'floorRight', x: 2070, y: 1410, width: 625, depth: .2, pivot: [.55,.96], environment: 'heavy', phase: 1.5 },
  { id: 'rightDesk', asset: 'rightDeskClear', x: 1900, y: 713, width: 833, depth: .68 },
  { id: 'hatVisitor', x: 1673, y: 653, height: 980, depth: .72, pivot: [.49,.53], bend: [.64,.87], head: [.23,.72,.21,.28], neck: [.44,.25], phase: 2.8 },
  { id: 'diagonalCane', x: 1375, y: 945, height: 650, depth: .9, parent: 'petitioner', pivot: [0,1] },
  { id: 'petitioner', x: 1450, y: 650, height: 1112, depth: .9, pivot: [.48,.47], bend: [.49,.76], head: [.42,1,.27,.34], neck: [.62,.3], phase: .3 },
  { id: 'handHat', x: 1669, y: 1028, height: 391, depth: .95, parent: 'petitioner', pivot: [.1,.08], phase: .3 },
  // The petitioner's hanging chicken is tied at his elbow, in front of
  // the sleeves and lower hat. The restored asset retains its legacy name.
  { id: 'furPouch', x: 1578, y: 1008, height: 415, depth: .9, parent: 'petitioner', pivot: [.5,0] },
  // One complete transparent figure, in the same native rig as the visitors.
  // Onboarding sways the Kitsune from its ankles, its body one solid from paws
  // to ears.
  { id: 'kitsune', x: 1835, y: 1007, height: 710, depth: .94, pivot: [.48,.95], bend: [.88,.97], head: [.22,1,.34,.40], neck: [.43,.335], arm: [.05,.76,.40,.65], joint: [.59,.38], responseScale: .45, phase: 1.3 },
  { id: 'basketWoman', x: 670, y: 960, height: 715, depth: .85, pivot: [.32,.94], head: [.43,.78,.35,.43], neck: [.58,.4], phase: 3.2 },
  { id: 'stool', x: 2673, y: 1221, width: 235, depth: .65 },
  { id: 'client', x: 2581, y: 554, height: 1092, depth: .85, pivot: [.63,.47], bend: [.67,.9], head: [.31,1,.27,.35], neck: [.58,.31], phase: 1.9 },
  { id: 'floorPail', x: 1053, y: 1575, height: 214, depth: 1, pivot: [.5,.95], environment: 'heavy', phase: .8 },
  { id: 'floorLeft', x: -18, y: 1578, width: 633, depth: 1, pivot: [.5,.96], environment: 'heavy', phase: 2.3 },
];
export const ACTORS = OBJECTS.filter(o => o.head && !o.overlay);
export const ORDER = OBJECTS.map(o => o.id);
export const bodyWeight = (y, bend) => bend ? 1 - smooth(bend[0], bend[1], y) : 1;

// A rigid hinged plane: the jamb stays fixed while the free edge slowly
// changes depth. Unlike a body mesh, no door pixel is locally deformed.
export function doorPose(seconds, still = false, response = 0) {
  const t=((seconds%LOOP)+LOOP)%LOOP;
  const opening = wave(t,.4,2.1,2.4,3.3)+.45*wave(t,13.6,2.4,.8,3.4);
  return still ? [1, 0] : [Math.max(.32, Math.min(1.22, 1 - .5 * opening + response)), .052 * opening - response * .06];
}

export function weights(actor, u, v) {
  let body = bodyWeight(v, actor.bend);
  if (actor.rigidZone) {
    const [left,right,top,bottom] = actor.rigidZone;
    body = Math.max(body, smooth(left,right,u) * (1-smooth(top,bottom,v)));
  }
  let head = 0, arm = 0;
  if (actor.head) {
    const [left,right,bottom,fade] = actor.head;
    head = smooth(left-.08,left,u) * (1-smooth(right,right+.08,u)) * (1-smooth(bottom,fade,v));
  }
  if (actor.arm) {
    const [left,right,top,bottom] = actor.arm;
    arm = smooth(left-.12,left,u) * (1-smooth(right,right+.12,u)) * smooth(top-.1,top,v) * (1-smooth(bottom,bottom+.06,v));
  }
  return [body, head, arm * (1 - head)];
}

export function poses(seconds, reactions = {}, still = false, objects = OBJECTS) {
  const t = ((seconds % LOOP) + LOOP) % LOOP;
  const engage = wave(t,.6,1.8,2.6,2.7);
  const answer = wave(t,3.2,1.7,2.2,2.9);
  const read = wave(t,10,1.8,3.1,3);
  const point = wave(t,10.4,1.5,2.1,2.7);
  const write = wave(t,.15,.95,5.8,1.5)+wave(t,11,1.3,4.4,2);
  const result = {};
  for (const a of objects) {
    const rock = Math.sin(t/LOOP*Math.PI*2+(a.phase??0))-Math.sin(a.phase??0);
    let tilt = 0, head = 0, arm = 0;
    switch(a.id) {
      case 'clerk': tilt=(3.6+.9*Math.sin(t*2))*write; head=1.1*write; break;
      case 'assistant': tilt=5.8*engage+4.9*point+.65*rock; head=-1.8*point; arm=3.8*point; break;
      case 'lawyer': tilt=-4.4*answer-5.8*read+.4*rock; head=-1.5*answer+2*read; break;
      case 'petitioner': tilt=-4.6*engage+2.5*answer+.85*rock; head=1.8*engage; break;
      case 'hatVisitor': tilt=-2*engage+2.7*answer+1.05*rock; head=-1.5*answer; break;
      case 'client': tilt=3.5*answer-2*read+1*rock; head=-1.7*answer; break;
      case 'waitingRear': tilt=-2.7*engage+1*rock; head=1.4*engage; break;
      case 'waitingFront': tilt=2.4*answer+.9*rock; head=-1.7*answer; break;
      case 'leftFront': tilt=1.1*rock+1.8*read; head=-1.1*read; break;
      case 'leftRear': tilt=1*rock+1.6*engage; head=1.1*engage; break;
      case 'basketWoman': tilt=-1.15*write+.75*rock; head=.9*write; break;
      case 'handHat': tilt=1.6*engage-1.2*answer; break;
      // Onboarding's Kitsune leans into the talk from its ankles and nods
      // along, its body staying one solid: no bend at the belly or hips, and
      // its paws keep the book still. Every extra motion closes on the loop,
      // so the loop joins seamlessly.
      case 'kitsune': {
        const nod=Math.sin(t/LOOP*Math.PI*2*8), sway=Math.sin(t/LOOP*Math.PI*2*3+1.3);
        tilt=-2*engage+.8*answer+.9*sway+.3*rock; head=1.8*engage-1.8*read+.7*nod; arm=0; break;
      }
    }
    const response = (reactions[a.id] ?? [0,0]).map(n=>n*(a.responseScale??1));
    result[a.id] = still ? [rad(a.restAngle??0),0,0,0,0] : [rad((a.restAngle??0)+Math.max(-7,Math.min(7,tilt+response[0]))), response[1], 0, rad(head), rad(arm)];
  }
  return result;
}

const rotate = (p,pivot,angle) => { const x=p[0]-pivot[0],y=p[1]-pivot[1],c=Math.cos(angle),s=Math.sin(angle); return [pivot[0]+c*x-s*y,pivot[1]+s*x+c*y]; };
export function transformPoint(point, actor, pose) {
  const [u,v]=point, w=actor.width,h=actor.height;
  const [body,head,arm]=weights(actor,u,v);
  let p=[u*w,v*h];
  if(actor.joint) { const q=rotate(p,[actor.joint[0]*w,actor.joint[1]*h],pose[4]); p=p.map((n,i)=>n+arm*(q[i]-n)); }
  if(actor.neck) { const q=rotate(p,[actor.neck[0]*w,actor.neck[1]*h],pose[3]); p=p.map((n,i)=>n+head*(q[i]-n)); }
  const q=rotate(p,[actor.pivot[0]*w,actor.pivot[1]*h],pose[0]);
  return p.map((n,i)=>n+body*(q[i]+pose[i+1]-n));
}
