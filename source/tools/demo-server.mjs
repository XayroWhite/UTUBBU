#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { createReadStream, existsSync, mkdirSync, readdirSync, rmSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const mediaRoot = resolve(process.argv[2] || 'release/UTUBBU');
const port = Number(process.argv[3] || 8765);
const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const runtimeRoot = join(projectRoot, '.runtime');
const cacheRoot = join(runtimeRoot, 'cache-tiny');
const workRoot = join(runtimeRoot, 'work');
const ytDlp = join(runtimeRoot, process.platform === 'win32' ? 'yt-dlp.exe' : 'yt-dlp');
const buildYvid = join(projectRoot, 'tools', 'build-yvid.mjs');
mkdirSync(cacheRoot, { recursive: true });
mkdirSync(workRoot, { recursive: true });

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: projectRoot,
    encoding: 'utf8',
    maxBuffer: 32 * 1024 * 1024,
    ...options
  });
  if (result.status !== 0) {
    const detail = (result.stderr || result.stdout || '').trim().split('\n').slice(-3).join(' ');
    throw new Error(`${basename(command)} fallito (${result.status}): ${detail}`);
  }
  return result.stdout || '';
}

function asciiTitle(value) {
  return String(value || 'Video senza titolo')
    .normalize('NFD').replace(/[\u0300-\u036f]/g, '')
    .replace(/[^\x20-\x7E]/g, ' ').replace(/[|\r\n]+/g, ' ')
    .replace(/\s+/g, ' ').trim().slice(0, 90) || 'Video senza titolo';
}

function sendText(response, status, text) {
  const body = Buffer.from(text, 'utf8');
  response.writeHead(status, {
    'Content-Type': 'text/plain; charset=utf-8',
    'Content-Length': body.length,
    'Cache-Control': 'no-store'
  });
  response.end(body);
}

function sendFile(request, response, path) {
  const size = statSync(path).size;
  response.writeHead(200, {
    'Content-Type': 'application/octet-stream',
    'Content-Length': size,
    'Cache-Control': 'no-store'
  });
  if (request.method === 'HEAD') response.end();
  else createReadStream(path).pipe(response);
  console.log(`${request.socket.remoteAddress} ${request.method} ${basename(path)} ${size}`);
}

function searchCatalog(request, response, query) {
  if (!existsSync(ytDlp)) throw new Error(`yt-dlp non trovato: ${ytDlp}`);
  const term = query.trim() || 'PSP homebrew';
  const output = run(ytDlp, [
    '--ignore-config', '--flat-playlist', '--dump-single-json',
    '--playlist-end', '24', `ytsearch24:${term}`
  ]);
  const data = JSON.parse(output);
  const host = request.headers.host || `127.0.0.1:${port}`;
  const lines = (data.entries || [])
    .filter(item => item && /^[A-Za-z0-9_-]{6,20}$/.test(item.id || ''))
    .filter(item => item.live_status !== 'is_live' &&
      (!Number.isFinite(item.duration) || item.duration <= 600))
    .slice(0, 8)
    .map(item => `${asciiTitle(item.title)}|http://${host}/video/${item.id}.yvid|yt-${item.id}.yvid`);
  sendText(response, 200, `# Risultati YouTube per: ${asciiTitle(term)}\n${lines.join('\n')}\n`);
}

function ensureConverted(videoId) {
  const output = join(cacheRoot, `${videoId}.yvid`);
  if (existsSync(output)) return output;
  const prefix = `source-${videoId}`;
  for (const name of readdirSync(workRoot)) {
    if (name.startsWith(prefix)) rmSync(join(workRoot, name), { force: true });
  }
  const input = join(workRoot, `${prefix}.mp4`);
  run(ytDlp, [
    '--ignore-config', '--no-playlist',
    '--match-filter', 'duration <= 600 & !is_live',
    '-f', 'b[height<=360]/bv*[height<=360]+ba',
    '--merge-output-format', 'mp4',
    '-o', input,
    `https://www.youtube.com/watch?v=${videoId}`
  ], { stdio: 'inherit', encoding: undefined });
  run(process.execPath, [buildYvid, input, output], { stdio: 'inherit', encoding: undefined });
  rmSync(input, { force: true });
  return output;
}

const server = createServer((request, response) => {
  try {
    const url = new URL(request.url, `http://${request.headers.host || '127.0.0.1'}`);
    if (url.pathname === '/search') {
      searchCatalog(request, response, url.searchParams.get('q') || '');
      return;
    }
    const videoMatch = url.pathname.match(/^\/video\/([A-Za-z0-9_-]{6,20})\.yvid$/);
    if (videoMatch) {
      sendFile(request, response, ensureConverted(videoMatch[1]));
      return;
    }
    if (url.pathname === '/psp-test-480p.yvid') {
      sendFile(request, response, join(mediaRoot, 'psp-test-480p.yvid'));
      return;
    }
    sendText(response, 404, 'Not found\n');
  } catch (error) {
    console.error(error);
    sendText(response, 500, `${error.message}\n`);
  }
});
server.requestTimeout = 0;
server.headersTimeout = 0;
server.listen(port, '0.0.0.0', () => {
  console.log(`UTUBBU search: http://127.0.0.1:${port}/search?q=psp`);
});
