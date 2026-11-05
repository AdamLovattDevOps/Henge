// make_icon.swift DIR: draws the stones of the app icon as transparent 1024x1024 layers, for ios/Henge/AppIcon.icon/Assets (Icon Composer's format: iOS adds the glass).
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers
let dir = CommandLine.arguments[1]
func newLayer() -> CGContext {
    let c = CGContext(data: nil, width: 1024, height: 1024, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    c.translateBy(x: 0, y: 1024); c.scaleBy(x: 1, y: -1)
    return c
}
func save(_ c: CGContext, _ name: String) {
    let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: dir + "/" + name) as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, c.makeImage()!, nil); CGImageDestinationFinalize(d)
}
func stone(_ tl: CGPoint, _ tr: CGPoint, _ br: CGPoint, _ bl: CGPoint, _ radius: CGFloat) -> CGPath {
    let p = CGMutablePath()
    p.move(to: CGPoint(x: (tl.x + tr.x) / 2, y: tl.y))
    p.addArc(tangent1End: tr, tangent2End: br, radius: radius)
    p.addArc(tangent1End: br, tangent2End: bl, radius: radius)
    p.addArc(tangent1End: bl, tangent2End: tl, radius: radius)
    p.addArc(tangent1End: tl, tangent2End: tr, radius: radius)
    p.closeSubpath()
    return p
}
func fillGradient(_ c: CGContext, _ path: CGPath, _ a: UInt32, _ b: UInt32, _ y0: CGFloat, _ y1: CGFloat) {
    func col(_ h: UInt32) -> CGColor { CGColor(srgbRed: CGFloat(h >> 16 & 255) / 255, green: CGFloat(h >> 8 & 255) / 255, blue: CGFloat(h & 255) / 255, alpha: 1) }
    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB)!, colors: [col(a), col(b)] as CFArray, locations: [0, 1])!
    c.saveGState(); c.addPath(path); c.clip()
    c.drawLinearGradient(g, start: CGPoint(x: 0, y: y0), end: CGPoint(x: 0, y: y1), options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
    c.restoreGState()
}
// A little smaller than the canvas: the rim wants room around it.
let top: CGFloat = 282, capH: CGFloat = 152, bottom: CGFloat = 746, lean: CGFloat = 14
let up = newLayer()
for (x0, x1) in [(CGFloat(262), CGFloat(462)), (CGFloat(562), CGFloat(762))] {
    let p = stone(CGPoint(x: x0 + lean, y: top + 60), CGPoint(x: x1 - lean, y: top + 60), CGPoint(x: x1, y: bottom), CGPoint(x: x0, y: bottom), 40)
    fillGradient(up, p, 0xF6F6FF, 0xC9CBF2, top + capH, bottom)
}
save(up, "uprights.png")
let cap = newLayer()
let cp = stone(CGPoint(x: 226, y: top), CGPoint(x: 798, y: top), CGPoint(x: 798, y: top + capH), CGPoint(x: 226, y: top + capH), 50)
fillGradient(cap, cp, 0xFFFFFF, 0xDFE1F6, top, top + capH)
save(cap, "lintel.png")
