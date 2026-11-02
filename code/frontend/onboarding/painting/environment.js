import { LOOP } from './scene.js';

const clamp = (n, limit) => Math.max(-limit, Math.min(limit, n));
const radians = n => n * Math.PI / 180;
const profiles = {
  hanging: { stiffness: 25, damping: 4.8, angle: 10, travel: 0 },
  heavy: { stiffness: 44, damping: 9, angle: 1.7, travel: 2 },
  paper: { stiffness: 8, damping: 3.6, angle: 26, travel: 75 },
  door: { stiffness: 19, damping: 5.6, angle: 1, travel: 0 },
};

// Rigid props: short spring substeps keep trackpad bursts and slow frames stable.
export class EnvironmentMotion {
  constructor(objects) {
    this.objects = objects.filter(a => a.environment);
    this.reset();
  }
  reset() {
    this.state = Object.fromEntries(this.objects.map(a => [a.id, { angle: 0, velocity: 0, x: 0, speed: 0, y: 0, speedY: 0 }]));
    this.shake = 0;
    this.shakeSpeed = 0;
    this.releaseEnergy = 0;
    this.falls = {};
  }
  impulse(amount, seconds = 0) {
    const kick = clamp(amount, 1);
    for (const a of this.objects) {
      if (this.falls[a.id]) continue;
      const p = profiles[a.environment], s = this.state[a.id];
      const direction = a.phase % 2 > 1 ? -1 : 1;
      s.velocity = clamp(s.velocity + kick * p.angle * 3 * direction, p.angle * 5);
      s.speed = clamp(s.speed + kick * p.travel * 5 * (a.environment === 'paper' ? a.direction[0] : 1), p.travel * 7);
      if (a.environment === 'paper') s.speedY = clamp(s.speedY + kick * 100 * a.direction[1], 140);
    }
    this.shakeSpeed = clamp(this.shakeSpeed + kick * 6, 10);
    this.releaseEnergy += Math.abs(kick) * .28;
    if (this.releaseEnergy > .85) {
      this.release('wallBag', seconds, kick);
      this.releaseEnergy = 0;
    }
  }
  release(id, seconds, direction = 1) {
    const a = this.objects.find(object => object.id === id);
    if (!a?.fallFloor || this.falls[id]) return false;
    const pose = this.pose(a, seconds);
    this.state[id] = { angle: 0, velocity: 0, x: 0, speed: 0, y: 0, speedY: 0 };
    this.falls[id] = { x: pose[1], y: pose[2], angle: pose[0], vx: 26 * direction, vy: 0, spin: .5 * direction, grounded: false };
    return true;
  }
  touch(id, direction) {
    const a = this.objects.find(object => object.id === id);
    if (!a || this.falls[id]) return;
    const p = profiles[a.environment], s = this.state[id];
    s.velocity = clamp(s.velocity + (direction || .45) * p.angle * 2, p.angle * 5);
    s.speed = clamp(s.speed + (direction || .45) * p.travel * 2, p.travel * 7);
  }
  brush(id, dx, dy) {
    const a = this.objects.find(object => object.id === id);
    if (a?.environment !== 'paper') return;
    const s = this.state[id];
    s.speed = clamp(s.speed + dx * 150, 340);
    s.speedY = clamp(s.speedY + dy * 65, 140);
    s.velocity = clamp(s.velocity + dx * 45, 95);
  }
  step(delta, target, still = false) {
    if (still) { this.reset(); return; }
    let remaining = Math.min(.1, Math.max(0, delta));
    while (remaining > 0) {
      const dt = Math.min(1 / 120, remaining);
      this.releaseEnergy *= Math.exp(-dt * 1.6);
      for (const a of this.objects) {
        const fall = this.falls[a.id];
        if (fall) {
          if (fall.grounded) continue;
          fall.vy += 980 * dt;
          fall.x += fall.vx * dt;
          fall.y += fall.vy * dt;
          fall.angle += fall.spin * dt;
          const width = a.width ?? a.height * .65;
          const pivot = [a.pivot[0] * width, a.pivot[1] * a.height];
          const bottom = Math.max(...[[0,0],[width,0],[0,a.height],[width,a.height]].map(([x,y]) => pivot[1] + (x-pivot[0])*Math.sin(fall.angle) + (y-pivot[1])*Math.cos(fall.angle)));
          if (a.y + fall.y + bottom >= a.fallFloor && fall.vy > 0) {
            fall.y = a.fallFloor - a.y - bottom;
            fall.vy *= -.18;
            fall.vx *= .45;
            fall.spin *= -.3;
            if (Math.abs(fall.vy) < 24) { fall.grounded = true; fall.vy = 0; fall.vx = 0; fall.spin = 0; }
          }
          continue;
        }
        const p = profiles[a.environment], s = this.state[a.id];
        if (a.environment === 'paper') {
          // Loose sheets keep their landing position. Friction removes energy;
          // an invisible spring does not pull them back across the room.
          s.speed *= Math.exp(-dt * 2.5);
          s.speedY *= Math.exp(-dt * 3.1);
          s.velocity *= Math.exp(-dt * 3.8);
          s.x += s.speed * dt;
          s.y += s.speedY * dt;
          s.angle += s.velocity * dt;
          if (Math.abs(s.x) > 120) { s.x = clamp(s.x, 120); s.speed *= -.08; }
          if (Math.abs(s.y) > 35) { s.y = clamp(s.y, 35); s.speedY *= -.08; }
          s.angle = clamp(s.angle, 32);
          continue;
        }
        const active = target?.id === a.id;
        const wantedAngle = active ? target.x * p.angle : 0;
        const wantedX = active ? target.x * p.travel : 0;
        s.velocity += ((wantedAngle - s.angle) * p.stiffness - s.velocity * p.damping) * dt;
        s.speed += ((wantedX - s.x) * p.stiffness - s.speed * p.damping) * dt;
        s.angle = clamp(s.angle + s.velocity * dt, p.angle * 1.6);
        s.x = clamp(s.x + s.speed * dt, p.travel * 1.6);
      }
      this.shakeSpeed += (-this.shake * 60 - this.shakeSpeed * 10) * dt;
      this.shake = clamp(this.shake + this.shakeSpeed * dt, 1.5);
      remaining -= dt;
    }
  }
  pose(a, seconds, still = false) {
    if (still) return [0, 0, 0, 0, 0];
    const fall = this.falls[a.id];
    if (fall) return [fall.angle, fall.x, fall.y, 0, 0];
    const s = this.state[a.id];
    if (a.environment === 'paper') {
      const phase = seconds / LOOP * Math.PI * 2;
      const breeze = Math.sin(phase * 2 + a.phase) - Math.sin(a.phase);
      const eddy = Math.sin(phase + a.phase + .8) - Math.sin(a.phase + .8);
      const [dx,dy] = a.direction;
      return [radians(s.angle + 2.8 * breeze + 1.2 * eddy), s.x + 17 * breeze * dx, s.y + 8 * eddy * dy, 0, 0];
    }
    const rock = Math.sin(seconds / LOOP * Math.PI * 2 + a.phase) - Math.sin(a.phase);
    const amplitude = a.environment === 'hanging' ? 1.4 : .25;
    return [radians(s.angle + amplitude * rock), s.x, 0, 0, 0];
  }
  door() { return clamp(this.state.door?.angle ?? 0, 1) * .3; }
  paper(a, seconds, still = false) {
    if (still || a.environment !== 'paper') return [0, 0, 0];
    const s = this.state[a.id];
    const energy = Math.min(1, (Math.abs(s.speed) + Math.abs(s.speedY)) / 180);
    const breath = Math.sin(seconds / LOOP * Math.PI * 4 + a.phase);
    return [energy * 7, energy * .18, (1.1 + energy * 2.4) * breath];
  }
}
