// make_icon.swift OUT.png [copper|indigo]: draws Henge's app icon, three standing stones in soft white
// on a deep gradient, 1024x1024, opaque and square (iOS rounds the corners).
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

struct RGB {
    var r, g, b: CGFloat
    init(_ hex: UInt32) {
        r = CGFloat(hex >> 16 & 255) / 255; g = CGFloat(hex >> 8 & 255) / 255; b = CGFloat(hex & 255) / 255
    }
    var cg: CGColor { CGColor(srgbRed: r, green: g, blue: b, alpha: 1) }
}

struct Palette { let top, bottom, glow, shadow: RGB }
let palettes: [String: Palette] = [
    "copper": Palette(top: RGB(0xF0A25A), bottom: RGB(0xAE4C1A), glow: RGB(0xFFDDB0), shadow: RGB(0x4E1E06)),
    "indigo": Palette(top: RGB(0x5B6CF0), bottom: RGB(0x2A2A9E), glow: RGB(0x8FA0FF), shadow: RGB(0x12124F)),
]

let args = CommandLine.arguments
let out = args[1], palette = palettes[args.count > 2 ? args[2] : "copper"]!
let ctx = CGContext(data: nil, width: 1024, height: 1024, bitsPerComponent: 8, bytesPerRow: 0,
                    space: CGColorSpace(name: CGColorSpace.sRGB)!,
                    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
// y grows downward from here on, as on the screen.
ctx.translateBy(x: 0, y: 1024)
ctx.scaleBy(x: 1, y: -1)

func gradient(_ a: CGColor, _ b: CGColor) -> CGGradient {
    CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB)!, colors: [a, b] as CFArray, locations: [0, 1])!
}

// The ground: one colour getting deeper downward, lit softly from above.
ctx.drawLinearGradient(gradient(palette.top.cg, palette.bottom.cg), start: .zero, end: CGPoint(x: 0, y: 1024), options: [])
ctx.drawRadialGradient(gradient(palette.glow.cg.copy(alpha: 0.5)!, palette.glow.cg.copy(alpha: 0)!),
                       startCenter: CGPoint(x: 512, y: -60), startRadius: 0,
                       endCenter: CGPoint(x: 512, y: -60), endRadius: 780, options: [])

// A four-sided stone with round corners: top left and right, then the bottom's.
func stonePath(_ tl: CGPoint, _ tr: CGPoint, _ br: CGPoint, _ bl: CGPoint, radius: CGFloat) -> CGPath {
    let p = CGMutablePath()
    p.move(to: CGPoint(x: (tl.x + tr.x) / 2, y: tl.y))
    p.addArc(tangent1End: tr, tangent2End: br, radius: radius)
    p.addArc(tangent1End: br, tangent2End: bl, radius: radius)
    p.addArc(tangent1End: bl, tangent2End: tl, radius: radius)
    p.addArc(tangent1End: tl, tangent2End: tr, radius: radius)
    p.closeSubpath()
    return p
}

func fill(_ path: CGPath, _ light: UInt32, _ dark: UInt32, _ y0: CGFloat, _ y1: CGFloat) {
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()
    ctx.drawLinearGradient(gradient(RGB(light).cg, RGB(dark).cg), start: CGPoint(x: 0, y: y0), end: CGPoint(x: 0, y: y1), options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
    ctx.restoreGState()
}

// The two uprights lean in a little; the lintel lies across them and casts its shadow down them.
let top: CGFloat = 262, capHeight: CGFloat = 164, bottom: CGFloat = 770
let underCap = top + capHeight
func upright(_ x0: CGFloat, _ x1: CGFloat) {
    let lean: CGFloat = 16
    let path = stonePath(CGPoint(x: x0 + lean, y: top + 60), CGPoint(x: x1 - lean, y: top + 60), CGPoint(x: x1, y: bottom), CGPoint(x: x0, y: bottom), radius: 34)
    fill(path, 0xF4F6FA, 0xCDD3DE, top + 60, bottom)
    let shade = RGB(0x262B38).cg
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()
    ctx.drawLinearGradient(gradient(shade.copy(alpha: 0.38)!, shade.copy(alpha: 0)!), start: CGPoint(x: 0, y: underCap - 4), end: CGPoint(x: 0, y: underCap + 96), options: [])
    ctx.restoreGState()
}

ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -32), blur: 76, color: palette.shadow.cg.copy(alpha: 0.6))
ctx.beginTransparencyLayer(auxiliaryInfo: nil)
upright(232, 452)
upright(572, 792)
let lintel = stonePath(CGPoint(x: 196, y: top), CGPoint(x: 828, y: top), CGPoint(x: 828, y: underCap), CGPoint(x: 196, y: underCap), radius: 44)
fill(lintel, 0xFFFFFF, 0xE6E9F0, top, underCap)
ctx.endTransparencyLayer()
ctx.restoreGState()

let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: out) as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
CGImageDestinationFinalize(dest)
