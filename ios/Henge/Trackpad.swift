import SwiftUI
import UIKit

/// A surface that works like a Mac's trackpad. One finger moves the pointer; a tap clicks; a finger
/// held still for a moment presses the button until it lifts, which is how to drag. Two fingers
/// scroll, and tapped together they right-click. Pointer speed suits a 1920-wide screen on a small one.
struct Trackpad: UIViewRepresentable {
    func makeUIView(context: Context) -> Surface { Surface() }
    func updateUIView(_ view: Surface, context: Context) {}

    final class Surface: UIView {
        private static let speed: CGFloat = 3
        private static let holdTime: TimeInterval = 0.4
        private static let tapTime: TimeInterval = 0.3
        private static let tapDistance: CGFloat = 8
        /// Points of two-finger travel to one unit of the wheel. macOS moves a page about half a pixel
        /// per unit (measured), so this scrolls about as far as the pointer would move.
        private static let scrollStep: CGFloat = 1.0 / 6

        private var fingers: [UITouch] = []
        private var started = Date()
        private var travelled: CGFloat = 0
        private var hadTwoFingers = false
        private var holding = false
        private var holdTimer: Timer?
        private var scrollX: CGFloat = 0
        private var scrollY: CGFloat = 0
        private let buzz = UIImpactFeedbackGenerator(style: .medium)

        override init(frame: CGRect) {
            super.init(frame: frame)
            isMultipleTouchEnabled = true
            backgroundColor = .clear
        }

        required init?(coder: NSCoder) { fatalError("made in code") }

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
            if fingers.isEmpty {
                started = Date()
                travelled = 0
                hadTwoFingers = false
                scrollX = 0
                scrollY = 0
                buzz.prepare()
            }
            fingers.append(contentsOf: touches)
            if fingers.count >= 2 {
                hadTwoFingers = true
                cancelHold()
            } else {
                armHold()
            }
        }

        override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
            guard let finger = fingers.first, touches.contains(finger) || fingers.count >= 2 else { return }
            let now = finger.location(in: self), before = finger.previousLocation(in: self)
            let dx = now.x - before.x, dy = now.y - before.y
            travelled += abs(dx) + abs(dy)
            if fingers.count >= 2 {
                scroll(dx, dy)
            } else {
                if travelled >= Self.tapDistance, !holding { cancelHold() }
                henge_vm_mouse_move(Int32((dx * Self.speed).rounded()), Int32((dy * Self.speed).rounded()))
            }
        }

        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }
        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }

        private func lift(_ touches: Set<UITouch>) {
            fingers.removeAll { touches.contains($0) }
            guard fingers.isEmpty else { return }
            cancelHold()
            if holding {
                holding = false
                henge_vm_mouse_button(0, 0)
                StickyKeys.shared.releaseAll()
                return
            }
            guard travelled < Self.tapDistance, Date().timeIntervalSince(started) < Self.tapTime else { return }
            let button: Int32 = hadTwoFingers ? 1 : 0
            henge_vm_mouse_button(button, 1)
            henge_vm_mouse_button(button, 0)
            StickyKeys.shared.releaseAll()
        }

        private func armHold() {
            holdTimer?.invalidate()
            holdTimer = Timer.scheduledTimer(withTimeInterval: Self.holdTime, repeats: false) { [weak self] _ in
                guard let self, self.fingers.count == 1, self.travelled < Self.tapDistance else { return }
                self.holding = true
                henge_vm_mouse_button(0, 1)
                self.buzz.impactOccurred()
            }
        }

        private func cancelHold() {
            holdTimer?.invalidate()
            holdTimer = nil
        }

        /// As on a Mac's trackpad, the page follows the fingers. The wheel only takes whole units.
        private func scroll(_ dx: CGFloat, _ dy: CGFloat) {
            scrollX += dx
            scrollY += dy
            let notchesX = Int32((scrollX / Self.scrollStep).rounded(.towardZero))
            let notchesY = Int32((scrollY / Self.scrollStep).rounded(.towardZero))
            guard notchesX != 0 || notchesY != 0 else { return }
            scrollX -= CGFloat(notchesX) * Self.scrollStep
            scrollY -= CGFloat(notchesY) * Self.scrollStep
            henge_vm_mouse_scroll(-notchesX, notchesY)
        }
    }
}

/// One mouse button you can hold: down when touched, up when let go.
struct MouseButton: View {
    let title: String
    let button: Int32
    @State private var down = false

    var body: some View {
        Text(title)
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(down ? Desk.text : Desk.dim)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(down ? Desk.raised : Color.clear)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        guard !down else { return }
                        down = true
                        henge_vm_mouse_button(button, 1)
                        UIImpactFeedbackGenerator(style: .light).impactOccurred()
                    }
                    .onEnded { _ in
                        down = false
                        henge_vm_mouse_button(button, 0)
                        StickyKeys.shared.releaseAll()
                    }
            )
            .accessibilityLabel(title)
    }
}
