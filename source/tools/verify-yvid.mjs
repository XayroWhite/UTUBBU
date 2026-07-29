#!/usr/bin/env node
import { closeSync, fstatSync, openSync, readSync } from 'node:fs';

const path = process.argv[2];
if (!path) throw new Error('Uso: node tools/verify-yvid.mjs file.yvid');
const fd = openSync(path, 'r');
const size = fstatSync(fd).size;
const header = Buffer.alloc(32);
readSync(fd, header, 0, 32, 0);
if (header.toString('ascii', 0, 8) !== 'UTUBVID\0') throw new Error('Magic YVID non valida');
if (header.readUInt32LE(8) !== 1) throw new Error('Versione YVID non supportata');
const frames = header.readUInt32LE(28);
const channels = header.readUInt16LE(24);
if (channels !== 1 && channels !== 2) throw new Error(`Canali non supportati: ${channels}`);
let offset = 32;
for (let i = 0; i < frames; i++) {
  const record = Buffer.alloc(16);
  if (readSync(fd, record, 0, 16, offset) !== 16) throw new Error(`Record ${i} troncato`);
  const jpegBytes = record.readUInt32LE(0);
  const samples = record.readUInt32LE(4);
  const pcmBytes = record.readUInt32LE(8);
  if (!jpegBytes || jpegBytes > 1048576) throw new Error(`JPEG ${i} fuori limite`);
  if (samples < 17 || samples > 4111 || pcmBytes !== samples * channels * 2) throw new Error(`Audio ${i} non valido`);
  offset += 16 + jpegBytes + pcmBytes;
  if (offset > size) throw new Error(`Payload ${i} troncato`);
}
closeSync(fd);
if (offset !== size) throw new Error(`${size - offset} byte extra dopo l'ultimo frame`);
console.log(`OK: ${frames} frame, ${size} byte, struttura completa`);
