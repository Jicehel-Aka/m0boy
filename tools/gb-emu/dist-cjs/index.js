"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.Emulator = void 0;
const atsamd21_1 = require("./atsamd21");
const st7735_1 = require("./st7735");
const buttons_1 = require("./buttons");
const sdcard_1 = require("./sdcard");
const sdfat_1 = require("./sdfat");
// Max steps to execute per `run()`
const maxLoopCount = 500000;
const programOffset = 0x4000;
class Emulator {
    constructor(locationId) {
        this.autoStart = true;
        this.loading = false;
        this._loopCount = maxLoopCount;
        // Create the canvas where the screen will be displayed
        var canvas = document.createElement("canvas");
        canvas.width = 320;
        canvas.height = 256;
        document.getElementById(locationId).appendChild(canvas);
        // Sélecteur de dossier : le dossier devient la carte SD, et son .bin
        // (s'il est unique) le firmware — un seul geste pour lancer un jeu.
        const dirInput = document.createElement("input");
        dirInput.type = "file";
        dirInput.setAttribute("webkitdirectory", "");
        dirInput.setAttribute("directory", "");
        dirInput.onchange = () => {
            var _a;
            const files = dirInput.files;
            if (!files || !files.length)
                return;
            const tree = [];
            const index = new Map();
            const bins = [];
            let rootFolder = "";
            for (let i = 0; i < files.length; i++) {
                const f = files[i];
                const rel = f.webkitRelativePath || f.name;
                const parts = rel.split("/");
                rootFolder = parts[0];
                if (parts.length === 1)
                    continue; // l'entrée du dossier lui-même
                let level = tree;
                for (let d = 0; d < parts.length - 1; d++) {
                    const name = parts[d];
                    let node = index.get("d/" + parts.slice(0, d + 1).join("/"));
                    if (!node) {
                        node = { name, children: [] };
                        index.set("d/" + parts.slice(0, d + 1).join("/"), node);
                        (d === 0 ? level : index.get("d/" + parts.slice(0, d).join("/")).children).push(node);
                    }
                    level = node.children;
                }
                const node = { name: parts[parts.length - 1] };
                index.set("f/" + rel, node);
                level.push(node);
                if (/\.bin$/i.test(f.name)) {
                    bins.push({ name: f.name, folder: (_a = parts[parts.length - 2]) !== null && _a !== void 0 ? _a : "", file: f });
                }
                const reader = new FileReader();
                reader.onload = () => {
                    node.data = new Uint8Array(reader.result);
                };
                reader.readAsArrayBuffer(f);
            }
            // attendre la lecture de tous les fichiers, puis construire la carte
            const allRead = new Promise((resolve) => {
                const check = setInterval(() => {
                    const total = files.length;
                    let done = 0;
                    index.forEach((n) => { if (n.data)
                        done++; });
                    if (done + bins.length >= total) {
                        clearInterval(check);
                        resolve(null);
                    }
                }, 50);
            });
            allRead.then(() => {
                this.loadSdImageFromBuffer(sdfat_1.buildFatImageFromTree(tree).image.buffer);
                const bin = bins.length === 1 ? bins[0]
                    : bins.find((b) => { var _a; return b.name.replace(/\.bin$/i, "") === ((_a = bins[0]) === null || _a === void 0 ? void 0 : _a.folder); });
                if (bin) {
                    const r = new FileReader();
                    r.onload = () => this.loadFromBuffer(r.result);
                    r.readAsArrayBuffer(bin.file);
                }
            });
        };
        document.getElementById(locationId).appendChild(dirInput);
        this._ctx = canvas.getContext("2d");
    }
    loadFromUrl(url) {
        this.loading = true;
        var oReq = new XMLHttpRequest();
        oReq.onload = (e) => {
            this.loadFromBuffer(oReq.response);
        };
        oReq.open("GET", url);
        oReq.responseType = "arraybuffer";
        oReq.send();
    }
    // Carte SD virtuelle : image brute (dump ou mkfs.fat) à lire par les jeux.
    loadSdImageFromBuffer(buffer) {
        this._sdImage = new Uint8Array(buffer);
        if (this._sd)
            this._sd.loadImage(this._sdImage);
    }
    loadFromBuffer(buffer) {
        this._atsamd21 = new atsamd21_1.Atsamd21();
        this._atsamd21.loadFlash(new Uint8Array(buffer), programOffset);
        this.loading = false;
        this._screen = new st7735_1.St7735(this._atsamd21.sercom4, this._atsamd21.portA, this._atsamd21.portB, this._ctx);
        // Clear the canvas
        this._screen.clear();
        this._buttons = new buttons_1.Buttons(this._atsamd21.sercom4, this._atsamd21.portA, this._atsamd21.portB);
        this._sd = new sdcard_1.SdCard(this._atsamd21.portA, this._atsamd21.sercom4);
        if (this._sdImage)
            this._sd.loadImage(this._sdImage);
        if (this._customKeymap) {
            this._buttons.setKeymap(this._customKeymap);
        }
        if (this.autoStart) {
            this.start();
        }
    }
    reset() {
        this._screen.clear();
        this._atsamd21.reset(programOffset);
        this._lastTickCount = this._atsamd21.tickCount;
    }
    start() {
        if (!this._running) {
            this._running = true;
            this.run();
            this._lastTickCount = this._atsamd21.tickCount;
            setTimeout(() => { this.performanceMonitor(); }, 1000);
        }
    }
    stop() {
        this._running = false;
    }
    run() {
        try {
            var runFunc = () => { this.run(); };
            // Run some number of steps in the microcontroller
            for (var i = 0; i < this._loopCount; i++) {
                this._atsamd21.step();
            }
            this._screen.updateCanvas();
            if (this._running) {
                setTimeout(runFunc, 0);
            }
        }
        catch (error) {
            this._running = false;
            throw error;
        }
    }
    performanceMonitor() {
        // Try to slow down or speed up based on how many cycles we are able to complete per second.
        const goalTicks = 20000000; // Based off of hack in ATSAMD21. Realistic would be 48M. 
        var factor = goalTicks / (this._atsamd21.tickCount - this._lastTickCount);
        this._loopCount = Math.round(this._loopCount * factor);
        if (this._loopCount > maxLoopCount || this._loopCount < 0)
            this._loopCount = maxLoopCount;
        if (this._running) {
            this._lastTickCount = this._atsamd21.tickCount;
            setTimeout(() => { this.performanceMonitor(); }, 1000);
        }
    }
    setKeymap(keymap) {
        this._customKeymap = keymap;
        if (this._buttons) {
            this._buttons.setKeymap(this._customKeymap);
        }
    }
    setButtonData(buttonData) {
        if (this._buttons) {
            this._buttons.customButtonData(buttonData);
        }
    }
}
exports.Emulator = Emulator;
