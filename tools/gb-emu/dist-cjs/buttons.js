"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.Buttons = void 0;
class Buttons {
    constructor(sercom, portA, portB) {
        this._keymap = [
            [83, 40],
            [65, 81, 37],
            [68, 39],
            [87, 90, 38],
            [74],
            [75],
            [85],
            [73] // HOME
        ];
        this._portA = portA;
        this._portB = portB;
        this._sercom = sercom;
        this._buttonData = 0xff; // nothing pressed
        sercom.registerDataListener(this.setButtonData.bind(this));
        document.addEventListener('keydown', (event) => {
            for (var i = 0; i < this._keymap.length; i++) {
                for (var code of this._keymap[i]) {
                    if (code == event.keyCode) {
                        // Stop normal behavior of this button
                        event.preventDefault();
                        // Set the correct bit to 0
                        this._buttonData &= (~(1 << i));
                        break;
                    }
                }
            }
        });
        document.addEventListener('keyup', (event) => {
            for (var i = 0; i < this._keymap.length; i++) {
                for (var code of this._keymap[i]) {
                    if (code == event.keyCode) {
                        // Set the correct bit to 1
                        this._buttonData |= (1 << i);
                        break;
                    }
                }
            }
        });
    }
    setButtonData() {
        if ((this._portB.getOut() & (1 << 3)) != 0)
            return;
        this._sercom.data = this._buttonData;
    }
    setKeymap(keymap) {
        this._keymap = keymap;
    }
    customButtonData(buttonData) {
        this._buttonData = buttonData;
    }
}
exports.Buttons = Buttons;
