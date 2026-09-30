"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.SercomRegister = void 0;
const SERCOM0_ADDR = 0x42000800;
const INTFLAG_OFFSET = 0x18;
const DATA_OFFSET = 0x28;
class SercomRegister {
    constructor(index, processor) {
        this._dataListeners = [];
        this.data = 0x80;
        var baseAddr = SERCOM0_ADDR + index * 0x400;
        processor.registerPeripheralReadHandler(baseAddr + INTFLAG_OFFSET, (address) => {
            // console.log(`In read SERCOM${index} INTFLAG`);
            return 0b00000111; // RXC, TXC, DRE 
        });
        processor.registerPeripheralWriteHandler(baseAddr + DATA_OFFSET, (address, value) => {
            value = value & 0xff;
            // console.log(`SENDING DATA 0x${value.toString(16)} ${String.fromCharCode(value)}`);
            this.data = 0x80;
            this._dataListeners.forEach(listener => {
                listener(value);
            });
        });
        processor.registerPeripheralReadHandler(baseAddr + DATA_OFFSET, (address) => {
            return this.data;
        });
    }
    registerDataListener(listener) {
        this._dataListeners.push(listener);
    }
}
exports.SercomRegister = SercomRegister;
