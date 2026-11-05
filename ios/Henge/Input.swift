import SwiftUI
import UIKit

/// Types into the Mac: an invisible view that holds the iPhone's keyboard.
struct KeyboardCatcher: UIViewRepresentable {
    @Binding var active: Bool

    func makeUIView(context: Context) -> CatcherView { CatcherView() }

    func updateUIView(_ view: CatcherView, context: Context) {
        DispatchQueue.main.async {
            if active, !view.isFirstResponder { view.becomeFirstResponder() }
            if !active, view.isFirstResponder { view.resignFirstResponder() }
        }
    }

    final class CatcherView: UIView, UIKeyInput {
        override var canBecomeFirstResponder: Bool { true }
        var hasText: Bool { true }
        var autocorrectionType: UITextAutocorrectionType = .no
        var autocapitalizationType: UITextAutocapitalizationType = .none
        var spellCheckingType: UITextSpellCheckingType = .no
        var smartQuotesType: UITextSmartQuotesType = .no
        var smartDashesType: UITextSmartDashesType = .no

        func insertText(_ text: String) {
            for c in text { Keys.type(c) }
        }

        func deleteBackward() { Keys.press(Keys.backspace) }
    }
}

/// Linux input key codes, which the Mac's keyboard device takes.
enum Keys {
    static let backspace: UInt16 = 14
    static let shift: UInt16 = 42
    private static let letters: [Character: UInt16] = [
        "q": 16, "w": 17, "e": 18, "r": 19, "t": 20, "y": 21, "u": 22, "i": 23, "o": 24, "p": 25,
        "a": 30, "s": 31, "d": 32, "f": 33, "g": 34, "h": 35, "j": 36, "k": 37, "l": 38,
        "z": 44, "x": 45, "c": 46, "v": 47, "b": 48, "n": 49, "m": 50,
    ]
    private static let plain: [Character: UInt16] = [
        "1": 2, "2": 3, "3": 4, "4": 5, "5": 6, "6": 7, "7": 8, "8": 9, "9": 10, "0": 11,
        "-": 12, "=": 13, "\t": 15, "[": 26, "]": 27, "\n": 28, ";": 39, "'": 40, "`": 41, "\\": 43,
        ",": 51, ".": 52, "/": 53, " ": 57,
    ]
    private static let shifted: [Character: UInt16] = [
        "!": 2, "@": 3, "#": 4, "$": 5, "%": 6, "^": 7, "&": 8, "*": 9, "(": 10, ")": 11,
        "_": 12, "+": 13, "{": 26, "}": 27, ":": 39, "\"": 40, "~": 41, "|": 43,
        "<": 51, ">": 52, "?": 53,
    ]

    static func press(_ code: UInt16, shifted: Bool = false) {
        if shifted { henge_vm_key(shift, 1) }
        henge_vm_key(code, 1)
        henge_vm_key(code, 0)
        if shifted { henge_vm_key(shift, 0) }
    }

    static func type(_ c: Character) {
        if let code = letters[c] { press(code) }
        else if let lower = c.lowercased().first, let code = letters[lower] { press(code, shifted: true) }
        else if let code = plain[c] { press(code) }
        else if let code = shifted[c] { press(code, shifted: true) }
    }
}
