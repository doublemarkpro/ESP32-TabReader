// Run after installing the pinned tools/ui-assets dependencies. No device font conversion needed.
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const sources = ['main/reader/reader_display.cc', 'main/reader/transfer_server.cc',
  'main/boards/m5stack/tab5-reader/tab5_reader.cc',
  'main/han_dictionary/reader/epub_reader.cc'];
const text = sources.map(p => fs.readFileSync(path.join(root, p), 'utf8')).join('');
const symbols = [...new Set([...text].filter(ch => ch.codePointAt(0) >= 0x80))].sort().join('');
fs.mkdirSync(path.join(root, 'main/reader/assets'), { recursive: true });
const converter = path.join(root, 'tools/ui-assets/node_modules/lv_font_conv/lv_font_conv.js');
const result = spawnSync(process.execPath, [converter, '--font',
  path.join(root, 'assets/source/fonts/NotoSansSC-Medium.ttf'), '--size', '24', '--bpp', '4',
  '--range', '0x20-0x7e', '--symbols', symbols, '--format', 'lvgl', '--no-kerning',
  '--lv-font-name', 'reader_ui_24', '--output', path.join(root, 'main/reader/assets/reader_ui_24.c')],
  { stdio: 'inherit' });
process.exitCode = result.status ?? 1;
if (process.exitCode === 0) {
  const output = path.join(root, 'main/reader/assets/reader_ui_24.c');
  fs.writeFileSync(output, fs.readFileSync(output, 'utf8').trimEnd() + '\n');
}
