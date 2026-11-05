// make_icon.swift DIR: draws the app icon's two windows as transparent 1024x1024 layers, for
// ios/Henge/AppIcon.icon/Assets (Icon Composer's format: iOS adds the glass). l0 is the front one.
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let dir = CommandLine.arguments[1]
let srgb = CGColorSpace(name: CGColorSpace.sRGB)!
func col(_ h: UInt32) -> CGColor { CGColor(srgbRed: CGFloat(h >> 16 & 255) / 255, green: CGFloat(h >> 8 & 255) / 255, blue: CGFloat(h & 255) / 255, alpha: 1) }

func window(_ file: String, _ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat, _ light: UInt32, _ dark: UInt32) {
    let c = CGContext(data: nil, width: 1024, height: 1024, bitsPerComponent: 8, bytesPerRow: 0, space: srgb, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.translateBy(x: 0, y: 1024); c.scaleBy(x: 1, y: -1)
    let r: CGFloat = 78, d: CGFloat = 22
    c.saveGState()
    c.addPath(CGPath(roundedRect: CGRect(x: x, y: y, width: w, height: h), cornerWidth: r, cornerHeight: r, transform: nil))
    c.clip()
    c.drawLinearGradient(CGGradient(colorsSpace: srgb, colors: [col(light), col(dark)] as CFArray, locations: [0, 1])!, start: CGPoint(x: 0, y: y), end: CGPoint(x: 0, y: y + h), options: [])
    c.restoreGState()
    for (k, dot) in [UInt32(0xFF6A5C), 0xFFC23D, 0x38D26A].enumerated() {
        c.setFillColor(col(dot))
        c.fillEllipse(in: CGRect(x: x + r * 0.62 + CGFloat(k) * d * 2.9 - d, y: y + r * 0.62 + d * 0.3 - d, width: 2 * d, height: 2 * d))
    }
    let out = CGImageDestinationCreateWithURL(URL(fileURLWithPath: dir + "/" + file) as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(out, c.makeImage()!, nil)
    CGImageDestinationFinalize(out)
}

window("l0.png", 300, 356, 590, 480, 0xFFFFFF, 0xE3E9FF)
window("l1.png", 134, 190, 590, 480, 0xCFE0FF, 0x8FB0F6)
