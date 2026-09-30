"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.SdCard = void 0;
/* SPI-mode SD card backed by an in-memory image (FAT16/FAT32 card dump or
 * image built with mkfs.fat/mcopy).  Implements the exact command surface
 * the gbl firmware uses (meta_sd.cpp): CMD0, CMD8, CMD55+ACMD41, CMD58,
 * CMD17 (read one 512-byte sector) and CMD24 (write one).  Block (SDHC)
 * addressing is always reported.  Selected on PA27 (SD_CS).
 *
 * SPI timing model: the byte the card drives during exchange N was decided
 * by the byte received in exchange N-1.  _pending holds that byte; each
 * received byte publishes it (sercom.data) and processes the received one. */
class SdCard {
    constructor(portA, sercom) {
        this._image = null;
        this._pending = 0xff; // byte driven during the current exchange
        this._out = []; // bytes driven on the following exchanges
        this._cmdBuf = new Uint8Array(6);
        this._cmdIdx = 0;
        this._writing = false; // CMD24 data phase (token + 512 + 2 CRC)
        this._writeBuf = new Uint8Array(515);
        this._writeIdx = 0;
        this._writeLba = 0;
        this._initialized = false;
        this._writeListener = null;
        this._portA = portA;
        this._sercom = sercom;
        sercom.registerDataListener(this.byteReceived.bind(this));
    }
    /* Observateur optionnel : (lba, secteur de 512 o) après chaque écriture
     * acceptée — permet de réécrire le fichier correspondant sur disque. */
    setWriteListener(fn) {
        this._writeListener = fn;
    }
    loadImage(image) {
        this._image = image;
        this._initialized = false;
        this.resetState();
    }
    resetState() {
        this._pending = 0xff;
        this._out.length = 0;
        this._cmdIdx = 0;
        this._writing = false;
        this._sercom.data = 0xff;
    }
    byteReceived(value) {
        if ((this._portA.getOut() & (1 << 27)) !== 0) { // not selected
            this.resetState();
            return;
        }
        this._sercom.data = this._pending;
        this._pending = this._out.length ? this._out.shift() : 0xff;
        this.process(value);
    }
    /* Consume one received byte; fill _out with what the card will say next. */
    process(value) {
        var _a;
        if (this._writing) {
            if (this._writeIdx === 0 && value === 0xff)
                return; // dummy avant le token
            this._writeBuf[this._writeIdx++] = value;
            if (this._writeIdx >= 515) {
                this._writing = false;
                if (this._image && this._writeBuf[0] === 0xfe) {
                    this._image.set(this._writeBuf.subarray(1, 513), this._writeLba * 512);
                    const sector = this._image.subarray(this._writeLba * 512, this._writeLba * 512 + 512);
                    (_a = this._writeListener) === null || _a === void 0 ? void 0 : _a.call(this, this._writeLba, sector);
                }
                this._out.push(0x05, 0x00, 0x00, 0xff); // accepted, then busy
            }
            return;
        }
        if (this._cmdIdx > 0) { // command frame continuation
            this._cmdBuf[this._cmdIdx++] = value;
            if (this._cmdIdx >= 6) {
                this._cmdIdx = 0;
                this.command(this._cmdBuf[0] & 0x3f, (((this._cmdBuf[1] << 24) | (this._cmdBuf[2] << 16) |
                    (this._cmdBuf[3] << 8) | this._cmdBuf[4]) >>> 0));
            }
            return;
        }
        if ((value & 0xc0) === 0x40) { // command start (0xff waits are ignored)
            this._cmdBuf[this._cmdIdx++] = value;
        }
    }
    command(cmd, arg) {
        switch (cmd) {
            case 0: // GO_IDLE_STATE
                this._out.push(this._initialized ? 0x00 : 0x01);
                break;
            case 8: // SEND_IF_COND: SD v2, echo voltage/check pattern
                this._out.push(0x01, 0x00, 0x00, 0x01, 0xaa);
                break;
            case 55: // APP_CMD
                this._out.push(0x01);
                break;
            case 41: // SD_SEND_OP_COND: ready at once
                this._initialized = true;
                this._out.push(0x00);
                break;
            case 58: // READ_OCR: busy + CCS (block addressing)
                this._out.push(0x00, 0xc0, 0x00, 0x00, 0x00);
                break;
            case 16: // SET_BLOCKLEN: accepted, ignored
                this._out.push(0x00);
                break;
            case 17: { // READ_SINGLE_BLOCK
                const base = arg * 512;
                if (this._image && base + 512 <= this._image.length) {
                    this._out.push(0x00, 0xfe);
                    for (let i = 0; i < 512; i++)
                        this._out.push(this._image[base + i]);
                    this._out.push(0xff, 0xff); // CRC
                }
                else {
                    this._out.push(0x04); // illegal address
                }
                break;
            }
            case 24: // WRITE_SINGLE_BLOCK
                this._out.push(0x00);
                this._writeLba = arg;
                this._writeIdx = 0;
                this._writing = true;
                break;
            default:
                this._out.push(0x04); // illegal command
                break;
        }
    }
}
exports.SdCard = SdCard;
