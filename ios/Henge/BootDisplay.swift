import SwiftUI

/// The Mac's screen, refreshed a few times a second: what its GPU draws once
/// macOS is up. Before that, the boot framebuffer, which only ever holds the
/// progress bar (a virtual Mac's firmware draws no logo into it), so the logo
/// over it is the app's own.
struct BootDisplay: View {
    @State private var image: CGImage?

    var body: some View {
        ZStack {
            Color.black
            if let image {
                Image(decorative: image, scale: 1).resizable().interpolation(.medium).aspectRatio(contentMode: .fit)
                if image.height > image.width {
                    Image(systemName: "apple.logo")
                        .font(.system(size: 44))
                        .foregroundStyle(.white)
                }
            } else {
                Image(systemName: "apple.logo")
                    .font(.system(size: 44))
                    .foregroundStyle(.white)
            }
        }
        .task {
            while !Task.isCancelled {
                image = Self.snapshot() ?? image
                try? await Task.sleep(nanoseconds: 66_000_000)
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
