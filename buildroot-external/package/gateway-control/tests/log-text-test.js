'use strict';
const assert=require('node:assert/strict');
const {render}=require('../../../board/gateway/rootfs-overlay/www/log-text.js');
const encoder=new TextEncoder();
const label=(utc,uptime)=>utc||`启动后 ${uptime.toFixed(3)} 秒`;
const perByte=bytes=>Array.from(bytes,(_,offset)=>({offset,count:1,uptime:1+offset/1000,utc:''}));
const prefix=offset=>`[启动后 ${(1+offset/1000).toFixed(3)} 秒 · @${offset}] `;

// Each byte arrives separately, including Chinese characters and CR/LF pairs.
const content='One complete sentence.\r\n中文温度正常。\r\ntail without LF';
const bytes=encoder.encode(content),second=encoder.encode('One complete sentence.\r\n').length;
const third=encoder.encode('One complete sentence.\r\n中文温度正常。\r\n').length;
assert.equal(render(bytes,0,perByte(bytes),label),prefix(0)+'One complete sentence.\n'+prefix(second)+'中文温度正常。\n'+prefix(third)+'tail without LF');

// Growing a live partial line keeps its existing timestamp and joins the suffix.
const partial=encoder.encode('not yet complete');
assert.equal(render(partial,0,perByte(partial),label),prefix(0)+'not yet complete');
const full=encoder.encode('not yet complete, now complete.\n');
assert.equal(render(full,0,perByte(full),label),prefix(0)+'not yet complete, now complete.\n');

// Index gaps never hide received bytes or manufacture a later timestamp.
assert.equal(render(encoder.encode('raw\n'),0,[],label),'[时间戳缺失 · @0] raw\n');
assert.equal(render(encoder.encode('ab\ncd'),0,[{offset:1,count:4,utc:'later'}],label),'[时间戳缺失 · @0] ab\n[later · @3] cd');
assert.equal(render(Uint8Array.of(0,255,10),0,[],label),'[时间戳缺失 · @0] ␀�\n');
assert.equal(render(new Uint8Array(),0,[],label),'');

// CR-only, empty lines, and page boundaries do not create false CRLF blank rows.
const endings=encoder.encode('a\rb\n\r\n');
assert.equal(render(endings,0,perByte(endings),label),prefix(0)+'a\n'+prefix(2)+'b\n'+prefix(4)+'\n');
assert.equal(render(encoder.encode('tail\nnext'),8,[{offset:0,count:30,utc:'time'}],label,97),'[续行 · @8] tail\n[time · @13] next');
assert.equal(render(Uint8Array.of(10,66,10),2,[{offset:0,count:10,utc:'time'}],label,13),'[time · @3] B\n');

const long=encoder.encode('x'.repeat(8191)+'汉\r\nend');
const rendered=render(long,0,[{offset:0,count:long.length,utc:'time'}],label);
assert.ok(rendered.includes('x'.repeat(8191)+'汉\n'));
assert.equal((rendered.match(/\[time · @/g)||[]).length,2);
console.log('Text log rendering: split receives, UTF-8, CRLF, live partial lines, missing index, controls, page continuations and long lines: PASS');
