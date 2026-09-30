/* Carte SD virtuelle depuis un répertoire local : construit l'arbre, le
 * convertit en image FAT16 (voir src/sdfat.ts, code partagé avec le web)
 * et renvoie la carte des fichiers pour réécrire les .SAV sur disque. */
const fs = require('fs');
const path = require('path');
const { buildFatImageFromTree } = require('./dist-cjs/sdfat.js');

function buildFatImage(dirPath) {
    const walk = (dir) => fs.readdirSync(dir, { withFileTypes: true })
        .filter((e) => !e.name.startsWith('.'))
        .map((e) => {
            const full = path.join(dir, e.name);
            if (e.isDirectory()) return { name: e.name, children: walk(full) };
            return { name: e.name, data: fs.readFileSync(full), path: full };
        });
    const { image, fileMap } = buildFatImageFromTree(walk(dirPath));
    return {
        image,
        fileMap: fileMap.map((m) => ({ lba: m.lba, bytes: m.bytes, path: m.node.path })),
    };
}

module.exports = { buildFatImage };
