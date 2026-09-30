"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.PortRegister = void 0;
const DIR_OFFSET = 0x00;
const DIRCLR_OFFSET = 0x04;
const DIRSET_OFFSET = 0x08;
const DIRTGL_OFFSET = 0x0C;
const OUT_OFFSET = 0x10;
const OUTCLR_OFFSET = 0x14;
const OUTSET_OFFSET = 0x18;
const OUTTGL_OFFSET = 0x1C;
const IN_OFFSET = 0x20;
class PortRegister {
    constructor(offset, processor) {
        this._outListeners = [];
        processor.registerPeripheralReadHandler(offset + OUT_OFFSET, (address) => {
            return this._out;
        });
        processor.registerPeripheralReadHandler(offset + OUTCLR_OFFSET, (address) => {
            return this._out;
        });
        processor.registerPeripheralReadHandler(offset + OUTSET_OFFSET, (address) => {
            return this._out;
        });
        processor.registerPeripheralReadHandler(offset + OUTTGL_OFFSET, (address) => {
            return this._out;
        });
        processor.registerPeripheralReadHandler(offset + IN_OFFSET, (address) => {
            return this._in;
        });
        processor.registerPeripheralReadHandler(offset + DIR_OFFSET, (address) => {
            return this._dir;
        });
        processor.registerPeripheralReadHandler(offset + DIRCLR_OFFSET, (address) => {
            return this._dir;
        });
        processor.registerPeripheralReadHandler(offset + DIRSET_OFFSET, (address) => {
            return this._dir;
        });
        processor.registerPeripheralReadHandler(offset + DIRTGL_OFFSET, (address) => {
            return this._dir;
        });
        processor.registerPeripheralWriteHandler(offset + OUT_OFFSET, (address, value) => {
            var diff = this._out ^ value;
            this._out = value;
            this.handleOutModified(diff);
        });
        processor.registerPeripheralWriteHandler(offset + OUTCLR_OFFSET, (address, value) => {
            //console.log(`PORT OUTCLR ${address.toString(16)} ${value.toString(16)}`);            
            var newVal = this._out & ~value;
            var diff = this._out ^ newVal;
            this._out = newVal;
            this.handleOutModified(diff);
        });
        processor.registerPeripheralWriteHandler(offset + OUTSET_OFFSET, (address, value) => {
            //console.log(`PORT OUTSET ${address.toString(16)} ${value.toString(16)}`);
            var newVal = this._out | value;
            var diff = this._out ^ newVal;
            this._out = newVal;
            this.handleOutModified(diff);
        });
        processor.registerPeripheralWriteHandler(offset + OUTTGL_OFFSET, (address, value) => {
            var newVal = this._out ^ value;
            var diff = this._out ^ newVal;
            this._out = newVal;
            this.handleOutModified(diff);
        });
        processor.registerPeripheralWriteHandler(offset + DIR_OFFSET, (address, value) => {
            this._dir = this._dir ^ value;
        });
        processor.registerPeripheralWriteHandler(offset + DIRCLR_OFFSET, (address, value) => {
            this._dir = this._dir & ~value;
        });
        processor.registerPeripheralWriteHandler(offset + DIRSET_OFFSET, (address, value) => {
            //console.log(`PORT DIRSET ${address.toString(16)} ${value.toString(16)}`);
            this._dir = this._dir | value;
        });
        processor.registerPeripheralWriteHandler(offset + DIRTGL_OFFSET, (address, value) => {
            this._dir = this._dir ^ value;
        });
    }
    addOutListener(listener) {
        this._outListeners.push(listener);
    }
    handleOutModified(diff) {
        if (diff) {
            this._outListeners.forEach(listener => {
                listener(diff, this._out);
            });
        }
    }
    getOut() {
        return this._out;
    }
}
exports.PortRegister = PortRegister;
