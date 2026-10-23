// Exact code-native mask asset: one opaque luminance tile, never AI artwork.
const fs = require('node:fs/promises'), path = require('node:path');
let sharp;
try { sharp = require('sharp'); }
catch { sharp = require(path.join(process.env.USERPROFILE, '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp')); }
const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="126"><rect width="64" height="126" fill="black"/><path d="M8 0 L56 63 L8 126 H64 V0Z" fill="white"/></svg>';
async function generate(file) {
  await sharp(Buffer.from(svg)).removeAlpha().png().toFile(file);
}
module.exports = generate;
if (require.main === module) (async () => {
  const root = path.resolve(__dirname, '..'), preview = path.join(root, 'docs/design/storybook-generated');
  for (const file of [path.join(preview, 'assets/zigzag-mask.png'), path.join(root, 'res/menu/storybook/zigzag-mask.png')]) await generate(file);
  const manifestPath = path.join(preview, 'manifest.json');
  const manifest = JSON.parse(await fs.readFile(manifestPath, 'utf8'));
  manifest.assets['zigzag-mask'] = {file:'assets/zigzag-mask.png', size:[64,126], content:[0,0,64,126], origin:'opaque black/white seamless luminance tile; one shared mask'};
  await fs.writeFile(manifestPath, JSON.stringify(manifest,null,2)+'\n');
  console.log('Generated one opaque 64x126 black/white zigzag tile for preview and native resources.');
})().catch(error => {console.error(error);process.exitCode=1;});
