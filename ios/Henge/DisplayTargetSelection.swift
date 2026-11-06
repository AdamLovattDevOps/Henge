import Foundation

/// Chooses one presentation surface for the running Mac. An external accessory
/// cannot keep presenting after its owning session's device surface has gone.
struct DisplayTargetSelection {
    enum Role { case device, external }

    private struct Target {
        let id: UUID
        let role: Role
    }

    private var targets: [Target] = []

    var selected: UUID? {
        guard let device = targets.last(where: { $0.role == .device }) else { return nil }
        return targets.last(where: { $0.role == .external })?.id ?? device.id
    }

    var isExternal: Bool {
        guard let selected else { return false }
        return targets.first(where: { $0.id == selected })?.role == .external
    }

    mutating func connect(_ id: UUID, role: Role) {
        // A layout refresh must not change which of two overlapping surfaces wins.
        guard !targets.contains(where: { $0.id == id }) else { return }
        targets.append(Target(id: id, role: role))
    }

    mutating func disconnect(_ id: UUID) {
        targets.removeAll { $0.id == id }
    }
}
