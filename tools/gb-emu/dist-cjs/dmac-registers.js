"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.DmacRegisters = void 0;
const DMAC_ADDR = 0x41004800;
const CTRL_OFFSET = 0x00;
const SWTRIGCTRL_OFFSET = 0x10;
const BASEADDR_OFFSET = 0x34;
const WRBADDR_OFFSET = 0x38;
const CHID_OFFSET = 0x3F;
const CHCTRLA_OFFSET = 0x40;
const CHCTRLB_OFFSET = 0x44;
const CHINTENSET_OFFSET = 0x4D;
const CHINTFLAG_OFFSET = 0x4E;
class DmacRegisters {
    constructor(processor) {
        this._descriptor = 0;
        processor.registerPeripheralWriteHandler(DMAC_ADDR + BASEADDR_OFFSET, (address, value) => {
            this._baseAddr = value;
            this.debugWrite("BASEADDR", address, value);
        });
        processor.registerPeripheralWriteHandler(DMAC_ADDR + WRBADDR_OFFSET, (address, value) => {
            this._wrbAddr = value;
            this.debugWrite("WRBADDR", address, value);
        });
        processor.registerPeripheralWriteHandler(DMAC_ADDR + CHID_OFFSET, (address, value) => {
            this._selectedChannelId = value;
            this.debugWrite("CHID", address, value);
        });
        processor.registerPeripheralWriteHandler(DMAC_ADDR + CHCTRLA_OFFSET, (address, value) => {
            this.debugWrite("CHCTRLA", address, value);
            if (value == 0b10) {
                // TODO use wrb appropriately
                if (this._descriptor == 0) {
                    this._descriptor = this._baseAddr + this._selectedChannelId * 0x10;
                }
                // TODO use btctrl
                var btctrl = processor.fetchHalfword(this._descriptor);
                var btcnt = processor.fetchHalfword(this._descriptor + 0x02);
                var srcaddr = processor.fetchWord(this._descriptor + 0x04);
                var dstaddr = processor.fetchWord(this._descriptor + 0x08);
                var descaddr = processor.fetchWord(this._descriptor + 0x0C);
                // console.log(`btctrl: 0x${btctrl.toString(16)}; btcnt: #${btcnt}; srcaddr: 0x${srcaddr.toString(16)}; dstaddr: 0x${dstaddr.toString(16)}; descaddr: 0x${descaddr.toString(16)};`)
                for (var i = 0; i < btcnt; i++) {
                    processor.writeByte(dstaddr, processor.fetchByte(srcaddr + i - btcnt));
                }
                this._descriptor = descaddr;
                processor.dmacInterrupt();
            }
        });
        processor.registerPeripheralReadHandler(DMAC_ADDR + CHINTFLAG_OFFSET, (address) => {
            // TODO Actual implementation of SUSP, TCMPL, and TERR
            return 0b010; // TCMPL 
        });
    }
    debugWrite(name, address, value) {
        // console.log(`${name} <= 0x${value.toString(16)}`);
    }
}
exports.DmacRegisters = DmacRegisters;
