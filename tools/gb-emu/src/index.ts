import { Atsamd21 } from './atsamd21';
import { St7735 } from './st7735';
import { Buttons } from './buttons';
import { SdCard } from './sdcard';
import { buildFatImageFromTree, FatNode } from './sdfat';

// Max steps to execute per `run()`
const maxLoopCount = 500000;
const programOffset = 0x4000;

export class Emulator {
    autoStart:boolean = true;
    loading:boolean = false;

    private _atsamd21:Atsamd21;
    private _screen:St7735;
    private _buttons:Buttons;
    private _ctx:CanvasRenderingContext2D;
    private _running:boolean;
    private _sdImage:Uint8Array;
    private _loopCount = maxLoopCount;
    private _customKeymap:number[][];
    private _lastTickCount:number;
    private _sd:SdCard;

    constructor(locationId:string) {
        // Create the canvas where the screen will be displayed
        var canvas = <HTMLCanvasElement>document.createElement("canvas");
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
            const files = dirInput.files;
            if (!files || !files.length) return;
            const tree: FatNode[] = [];
            const index = new Map<string, FatNode>();
            const bins: { name: string; folder: string; file: File }[] = [];
            let rootFolder = "";
            for (let i = 0; i < files.length; i++) {
                const f = files[i];
                const rel = (f as any).webkitRelativePath as string || f.name;
                const parts = rel.split("/");
                rootFolder = parts[0];
                if (parts.length === 1) continue; // l'entrée du dossier lui-même
                let level = tree;
                for (let d = 0; d < parts.length - 1; d++) {
                    const name = parts[d];
                    let node = index.get("d/" + parts.slice(0, d + 1).join("/"));
                    if (!node) {
                        node = { name, children: [] };
                        index.set("d/" + parts.slice(0, d + 1).join("/"), node);
                        (d === 0 ? level : index.get("d/" + parts.slice(0, d).join("/"))!.children!).push(node);
                    }
                    level = node.children!;
                }
                const node: FatNode = { name: parts[parts.length - 1] };
                index.set("f/" + rel, node);
                level.push(node);
                if (/\.bin$/i.test(f.name)) {
                    bins.push({ name: f.name, folder: parts[parts.length - 2] ?? "", file: f });
                }
                const reader = new FileReader();
                reader.onload = () => {
                    node.data = new Uint8Array(reader.result as ArrayBuffer);
                };
                reader.readAsArrayBuffer(f);
            }
            // attendre la lecture de tous les fichiers, puis construire la carte
            const allRead = new Promise((resolve) => {
                const check = setInterval(() => {
                    const total = files.length;
                    let done = 0;
                    index.forEach((n) => { if (n.data) done++; });
                    if (done + bins.length >= total) {
                        clearInterval(check);
                        resolve(null);
                    }
                }, 50);
            });
            allRead.then(() => {
                this.loadSdImageFromBuffer(buildFatImageFromTree(tree).image.buffer as ArrayBuffer);
                const bin = bins.length === 1 ? bins[0]
                    : bins.find((b) => b.name.replace(/\.bin$/i, "") === bins[0]?.folder);
                if (bin) {
                    const r = new FileReader();
                    r.onload = () => this.loadFromBuffer(r.result as ArrayBuffer);
                    r.readAsArrayBuffer(bin.file);
                }
            });
        };
        document.getElementById(locationId).appendChild(dirInput);
        this._ctx = canvas.getContext("2d");
    }

    loadFromUrl(url:string): void {
        this.loading = true;

        var oReq = new XMLHttpRequest();
        
        oReq.onload = (e) => {
            this.loadFromBuffer(oReq.response);
        }

        oReq.open("GET", url);
        oReq.responseType = "arraybuffer";
        oReq.send();
    }

    // Carte SD virtuelle : image brute (dump ou mkfs.fat) à lire par les jeux.
    loadSdImageFromBuffer(buffer: ArrayBuffer): void {
        this._sdImage = new Uint8Array(buffer);
        if (this._sd) this._sd.loadImage(this._sdImage);
    }

    loadFromBuffer(buffer: ArrayBuffer) {
        this._atsamd21 = new Atsamd21();
        this._atsamd21.loadFlash(new Uint8Array(buffer), programOffset);
        this.loading = false;
        
        this._screen = new St7735(this._atsamd21.sercom4, this._atsamd21.portA, this._atsamd21.portB, this._ctx);
        // Clear the canvas
        this._screen.clear();
    
        this._buttons = new Buttons(this._atsamd21.sercom4, this._atsamd21.portA, this._atsamd21.portB);
        this._sd = new SdCard(this._atsamd21.portA, this._atsamd21.sercom4);
        if (this._sdImage) this._sd.loadImage(this._sdImage);
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

    private run() {
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

    private performanceMonitor() {
        // Try to slow down or speed up based on how many cycles we are able to complete per second.
        const goalTicks = 20000000; // Based off of hack in ATSAMD21. Realistic would be 48M. 
        var factor = goalTicks / (this._atsamd21.tickCount - this._lastTickCount);

        this._loopCount = Math.round(this._loopCount * factor);
        if (this._loopCount > maxLoopCount || this._loopCount < 0) this._loopCount = maxLoopCount;

        if (this._running) {
            this._lastTickCount = this._atsamd21.tickCount;
            setTimeout(() => { this.performanceMonitor(); }, 1000);
        }
    }

    setKeymap(keymap:number[][]) {
        this._customKeymap = keymap;

        if (this._buttons) {
            this._buttons.setKeymap(this._customKeymap);
        }
    }

    setButtonData(buttonData: number) {
        if (this._buttons) {
            this._buttons.customButtonData(buttonData);
        }
    }
}
