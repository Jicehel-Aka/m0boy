/* FAT16 en mémoire, construit depuis un arbre de nœuds {name, data|children}.
 * Utilisable dans le navigateur comme sous Node (aucune dépendance fs).
 * Noms courts 8.3, fichiers et dossiers en chaînes contiguës, image 64 Mio.
 * Renvoie la carte des plages LBA -> nœud, pour réécrire les secteurs
 * modifiés (sauvegardes) vers les fichiers d'origine. */

export interface FatNode {
    name: string;
    data?: Uint8Array;        // fichier
    children?: FatNode[];     // dossier
    path?: string;            // info optionnelle, renvoyée dans fileMap
}

const SECTOR = 512;
const SPC = 8;                    // 4 Kio par cluster
const TOTAL_SECTORS = 131072;     // 64 Mio -> FAT16
const ROOT_ENTRIES = 512;

interface Installed {
    name83: string;
    attr: number;
    first: number;      // premier cluster
    clusters: number;
    size: number;
    node: FatNode;
}

function to83(name: string, used: Set<string>): string {
    const up = name.toUpperCase();
    const dot = up.lastIndexOf('.');
    let base = (dot > 0 ? up.slice(0, dot) : up).replace(/[^A-Z0-9_$%'-@{}~`!#()&-]/g, '').slice(0, 8);
    const ext = (dot > 0 ? up.slice(dot + 1) : '').replace(/[^A-Z0-9_$%'-@{}~`!#()&-]/g, '').slice(0, 3);
    let combined = base + (ext ? '.' + ext : '');
    if (combined !== up || used.has(combined)) { // tronqué/transformé ou collision
        for (let i = 1; i < 100; i++) {
            combined = base.slice(0, 6) + '~' + i + (ext ? '.' + ext : '');
            if (!used.has(combined)) break;
        }
    }
    used.add(combined);
    return (combined.split('.')[0] || '').padEnd(8, ' ')
         + (combined.split('.')[1] || '').padEnd(3, ' ');
}

export function buildFatImageFromTree(nodes: FatNode[]): { image: Uint8Array; fileMap: { lba: number; bytes: number; node: FatNode }[] } {
    const image = new Uint8Array(TOTAL_SECTORS * SECTOR);
    const fileMap: { lba: number; bytes: number; node: FatNode }[] = [];

    // --- boot sector (meta_fat : signature, 512 o/secteur, spc puissance de 2, 1-2 FAT)
    image[0] = 0xeb; image[1] = 0x3c; image[2] = 0x90;
    image.set(new TextEncoder().encode('GBREMU'), 3);
    image[0x0b] = 0x00; image[0x0c] = 0x02;
    image[0x0d] = SPC;
    image[0x0e] = 0x01;
    image[0x10] = 0x02;
    image[0x11] = 0x00; image[0x12] = 0x02;
    image[0x15] = 0xf8;
    let fatsz = 64, clusters: number;
    for (;;) {
        clusters = Math.floor((TOTAL_SECTORS - 1 - 2 * fatsz - ROOT_ENTRIES * 32 / SECTOR) / SPC);
        const need = Math.ceil((clusters + 2) * 2 / SECTOR);
        if (need === fatsz) break;
        fatsz = need;
    }
    image[0x16] = fatsz & 0xff; image[0x17] = (fatsz >> 8) & 0xff;
    image[0x20] = TOTAL_SECTORS & 0xff; image[0x21] = (TOTAL_SECTORS >> 8) & 0xff;
    image[0x22] = (TOTAL_SECTORS >> 16) & 0xff; image[0x23] = (TOTAL_SECTORS >> 24) & 0xff;
    image[510] = 0x55; image[511] = 0xaa;

    // --- placement récursif : alloue, écrit les contenus, renvoie les entrées
    const fat = new Uint8Array(fatsz * SECTOR);
    fat[0] = 0xf8; fat[1] = 0xff;
    fat[2] = 0xff; fat[3] = 0xff;
    let nextCluster = 2;
    const allocRun = (n: number) => {
        const first = nextCluster;
        for (let i = 0; i < n; i++) {
            const c = first + i;
            const v = i === n - 1 ? 0xffff : c + 1;
            fat[c * 2] = v & 0xff; fat[c * 2 + 1] = (v >> 8) & 0xff;
        }
        nextCluster += n;
        return first;
    };
    const dataLba = (first: number) => 1 + 2 * fatsz + ROOT_ENTRIES * 32 / SECTOR + (first - 2) * SPC;
    const writeRun = (first: number, data: Uint8Array) => {
        image.set(data.subarray(0, Math.min(data.length, SPC * SECTOR * 4096)), dataLba(first) * SECTOR);
    };
    const dirBuffer = (installed: Installed[], selfCluster: number, parentCluster: number): Uint8Array => {
        const buf = new Uint8Array(SPC * SECTOR);
        const entry = (off: number, name83: string, attr: number, cluster: number, size: number) => {
            for (let i = 0; i < 11; i++) buf[off + i] = name83.charCodeAt(i);
            buf[off + 11] = attr;
            buf[off + 26] = cluster & 0xff; buf[off + 27] = (cluster >> 8) & 0xff;
            buf[off + 28] = size & 0xff; buf[off + 29] = (size >> 8) & 0xff;
            buf[off + 30] = (size >> 16) & 0xff; buf[off + 31] = (size >> 24) & 0xff;
        };
        entry(0, '.          ', 0x10, selfCluster, 0);
        entry(32, '..         ', 0x10, parentCluster, 0);
        installed.forEach((n, i) => entry(64 + 32 * i, n.name83, n.attr, n.first, n.size));
        return buf;
    };
    const placeTree = (nodes: FatNode[], parentFirst: number, isRoot: boolean): Installed[] => {
        const used = new Set<string>();
        const installed: Installed[] = [];
        for (const n of nodes) {
            const name83 = to83(n.name, used);
            if (n.children) {
                const self = allocRun(1);
                const children = placeTree(n.children, self, false);
                writeRun(self, dirBuffer(children, self, isRoot ? self : parentFirst));
                installed.push({ name83, attr: 0x10, first: self, clusters: 1, size: 0, node: n });
            } else if (n.data && n.data.length) {
                const clusters = Math.ceil(n.data.length / (SPC * SECTOR));
                const first = allocRun(clusters);
                writeRun(first, n.data);
                fileMap.push({ lba: dataLba(first), bytes: n.data.length, node: n });
                installed.push({ name83, attr: 0x20, first, clusters, size: n.data.length, node: n });
            }
        }
        return installed;
    };
    const rootInstalled = placeTree(nodes, 0, true);

    // racine : zone fixe FAT16
    const rootBuf = new Uint8Array(ROOT_ENTRIES * 32);
    rootInstalled.forEach((n, i) => {
        const off = 32 * i;
        for (let k = 0; k < 11; k++) rootBuf[off + k] = n.name83.charCodeAt(k);
        rootBuf[off + 11] = n.attr;
        rootBuf[off + 26] = n.first & 0xff; rootBuf[off + 27] = (n.first >> 8) & 0xff;
        rootBuf[off + 28] = n.size & 0xff; rootBuf[off + 29] = (n.size >> 8) & 0xff;
        rootBuf[off + 30] = (n.size >> 16) & 0xff; rootBuf[off + 31] = (n.size >> 24) & 0xff;
    });
    image.set(rootBuf, (1 + 2 * fatsz) * SECTOR);
    // FATs
    image.set(fat, SECTOR);
    image.set(fat, (1 + fatsz) * SECTOR);
    return { image, fileMap };
}
