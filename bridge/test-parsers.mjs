// Offline checks for the header/model parsing. No credentials, no network.
//   node test-parsers.mjs
import assert from 'node:assert/strict';
import { readWindowHeaders, readOpusWindow, parseReset, prettyModel, priceFor } from './bridge.mjs';

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log(`  ok   ${name}`);
  } catch (err) {
    failures++;
    console.log(`  FAIL ${name}\n       ${err.message}`);
  }
}

const inFiveHours = Math.floor((Date.now() + 5 * 3600_000) / 1000);
const headers = new Headers({
  'anthropic-ratelimit-unified-5h-utilization': '0.42',
  'anthropic-ratelimit-unified-5h-reset': String(inFiveHours),
  'anthropic-ratelimit-unified-7d-utilization': '0.615',
  'anthropic-ratelimit-unified-7d-reset': new Date(Date.now() + 3 * 86400_000).toISOString(),
  'anthropic-ratelimit-unified-7d-opus-utilization': '0.12',
  'anthropic-ratelimit-unified-7d-opus-reset': String(inFiveHours),
  'anthropic-ratelimit-unified-status': 'allowed',
});

console.log('rate-limit headers');
check('5h utilisation is scaled to a percentage', () => {
  assert.equal(readWindowHeaders(headers, '5h').pct, 42);
});
check('5h reset becomes seconds remaining', () => {
  const s = readWindowHeaders(headers, '5h').resetInSec;
  assert.ok(s > 5 * 3600 - 60 && s <= 5 * 3600, `got ${s}`);
});
check('7d ignores the opus-scoped variant', () => {
  assert.equal(readWindowHeaders(headers, '7d').pct, 61.5);
});
check('opus window is read separately', () => {
  assert.equal(readOpusWindow(headers).pct, 12);
});
check('percentages already in 0..100 pass through', () => {
  assert.equal(readWindowHeaders(new Headers({ 'anthropic-ratelimit-unified-5h-utilization': '73' }), '5h').pct, 73);
});
check('missing headers yield null', () => {
  assert.equal(readWindowHeaders(new Headers({}), '5h'), null);
});
check('ISO resets parse', () => {
  const s = parseReset(new Date(Date.now() + 7200_000).toISOString());
  assert.ok(s > 7100 && s <= 7200, `got ${s}`);
});
check('missing reset is 0', () => assert.equal(parseReset(undefined), 0));

console.log('model names');
const names = {
  'claude-opus-5-20260101': 'Opus 5',
  'claude-haiku-4-5-20251001': 'Haiku 4.5',
  'claude-sonnet-4-20250514': 'Sonnet 4',
  'claude-3-5-haiku-20241022': 'Haiku',
};
for (const [id, want] of Object.entries(names)) {
  check(`${id} -> ${want}`, () => assert.equal(prettyModel(id), want));
}

console.log('pricing');
check('opus id picks opus pricing', () => assert.equal(priceFor('claude-opus-5-20260101').output, 75));
check('haiku 4.5 picks haiku pricing', () => assert.equal(priceFor('claude-haiku-4-5-20251001').input, 1));
check('3-5-haiku wins over haiku', () => assert.equal(priceFor('claude-3-5-haiku-20241022').input, 0.8));
check('3-5-sonnet picks sonnet pricing', () => assert.equal(priceFor('claude-3-5-sonnet-20241022').input, 3));
check('unknown model has no price', () => assert.equal(priceFor('<synthetic>'), null));

console.log(failures ? `\n${failures} failing` : '\nall passing');
process.exit(failures ? 1 : 0);
