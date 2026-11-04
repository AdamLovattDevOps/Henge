import SwiftUI
import UIKit

enum Stone {
    private static func adaptive(dark: UIColor, light: UIColor) -> Color {
        Color(uiColor: UIColor { $0.userInterfaceStyle == .dark ? dark : light })
    }
    private static func rgb(_ r: Int, _ g: Int, _ b: Int) -> UIColor {
        UIColor(red: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: 1)
    }

    static let page = adaptive(dark: rgb(20, 18, 15), light: rgb(246, 241, 232))
    static let card = adaptive(dark: rgb(32, 28, 23), light: .white)
    static let raised = adaptive(dark: rgb(44, 38, 31), light: rgb(236, 228, 216))
    static let text = adaptive(dark: rgb(244, 239, 230), light: rgb(28, 22, 16))
    static let dim = adaptive(dark: rgb(244, 239, 230).withAlphaComponent(0.58), light: rgb(28, 22, 16).withAlphaComponent(0.55))
    static let faint = adaptive(dark: rgb(244, 239, 230).withAlphaComponent(0.32), light: rgb(28, 22, 16).withAlphaComponent(0.32))
    static let copper = Color(red: 0.894, green: 0.631, blue: 0.353)
    static let copperDeep = Color(red: 0.29, green: 0.18, blue: 0.08)
    static let lichen = Color(red: 0.545, green: 0.639, blue: 0.478)
    static let hairline = adaptive(dark: UIColor(white: 1, alpha: 0.08), light: UIColor(white: 0, alpha: 0.08))

    /// The session is a screen, so it stays dark in either appearance.
    static let bezel = Color(red: 0.05, green: 0.045, blue: 0.038)
    static let bezelText = Color(red: 0.96, green: 0.94, blue: 0.90)
    static let bezelDim = Color.white.opacity(0.55)
}

extension Font {
    static func stone(_ size: CGFloat, weight: Font.Weight = .semibold) -> Font {
        .system(size: size, weight: weight, design: .serif)
    }
}

struct StonesMark: View {
    var height: CGFloat = 28
    var body: some View {
        HStack(alignment: .bottom, spacing: height * 0.14) {
            pillar(0.62)
            pillar(1)
            pillar(0.76)
        }
        .frame(height: height)
    }

    private func pillar(_ scale: CGFloat) -> some View {
        RoundedRectangle(cornerRadius: 2, style: .continuous)
            .fill(Stone.copper)
            .frame(width: height * 0.22, height: height * scale)
    }
}

extension View {
    func stoneCard() -> some View {
        background(Stone.card, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous).stroke(Stone.hairline, lineWidth: 1))
    }
}

enum ByteText {
    static func string(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        formatter.allowedUnits = [.useGB, .useMB]
        formatter.includesUnit = true
        return formatter.string(fromByteCount: bytes)
    }

    static func string(_ bytes: UInt64) -> String {
        string(Int64(clamping: bytes))
    }
}

/// The names the JIT setup screens (from Husk) use, in Henge's colours.
enum Theme {
    static let accent = Stone.copper
    static let accentSoft = Stone.copper.opacity(0.18)
    static let backdrop = Stone.page
    static let text = Stone.text
    static let textDim = Stone.dim
}

extension View {
    /// high: a card raised above the others (a result, an outcome).
    func hengeCard(high: Bool = false) -> some View {
        background(high ? Stone.raised : Stone.card, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous).stroke(Stone.hairline, lineWidth: 1))
    }
}

/// The one action a screen is for (the JIT setup screens use it).
struct PrimaryButtonStyle: ButtonStyle {
    var enabled = true

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(enabled ? Stone.copperDeep : Stone.dim)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 15)
            .background(enabled ? Stone.copper : Stone.raised, in: RoundedRectangle(cornerRadius: 15, style: .continuous))
            .opacity(configuration.isPressed ? 0.85 : 1)
            .scaleEffect(configuration.isPressed ? 0.985 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// A card that is also a button: it moves a little under the finger.
struct CardButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .opacity(configuration.isPressed ? 0.9 : 1)
            .animation(.easeOut(duration: 0.14), value: configuration.isPressed)
    }
}
