import Foundation

@main
struct DisplayTargetTests {
    static func main() {
        let phone = UUID(), monitor = UUID(), replacement = UUID()
        var route = DisplayTargetSelection()
        precondition(route.selected == nil)

        // A display arriving before the session must not claim an idle VM.
        route.connect(monitor, role: .external)
        precondition(route.selected == nil && !route.isExternal)
        route.connect(phone, role: .device)
        precondition(route.selected == monitor && route.isExternal)

        // A monitor swap can attach the new view before detaching the old one.
        route.connect(replacement, role: .external)
        route.connect(monitor, role: .external) // late layout of the old surface
        route.disconnect(monitor)
        precondition(route.selected == replacement && route.isExternal)

        // Unplugging restores the very same phone surface, without a VM restart.
        route.disconnect(replacement)
        precondition(route.selected == phone && !route.isExternal)
        route.disconnect(replacement) // duplicate disconnect is harmless
        precondition(route.selected == phone)

        // Ending a session must detach its GPU output even if accessory teardown lags.
        route.connect(monitor, role: .external)
        route.disconnect(phone)
        precondition(route.selected == nil && !route.isExternal)
        route.disconnect(monitor)
        route.connect(phone, role: .device)
        precondition(route.selected == phone && !route.isExternal)
        print("Display routing: all lifecycle checks passed")
    }
}
