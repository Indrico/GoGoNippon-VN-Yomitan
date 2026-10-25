// Small, uncompressed ZIP writer for local diagnostic exports (no external tool).
const fs = require('node:fs');
const table = Array.from({length: 256}, (_, n) => {
  for (let i = 0; i < 8; i++) n = (n & 1) ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});
function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) crc = table[(crc ^ byte) & 255] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}
exports.writeZip = (output, entries) => {
  const parts = [], directory = [];
  let offset = 0;
  for (const [name, data] of entries) {
    const filename = Buffer.from(name, 'utf8'), bytes = Buffer.isBuffer(data) ? data : Buffer.from(data);
    if (bytes.length > 64 * 1024 * 1024 || offset + bytes.length > 128 * 1024 * 1024) throw new Error('Diagnostik terlalu besar; mulai sesi diagnostik baru.');
    const header = Buffer.alloc(30), central = Buffer.alloc(46), crc = crc32(bytes);
    header.writeUInt32LE(0x04034b50); header.writeUInt16LE(20, 4); header.writeUInt16LE(0x800, 6);
    header.writeUInt32LE(crc, 14); header.writeUInt32LE(bytes.length, 18); header.writeUInt32LE(bytes.length, 22); header.writeUInt16LE(filename.length, 26);
    central.writeUInt32LE(0x02014b50); central.writeUInt16LE(20, 4); central.writeUInt16LE(20, 6); central.writeUInt16LE(0x800, 8);
    central.writeUInt32LE(crc, 16); central.writeUInt32LE(bytes.length, 20); central.writeUInt32LE(bytes.length, 24);
    central.writeUInt16LE(filename.length, 28); central.writeUInt32LE(offset, 42);
    parts.push(header, filename, bytes); directory.push(central, filename); offset += header.length + filename.length + bytes.length;
  }
  const centralBytes = Buffer.concat(directory), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(centralBytes.length, 12); end.writeUInt32LE(offset, 16);
  fs.writeFileSync(output, Buffer.concat([...parts, centralBytes, end]));
};
