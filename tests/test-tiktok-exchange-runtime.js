// Run the generated exchange script in isolated document contexts.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const vm = require('node:vm');
const script = execFileSync(process.argv[2], ['--emit-starter'], { encoding: 'utf8' });
let calls = 0;
let resolveFetch;
const fetch = () => {
  ++calls;
  return new Promise(resolve => { resolveFetch = resolve; });
};
const document = (host, pathname) => vm.createContext({
  window: { location: { host, pathname } }, fetch,
});
(async () => {
  const stale = document('www.tiktok.com', '/login');
  assert.equal(vm.runInContext(script, stale), 'wrong-page');
  assert.equal(calls, 0);
  const committed = document('streamlabs.com', '/dashboard');
  assert.equal(vm.runInContext(script, committed), 'started');
  assert.equal(vm.runInContext(script, committed), 'in-progress');
  assert.equal(calls, 1);
  resolveFetch({ status: 200, text: async () => '{"success":true,"data":{"oauth_token":"FAKE"}}' });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(vm.runInContext(script, committed), 'kept');
  assert.equal(calls, 1);
  // A new success document has a fresh slot: the committed-document retry
  // remains available after an earlier navigation/context was stale.
  const nextDocument = document('streamlabs.com', '/dashboard');
  assert.equal(vm.runInContext(script, nextDocument), 'started');
  assert.equal(calls, 2);
  console.log('Exchange document, concurrency, and retry checks passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
