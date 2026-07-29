#!/usr/bin/env node
import { readFileSync, writeFileSync } from 'node:fs';

const [, , sourcePath, outputPath, iconPath, backgroundPath] = process.argv;
if (!sourcePath || !outputPath || !iconPath || !backgroundPath) {
  console.error('Uso: node tools/repack-pbp.mjs input.pbp output.pbp ICON0.PNG PIC1.PNG');
  process.exit(2);
}

const source = readFileSync(sourcePath);
if (source.subarray(0, 4).toString('binary') !== '\0PBP') throw new Error('PBP non valido');

const oldOffsets = Array.from({ length: 8 }, (_, index) => source.readUInt32LE(8 + index * 4));
const oldEnd = [...oldOffsets.slice(1), source.length];
const sections = oldOffsets.map((offset, index) => source.subarray(offset, oldEnd[index]));
sections[1] = readFileSync(iconPath);
sections[4] = readFileSync(backgroundPath);
sections[6] = Buffer.from(sections[6]);
const oldVersion = Buffer.from('UTUBBU 0.2', 'ascii');
const versionOffset = sections[6].indexOf(oldVersion);
if (versionOffset >= 0) sections[6].write('UTUBBU 0.3', versionOffset, 'ascii');

const header = Buffer.alloc(40);
source.copy(header, 0, 0, 8);
let offset = header.length;
for (let index = 0; index < sections.length; index += 1) {
  header.writeUInt32LE(offset, 8 + index * 4);
  offset += sections[index].length;
}
writeFileSync(outputPath, Buffer.concat([header, ...sections]));
console.log(`${outputPath}: ${offset} byte`);
