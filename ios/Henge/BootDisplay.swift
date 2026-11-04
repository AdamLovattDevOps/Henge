import SwiftUI

/// The guest's boot framebuffer: what iBoot and the kernel draw before a GPU
/// driver takes over (the Apple logo and the progress bar), refreshed a few
/// times a second. iOS has no paravirtualized GPU, so this is all of the
/// Mac's screen Henge can show for now.
struct BootDisplay: View {
    @State private var image: CGImage?

    var body: some View {
        Group {
            if let image {
                Image(decorative: image, scale: 1).resizable().interpolation(.medium).aspectRatio(contentMode: .fit)
            } else {
                Color.black
            }
        }
        .task {
            while !Task.isCancelled {
                image = Self.snapshot() ?? image
                try? await Task.sleep(nanoseconds: 400_000_000)
            }
        }
    }

    static func snapshot() -> CGImage? {
        var px: UnsafePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0, stride: Int32 = 0
        guard henge_vm_boot_display(&px, &w, &h, &stride) == 0, let px else { return nil }
        let bytes = Int(stride) * Int(h)
        let data = Data(bytes: px, count: bytes) // a copy: the guest keeps drawing
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        return CGImage(width: Int(w), height: Int(h), bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: Int(stride),
                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info, provider: provider, decode: nil,
                       shouldInterpolate: true, intent: .defaultIntent)
    }
}
