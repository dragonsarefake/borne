// Pull the web page's JavaScript out of the sketch so page_harness.js can run
// it under node. The harness used to require a page_check.js that nothing in
// the repo produced, so the harness could not run at all - which is how the
// state renumbering (13/14/15 -> 6/7/8) reached flashed firmware unnoticed.
//
// This reads the .ino directly rather than passing the source through a shell
// or python layer: every time C or JS has gone through that pipeline in this
// project the escaping has broken the build.
const fs = require('fs');

const SRC = 'loadcell_testbed.ino';
const OUT = 'page_check.js';

const ino = fs.readFileSync(SRC, 'utf8');

const open = ino.indexOf('</div><script>');
if (open < 0) {
  console.error('FAIL: no "</div><script>" in ' + SRC);
  process.exit(1);
}
const start = open + '</div><script>'.length;
const end = ino.indexOf('</script>', start);
if (end < 0) {
  console.error('FAIL: no closing </script> after the page script');
  process.exit(1);
}

let js = ino.slice(start, end);

// The page lives in one C++ raw string literal, PAGE[] = R"HTML(...)HTML", so
// what sits between the markers is already verbatim - nothing to unescape.
// Guard that assumption: either delimiter appearing inside the extract means
// the literal was closed and reopened, and the extract is not the whole page.
if (js.indexOf(')HTML"') >= 0 || js.indexOf('R"HTML(') >= 0) {
  console.error('FAIL: a raw-string delimiter appears inside the extracted page.');
  console.error('      PAGE[] is no longer one literal - the extract is incomplete.');
  process.exit(1);
}

fs.writeFileSync(OUT, js);

// The ids the markup actually defines. The harness uses this to make
// getElementById return null for anything else, exactly as a browser does -
// otherwise a stub that invents elements on demand hides real crashes. That is
// how modePanel came to loop over m0..m3 when only m0 and m2 existed.
const markup = ino.slice(ino.indexOf('R"HTML('), open);
const ids = [...markup.matchAll(/\bid=["']?([A-Za-z][A-Za-z0-9_-]*)/g)].map(m => m[1]);
fs.writeFileSync('page_ids.json', JSON.stringify([...new Set(ids)].sort()));

const lines = js.split('\n').length;
console.log('extracted ' + js.length + ' bytes / ' + lines + ' lines -> ' + OUT);
console.log(new Set(ids).size + ' element ids -> page_ids.json');

// Fail loudly on a syntax error here rather than in a phone browser.
try {
  new Function(js);
  console.log('syntax OK');
} catch (e) {
  console.error('FAIL: page script does not parse: ' + e.message);
  process.exit(1);
}
