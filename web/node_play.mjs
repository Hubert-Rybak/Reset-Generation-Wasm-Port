// Drive deeper into the game to reach the puzzle board; dump a filmstrip.
import fs from 'fs'; import path from 'path'; import zlib from 'zlib';
import { fileURLToPath } from 'url';
import { RG } from './rg.js';
const HERE = path.dirname(fileURLToPath(import.meta.url)), ROOT = path.join(HERE, '..');
const ORIG = path.join(ROOT, 'original', 'ResetGeneration');
const PAKS = ['base.pak','fonts.pak','menus.pak','intro.pak','levels.pak','gameplay.pak','gameplay2.pak','story.pak','fight.pak'];
function crc32(b){let c=~0;for(let i=0;i<b.length;i++){c^=b[i];for(let k=0;k<8;k++)c=(c>>>1)^(0xEDB88320&-(c&1));}return ~c;}
function png(f,w,h,rgba){const raw=Buffer.alloc((w*4+1)*h);for(let y=0;y<h;y++){raw[y*(w*4+1)]=0;Buffer.from(rgba.buffer,rgba.byteOffset+y*w*4,w*4).copy(raw,y*(w*4+1)+1);}const idat=zlib.deflateSync(raw);const out=[Buffer.from([137,80,78,71,13,10,26,10])];const ch=(t,d)=>{const l=Buffer.alloc(4);l.writeUInt32BE(d.length,0);const tt=Buffer.from(t);const cr=Buffer.alloc(4);cr.writeUInt32BE(crc32(Buffer.concat([tt,d]))>>>0,0);out.push(l,tt,d,cr);};const ih=Buffer.alloc(13);ih.writeUInt32BE(w,0);ih.writeUInt32BE(h,4);ih[8]=8;ih[9]=6;ch('IHDR',ih);ch('IDAT',idat);ch('IEND',Buffer.alloc(0));fs.writeFileSync(f,Buffer.concat(out));}

const rg = new RG(); rg.onLog=()=>{}; rg.onAbort=s=>{throw new Error(s)};
await rg.load(fs.readFileSync(ROOT+'/build/reset.wasm')); rg.setup();
rg.loadImage(fs.readFileSync(ROOT+'/runtime/rg_image.bin'));
for(const p of PAKS) rg.addFile(p, fs.readFileSync(path.join(ORIG,p)));
for(const f of ['settings.cfg','single_player_game.save','achievements.dat']){const fp=path.join(ORIG,f);if(fs.existsSync(fp))rg.addFile(f,fs.readFileSync(fp));}
rg.boot(0x1014f74e); rg.init(240,320,0); for(const p of PAKS) rg.notifyPak(p);
const SEL=rg.selectKey()||17, BACK=rg.backKey()||16, DOWN=40, UP=38, LEFT=37, RIGHT=39;
const tickN=(n)=>{for(let i=0;i<n;i++)rg.tick();};
const press=(code,hold=4)=>{rg.keyDown(Date.now(),code,0xffff,1);tickN(hold);rg.keyUp(Date.now(),code,0xffff,1);tickN(4);};
const shots=[]; const snap=()=>shots.push(rg.frame());

tickN(500);                    // intro -> main menu
press(SEL); tickN(60);         // enter Single-Player submenu
snap();
press(SEL); tickN(120);        // choose Story Mode (top item)
snap();
for (let i=0;i<4;i++){ press(SEL); tickN(120); snap(); }  // advance through char/level selects
tickN(200); snap();            // settle on gameplay
// try a few directional + select moves on the board
press(RIGHT); press(DOWN); press(SEL); tickN(120); snap();
press(LEFT); press(SEL); tickN(120); snap();

const n=shots.length, W=240, H=320, strip=new Uint8ClampedArray(W*n*H*4);
for(let s=0;s<n;s++)for(let y=0;y<H;y++)for(let x=0;x<W;x++){const si=(y*W+x)*4,di=(y*(W*n)+s*W+x)*4;strip[di]=shots[s][si];strip[di+1]=shots[s][si+1];strip[di+2]=shots[s][si+2];strip[di+3]=255;}
png(ROOT+'/build/playthrough.png',W*n,H,strip);
console.log('wrote playthrough with',n,'frames; select=',SEL,'back=',BACK);
