// Probe: boot, report select/back keycodes, drive ticks + key presses,
// and write a horizontal filmstrip PNG of sampled frames.
import fs from 'fs';
import path from 'path';
import zlib from 'zlib';
import { fileURLToPath } from 'url';
import { RG } from './rg.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(HERE, '..');
const ORIG = path.join(ROOT, 'original', 'ResetGeneration');
const ENTRY = 0x1014f74e;
const PAKS = ['base.pak', 'fonts.pak', 'menus.pak', 'intro.pak', 'levels.pak',
              'gameplay.pak', 'gameplay2.pak', 'story.pak', 'fight.pak'];

function crc32(buf){let c=~0;for(let i=0;i<buf.length;i++){c^=buf[i];for(let k=0;k<8;k++)c=(c>>>1)^(0xEDB88320&-(c&1));}return ~c;}
function png(file,w,h,rgba){
  const raw=Buffer.alloc((w*4+1)*h);
  for(let y=0;y<h;y++){raw[y*(w*4+1)]=0;Buffer.from(rgba.buffer,rgba.byteOffset+y*w*4,w*4).copy(raw,y*(w*4+1)+1);}
  const idat=zlib.deflateSync(raw);const out=[Buffer.from([137,80,78,71,13,10,26,10])];
  const ch=(t,d)=>{const l=Buffer.alloc(4);l.writeUInt32BE(d.length,0);const tt=Buffer.from(t);const cr=Buffer.alloc(4);cr.writeUInt32BE(crc32(Buffer.concat([tt,d]))>>>0,0);out.push(l,tt,d,cr);};
  const ih=Buffer.alloc(13);ih.writeUInt32BE(w,0);ih.writeUInt32BE(h,4);ih[8]=8;ih[9]=6;ch('IHDR',ih);ch('IDAT',idat);ch('IEND',Buffer.alloc(0));
  fs.writeFileSync(file,Buffer.concat(out));
}

async function main(){
  const rg=new RG(); let nlog=0;
  rg.onLog=(s)=>{ if(nlog++<60) console.log('[wasm]',s); };
  rg.onAbort=(s)=>{ console.error('[ABORT]',s); throw new Error(s); };
  await rg.load(fs.readFileSync(path.join(ROOT,'build','reset.wasm')));
  rg.setup();
  rg.loadImage(fs.readFileSync(path.join(ROOT,'runtime','rg_image.bin')));
  for(const p of PAKS){const fp=path.join(ORIG,p);if(fs.existsSync(fp))rg.addFile(p,fs.readFileSync(fp));}
  for(const f of ['settings.cfg','single_player_game.save','achievements.dat']){const fp=path.join(ORIG,f);if(fs.existsSync(fp))rg.addFile(f,fs.readFileSync(fp));}
  rg.boot(ENTRY);
  rg.init(240,320,0);
  for(const p of PAKS) rg.notifyPak(p);
  console.log('select key =', rg.selectKey(), 'back key =', rg.backKey());

  const W=240,H=320, shots=[], SAMPLE=[200,400,600,800];
  // schedule: tick a while, then press select to skip intro, continue
  const press=(code)=>{ rg.keyDown(Date.now(),code,0,1); for(let k=0;k<3;k++)rg.tick(); rg.keyUp(Date.now(),code,0,1); };
  const maxT=900;
  for(let i=0;i<maxT;i++){
    rg.tick();
    if(i===500){ console.log('pressing select', rg.selectKey()); press(rg.selectKey()||10); }
    if(i===700){ console.log('pressing enter'); press(10); }
    if(SAMPLE.includes(i)) shots.push(rg.frame());
  }
  shots.push(rg.frame());
  // build filmstrip
  const n=shots.length, strip=new Uint8ClampedArray(W*n*H*4);
  for(let s=0;s<n;s++) for(let y=0;y<H;y++) for(let x=0;x<W;x++){
    const si=(y*W+x)*4, di=(y*(W*n)+s*W+x)*4;
    strip[di]=shots[s][si];strip[di+1]=shots[s][si+1];strip[di+2]=shots[s][si+2];strip[di+3]=255;
  }
  png(path.join(ROOT,'build','filmstrip.png'),W*n,H,strip);
  console.log('wrote filmstrip with',n,'frames');
}
main().catch(e=>{console.error(e);process.exit(1);});
