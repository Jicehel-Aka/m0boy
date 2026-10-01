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
const SERCOM4_DMAC_ID_TX = 0x0A;
class DmacRegisters {
    constructor(processor) {
        // Next descriptor of each channel (0: the channel's entry in BASEADDR).
        this._descriptor = [];
        this._trigsrc = [];
        this._selectedChannelId = 0;
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
        processor.registerPeripheralWriteHandler(DMAC_ADDR + CHCTRLB_OFFSET, (address, value) => {
            this._trigsrc[this._selectedChannelId] = (value >> 8) & 0x3F;
            this.debugWrite("CHCTRLB", address, value);
        });
        processor.registerPeripheralWriteHandler(DMAC_ADDR + CHCTRLA_OFFSET, (address, value) => {
            var _a;
            this.debugWrite("CHCTRLA", address, value);
            const ch = this._selectedChannelId;
            // Only SERCOM4 TX (the display) is modelled, as an instant copy.
            // Other triggers (the audio channel, TC4 to the DAC) are not
            // emulated: TC4 and the DAC are not either.
            if (value == 0b10 && ((_a = this._trigsrc[ch]) !== null && _a !== void 0 ? _a : SERCOM4_DMAC_ID_TX) == SERCOM4_DMAC_ID_TX) {
                // TODO use wrb appropriately
                if (!this._descriptor[ch]) {
                    this._descriptor[ch] = this._baseAddr + ch * 0x10;
                }
                // TODO use btctrl
                var desc = this._descriptor[ch];
                var btctrl = processor.fetchHalfword(desc);
                var btcnt = processor.fetchHalfword(desc + 0x02);
                var srcaddr = processor.fetchWord(desc + 0x04);
                var dstaddr = processor.fetchWord(desc + 0x08);
                var descaddr = processor.fetchWord(desc + 0x0C);
                // console.log(`btctrl: 0x${btctrl.toString(16)}; btcnt: #${btcnt}; srcaddr: 0x${srcaddr.toString(16)}; dstaddr: 0x${dstaddr.toString(16)}; descaddr: 0x${descaddr.toString(16)};`)
                for (var i = 0; i < btcnt; i++) {
                    processor.writeByte(dstaddr, processor.fetchByte(srcaddr + i - btcnt));
                }
                this._descriptor[ch] = descaddr;
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
