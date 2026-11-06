import SwiftUI
import UIKit

/// A surface that works like a Mac's trackpad. One finger moves the pointer; a tap clicks; a finger
/// held still for a moment presses the button until it lifts, which is how to drag. Two fingers
/// scroll, and tapped together they right-click. Pointer speed suits a 1920-wide screen on a small
/// one. Only fingers count here: a real trackpad or mouse goes through HardwareMouse.
struct Trackpad: UIViewRepresentable {
    func makeUIView(context: Context) -> Surface { Surface() }
    func updateUIView(_ view: Surface, context: Context) {}

    final class Surface: UIView, UIPointerInteractionDelegate {
        private static let holdTime: TimeInterval = 0.4
        private static let tapTime: TimeInterval = 0.3
        private static let tapDistance: CGFloat = 8
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
            addInteraction(UIPointerInteraction(delegate: self))
        }

        /// Over the Mac there is only the Mac's pointer, if iPadOS hasn't locked its own away.
        func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? { .hidden() }

        /// A full-size iPad has more room to move a finger across than a phone does.
        private var speed: CGFloat { traitCollection.horizontalSizeClass == .regular ? 2 : 3 }

        required init?(coder: NSCoder) { fatalError("made in code") }

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
            let touches = touches.filter { $0.type != .indirectPointer }
            guard !touches.isEmpty else { return }
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
            guard let finger = fingers.first, touches.contains(finger) || (fingers.count >= 2 && touches.contains(where: fingers.contains)) else { return }
            let now = finger.location(in: self), before = finger.previousLocation(in: self)
            let dx = now.x - before.x, dy = now.y - before.y
            travelled += abs(dx) + abs(dy)
            if fingers.count >= 2 {
                scroll(dx, dy)
            } else {
                if travelled >= Self.tapDistance, !holding { cancelHold() }
                henge_vm_mouse_move(Int32((dx * speed).rounded()), Int32((dy * speed).rounded()))
            }
        }

        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }
        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }

        private func lift(_ touches: Set<UITouch>) {
            let touches = touches.filter { $0.type != .indirectPointer }
            guard !touches.isEmpty else { return }
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

        /// As on a Mac's trackpad, the page follows the fingers. macOS accelerates the wheel hard: a page
        /// moves about 0.08 d² pixels for an event of d units (measured: 8 units 4 px, 100 units 970 px).
        /// So d comes from the square root of how far the fingers went, to move the page about three
        /// pixels for each point, the same as the pointer.
        private func scroll(_ dx: CGFloat, _ dy: CGFloat) {
            scrollX += dx
            scrollY += dy
            // Mostly one way is that way only: a page shouldn't drift sideways, or a browser go back.
            if abs(scrollY) > 2 * abs(scrollX) { scrollX = 0 } else if abs(scrollX) > 2 * abs(scrollY) { scrollY = 0 }
            let unitsX = wheelUnits(scrollX), unitsY = wheelUnits(scrollY)
            guard unitsX != 0 || unitsY != 0 else { return }
            if unitsX != 0 { scrollX = 0 }
            if unitsY != 0 { scrollY = 0 }
            henge_vm_mouse_scroll(-unitsX, unitsY)
        }

        private func wheelUnits(_ points: CGFloat) -> Int32 {
            guard abs(points) >= 0.25 else { return 0 } // less than this is a finger resting
            let units = (6 * abs(points).squareRoot()).rounded()
            return Int32(points < 0 ? -units : units)
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
