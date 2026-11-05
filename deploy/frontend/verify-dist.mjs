import { readFile, stat } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const frontendRoot = dirname(fileURLToPath(import.meta.url));
const html = await readFile(resolve(frontendRoot, 'dist/index.html'), 'utf8');

if (html.includes('type="module"') || html.includes(' crossorigin')) {
  throw new Error('Production HTML still contains file://-incompatible ESM');
}

const scriptSource = html.match(/<script defer src="(.+?)"><\/script>/)?.[1];
if (!scriptSource?.startsWith('./')) {
  throw new Error('Production HTML must reference one relative classic script');
}

await stat(resolve(frontendRoot, 'dist', scriptSource));

// Onboarding's painting draws its parts from the script beside the page, never
// from their files, which taint its canvas on disk (vite.config.js,
// painting-parts-script).
const paintingParts = await readFile(resolve(frontendRoot, 'dist/painting/parts.js'), 'utf8');
if (!paintingParts.startsWith('window.__singularityPaintingParts = {"')) {
  throw new Error("The painting's parts script beside the page is missing its parts");
}
if (await stat(resolve(frontendRoot, 'dist/painting/parts')).then(() => true, () => false)) {
  throw new Error("The painting's parts must not also stand beside the page as files");
}

// Singularity's pane in Word, Excel and Outlook (code/frontend/office): Office's
// script and the connection's transport first, then one deferred bundle.
const pane = await readFile(resolve(frontendRoot, 'dist/office/index.html'), 'utf8');
if (!pane.includes('https://appsforoffice.microsoft.com/lib/1/hosted/office.js') || !pane.includes('./native-bridge.js')) {
  throw new Error('The Office pane must load office.js and the connection before its bundle');
}
const paneScript = pane.match(/<script defer src="(.+?)"><\/script>/)?.[1];
if (!paneScript?.startsWith('./')) {
  throw new Error('The Office pane must reference one relative classic bundle');
}
await stat(resolve(frontendRoot, 'dist/office', paneScript));
for (const asset of ['commands.html', 'assets/singularity-16.png', 'assets/singularity-32.png', 'assets/singularity-64.png', 'assets/singularity-80.png']) {
  await stat(resolve(frontendRoot, 'dist/office', asset));
}
console.log('Desktop production bundle verified.');
