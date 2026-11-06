import SwiftUI
import QuartzCore

/// The Mac's screen, refreshed a few times a second: what its GPU draws once
/// macOS is up. Before that, the boot framebuffer, which only ever holds the
/// progress bar (a virtual Mac's firmware draws no logo into it), so the logo
/// over it is the app's own.
struct BootDisplay: View {
    let display: DisplayRouter
    let role: DisplayTargetSelection.Role

    @State private var image: CGImage?
    @State private var gpuLive = false

    var body: some View {
        ZStack {
            Color.black
            if !gpuLive {
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
            GPUScreen(display: display, role: role).allowsHitTesting(false)
        }
        .task {
            // Once macOS draws with the GPU the frames go straight to GPUScreen:
            // nothing to copy here any more.
            while !Task.isCancelled && !gpuLive {
                if henge_vm_gpu_frames() > 0 {
                    gpuLive = true
                    image = nil
                    break
                }
                image = Self.snapshot() ?? image
                try? await Task.sleep(nanoseconds: 100_000_000)
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

/// A layer the engine draws the Mac's frames into, with the GPU, without the main thread.
private struct GPUScreen: UIViewRepresentable {
    let display: DisplayRouter
    let role: DisplayTargetSelection.Role

    final class Screen: UIView {
        override class var layerClass: AnyClass { CAMetalLayer.self }

        private let id = UUID()
        private let display: DisplayRouter
        private let role: DisplayTargetSelection.Role
        private var attached = false
        private var lastSize: CGSize = .zero

        init(display: DisplayRouter, role: DisplayTargetSelection.Role) {
            self.display = display
            self.role = role
            super.init(frame: .zero)
            backgroundColor = .clear
            isOpaque = false
            layer.contentsGravity = .resizeAspect
            layer.isOpaque = false
        }

        required init?(coder: NSCoder) { fatalError("made in code") }

        override func didMoveToWindow() {
            super.didMoveToWindow()
            if window == nil { detach() } else { attachIfReady() }
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            attachIfReady()
        }

        private func attachIfReady() {
            guard window != nil, bounds.width > 0, bounds.height > 0,
                  let metal = layer as? CAMetalLayer else { return }
            if !attached {
                attached = true
                lastSize = bounds.size
                display.connect(id, layer: metal, role: role)
            } else if lastSize != bounds.size {
                lastSize = bounds.size
                display.refresh(id)
            }
        }

        func detach() {
            guard attached else { return }
            attached = false
            display.disconnect(id)
        }
    }

    func makeUIView(context: Context) -> Screen { Screen(display: display, role: role) }
    func updateUIView(_ view: Screen, context: Context) {}
    static func dismantleUIView(_ view: Screen, coordinator: ()) { view.detach() }
}
