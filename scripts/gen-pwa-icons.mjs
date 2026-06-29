import sharp from 'sharp';
import { fileURLToPath } from 'url';
import path from 'path';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const src = path.join(root, 'logo.jpg');

const meta = await sharp(src).metadata();
const side = Math.min(meta.width, meta.height);
const left = Math.floor((meta.width - side) / 2);
const top = Math.floor((meta.height - side) / 2);

const square = sharp(src).extract({ left, top, width: side, height: side });

for (const size of [192, 512]) {
  const out = path.join(root, `icon-${size}.png`);
  await square.clone().resize(size, size).png({ compressionLevel: 9 }).toFile(out);
  console.log(`wrote ${out} (${size}x${size})`);
}
