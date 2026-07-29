#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { closeSync, mkdtempSync, openSync, readFileSync, readSync, readdirSync, rmSync, statSync, writeSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const [, , inputArg, outputArg] = process.argv;
if (!inputArg || !outputArg) {
  console.error('Uso: node tools/build-yvid.mjs input.mp4 output.yvid');
  process.exit(2);
}

const input = resolve(inputArg);
const output = resolve(outputArg);
const width = 480;
const height = 272;
const fpsNum = 8;
const fpsDen = 1;
const sampleRate = 11025;
const channels = 1;
const work = mkdtempSync(join(tmpdir(), 'utubbu-yvid-'));

function ffmpeg(args) {
  const result = spawnSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', ...args], {
    stdio: 'inherit'
  });
  if (result.status !== 0) throw new Error(`ffmpeg terminato con codice ${result.status}`);
}

try {
  ffmpeg([
    '-i', input, '-vf', `fps=${fpsNum},scale=${width}:${height}:force_original_aspect_ratio=decrease,pad=${width}:${height}:(ow-iw)/2:(oh-ih)/2`,
    '-q:v', '20', '-pix_fmt', 'yuvj420p', join(work, 'frame-%08d.jpg')
  ]);
  ffmpeg([
    '-i', input, '-vn', '-ac', String(channels), '-ar', String(sampleRate),
    '-f', 's16le', join(work, 'audio.pcm')
  ]);

  const frames = readdirSync(work).filter((name) => name.endsWith('.jpg')).sort();
  const pcmPath = join(work, 'audio.pcm');
  const pcmSize = statSync(pcmPath).size;
  const header = Buffer.alloc(32);
  header.write('UTUBVID\0', 0, 'ascii');
  header.writeUInt32LE(1, 8);
  header.writeUInt16LE(width, 12);
  header.writeUInt16LE(height, 14);
  header.writeUInt16LE(fpsNum, 16);
  header.writeUInt16LE(fpsDen, 18);
  header.writeUInt32LE(sampleRate, 20);
  header.writeUInt16LE(channels, 24);
  header.writeUInt16LE(0, 26);
  header.writeUInt32LE(frames.length, 28);

  const outputFd = openSync(output, 'w');
  const pcmFd = openSync(pcmPath, 'r');
  writeSync(outputFd, header);
  let outputBytes = header.length;
  let consumedSamples = 0;
  try {
    for (let i = 0; i < frames.length; i++) {
      const jpeg = readFileSync(join(work, frames[i]));
      const targetSamples = Math.round(((i + 1) * sampleRate * fpsDen) / fpsNum);
      const samples = targetSamples - consumedSamples;
      const pcmStart = consumedSamples * channels * 2;
      const audio = Buffer.alloc(samples * channels * 2);
      if (pcmStart < pcmSize) readSync(pcmFd, audio, 0, Math.min(audio.length, pcmSize - pcmStart), pcmStart);
      consumedSamples = targetSamples;

      const record = Buffer.alloc(16);
      record.writeUInt32LE(jpeg.length, 0);
      record.writeUInt32LE(samples, 4);
      record.writeUInt32LE(audio.length, 8);
      record.writeUInt32LE(Math.round((i * 1000 * fpsDen) / fpsNum), 12);
      writeSync(outputFd, record);
      writeSync(outputFd, jpeg);
      writeSync(outputFd, audio);
      outputBytes += record.length + jpeg.length + audio.length;
    }
  } finally {
    closeSync(pcmFd);
    closeSync(outputFd);
  }
  console.log(`${output}: ${frames.length} frame, ${(outputBytes / 1048576).toFixed(2)} MiB`);
} finally {
  rmSync(work, { recursive: true, force: true });
}
