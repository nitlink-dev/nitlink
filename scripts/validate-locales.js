// Validate localization resources without a browser or third-party dependencies.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const context = vm.createContext({window: {}});
const locales = ['en-US', 'zh-TW', 'zh-CN', 'de-DE', 'ko-KR'];
for (const locale of locales) {
  vm.runInContext(fs.readFileSync(path.join(root, 'locales', locale + '.js'), 'utf8'), context);
}
const tables = context.window.NitLinkLocales;
const keys = Object.keys(tables['en-US']).sort();
const placeholders = value => [...value.matchAll(/\{\w+\}/g)].map(m => m[0]).sort();
for (const locale of locales) {
  assert.deepEqual(Object.keys(tables[locale]).sort(), keys, locale + ': missing/extra keys');
  for (const key of keys) {
    assert.equal(typeof tables[locale][key], 'string', locale + ': ' + key);
    assert.ok(tables[locale][key].trim(), locale + ': empty ' + key);
    assert.deepEqual(placeholders(tables[locale][key]), placeholders(tables['en-US'][key]), locale + ': ' + key);
  }
}
const html = fs.readFileSync(path.join(root, 'nitlink-menu.html'), 'utf8');
for (const [, key] of html.matchAll(/(?:data-i18n(?:-title|-aria-label)?|data-audio-help)="([^"]+)"/g)) {
  assert.ok(keys.includes(key), 'HTML references missing translation: ' + key);
}
const cpp = fs.readFileSync(path.join(root, 'src/app/localization.cpp'), 'utf8');
const nativeTables = [...cpp.matchAll(/const Table& (\w+)Table\(\)\s*\{([\s\S]*?)return table;/g)].map(([, name, body]) => {
  const values = new Map([...body.matchAll(/\{L"([^"]+)", L"((?:\\.|[^"\\])*)"\}/g)].map(m => [m[1], m[2]]));
  return {name, values};
});
assert.equal(nativeTables.length, 5);
for (const {name, values} of nativeTables) {
  assert.deepEqual([...values.keys()].sort(), [...nativeTables[0].values.keys()].sort(), name + ': native key coverage');
  for (const [key, value] of values) assert.deepEqual(placeholders(value), placeholders(nativeTables[0].values.get(key)), name + ': ' + key);
}
console.log(`Validated ${keys.length} menu strings and ${nativeTables[0].values.size} native strings in all five languages.`);
