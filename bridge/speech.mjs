//
// Text to speech for the device's spoken alerts.
//
// The ESP32-S3 has a speaker and an ES8311 codec but nothing that can turn a
// sentence into audio, so the phrase is synthesised here with whatever voice
// the OS already ships and handed over as a plain PCM WAV. No cloud service,
// no API key, no per-call cost.
//
// Output is always 16 kHz / 16-bit / mono, which is what the firmware's I2S
// path is configured for and keeps a five second phrase under 200 KB.
//
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import { execFile } from 'node:child_process';

export const SAMPLE_RATE = 16000;

function run(cmd, args, timeoutMs = 20000) {
  return new Promise((resolve, reject) => {
    execFile(cmd, args, { timeout: timeoutMs, windowsHide: true }, (err, stdout, stderr) =>
      err ? reject(new Error(stderr?.toString().trim() || err.message)) : resolve(stdout),
    );
  });
}

// PowerShell string literal: single-quoted, with '' escaping an apostrophe.
function psQuote(s) {
  return `'${String(s).replace(/'/g, "''")}'`;
}

async function synthWindows(text, outFile, voice, lang) {
  const wanted = voice || (lang === 'es' ? 'es-' : 'en-');
  // Picking by culture prefix rather than an exact name keeps this working on
  // machines with a different set of voices installed.
  const script = `
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$want = ${psQuote(wanted)}
$v = $s.GetInstalledVoices() | Where-Object {
       $_.VoiceInfo.Name -eq $want -or $_.VoiceInfo.Culture.Name -like ($want + '*')
     } | Select-Object -First 1
if ($v) { $s.SelectVoice($v.VoiceInfo.Name) }
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(${SAMPLE_RATE}, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$s.SetOutputToWaveFile(${psQuote(outFile)}, $fmt)
$s.Speak(${psQuote(text)})
$s.Dispose()
`;
  await run('powershell', ['-NoProfile', '-NonInteractive', '-Command', script]);
}

async function synthMac(text, outFile, voice) {
  const args = ['-o', outFile, '--data-format=LEI16@' + SAMPLE_RATE];
  if (voice) args.push('-v', voice);
  args.push(text);
  await run('say', args);
}

async function synthLinux(text, outFile, voice, lang) {
  await run('espeak-ng', ['-w', outFile, '-s', '150', '-v', voice || lang || 'en', text]);
}

// Walks the RIFF chunk list instead of assuming the data starts at byte 44.
// SAPI emits an 18-byte `fmt ` chunk, which puts the samples at 46.
export function wavInfo(buf) {
  if (buf.length < 44 || buf.toString('ascii', 0, 4) !== 'RIFF') return null;

  let off = 12;
  let fmt = null;
  while (off + 8 <= buf.length) {
    const id = buf.toString('ascii', off, off + 4);
    const size = buf.readUInt32LE(off + 4);
    const body = off + 8;

    if (id === 'fmt ') {
      fmt = {
        channels: buf.readUInt16LE(body + 2),
        sampleRate: buf.readUInt32LE(body + 4),
        bits: buf.readUInt16LE(body + 14),
      };
    } else if (id === 'data') {
      return { ...fmt, dataOffset: body, dataSize: Math.min(size, buf.length - body) };
    }
    off = body + size + (size % 2); // chunks are word aligned
  }
  return null;
}

export class Speech {
  constructor(config, cacheDir) {
    this.config = config;
    this.cacheDir = cacheDir;
    this.available = null; // resolved lazily on first use
  }

  // Same sentence, same file — alerts repeat, so synthesis should not.
  cachePath(text) {
    const key = crypto
      .createHash('sha1')
      .update(`${text}|${this.config.voice}|${this.config.lang}|${SAMPLE_RATE}`)
      .digest('hex')
      .slice(0, 16);
    return path.join(this.cacheDir, `${key}.wav`);
  }

  async synthesize(text) {
    if (!this.config.enabled) throw new Error('speech disabled');

    const out = this.cachePath(text);
    if (fs.existsSync(out) && fs.statSync(out).size > 128) return out;

    await fsp.mkdir(this.cacheDir, { recursive: true });
    const tmp = `${out}.${process.pid}.tmp`;

    try {
      if (process.platform === 'win32') {
        await synthWindows(text, tmp, this.config.voice, this.config.lang);
      } else if (process.platform === 'darwin') {
        await synthMac(text, tmp, this.config.voice);
      } else {
        await synthLinux(text, tmp, this.config.voice, this.config.lang);
      }
      await fsp.rename(tmp, out);
      this.available = true;
      return out;
    } catch (err) {
      this.available = false;
      await fsp.rm(tmp, { force: true }).catch(() => {});
      throw new Error(`tts failed: ${err.message.split('\n')[0].slice(0, 120)}`);
    }
  }
}

export function defaultCacheDir(here) {
  return path.join(here, '.cache');
}

export { os };
