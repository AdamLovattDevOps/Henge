import SwiftUI
import AVFAudio

@main
struct HengeApp: App {
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Order matters: HengeLog takes over stderr, so the engine's and the JIT
        // allocator's first lines land in Documents/henge.log too.
        HengeLog.start()
        HengeLog.logFootprint("app-launch")
        JITBootstrap.noteLaunchState()
        // Without the guard, a brk no debugger answers kills the app instead of failing.
        JITBootstrap.installTrapGuard()
        try? AVAudioSession.sharedInstance().setCategory(.playback, options: [.mixWithOthers])
        Machines.shared.collectImports()
        // The display question: does this iOS have Apple's paravirtualized graphics at all?
        for path in ["/System/Library/Frameworks/ParavirtualizedGraphics.framework/ParavirtualizedGraphics",
                     "/System/Library/PrivateFrameworks/ParavirtualizedGraphics.framework/ParavirtualizedGraphics"] {
            HengeLog.log("app", "\(path): \(dlopen(path, RTLD_LAZY) != nil ? "present" : "absent")")
        }
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .onOpenURL { url in HengeLog.log("app", "opened with \(url.absoluteString)") } // henge:// back from LocalDevVPN
        }
    }
}

struct RootView: View {
    @ObservedObject private var machines = Machines.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var running: VirtualMac?

    var body: some View {
        ZStack {
            TabView {
                HomeView(running: $running)
                    .tabItem { Label("Machines", systemImage: "macwindow") }
                SettingsView()
                    .tabItem { Label("Settings", systemImage: "gearshape") }
            }
            if let running {
                SessionView(mac: running) { self.running = nil }
                    .transition(.opacity)
            }
        }
        .sheet(isPresented: $jit.showSetup) { JITSetupFlow() }
        .onChange(of: scenePhase) { phase in
            guard phase == .active else { return }
            // Back from StikDebug or TrollStore: claim the JIT region while the debugger is there.
            JITBootstrap.prewarm()
            machines.collectImports()
            jit.enableAtLaunchIfAsked()
        }
        .onChange(of: jit.attachGeneration) { _ in JITBootstrap.prewarm() } // the built-in helper attached
    }
}

extension View {
    /// Husk's screens are written against an iOS 15 shim; Henge needs iOS 17, so this is the real thing.
    func hengeDestination<D: Hashable, C: View>(for data: D.Type, @ViewBuilder destination: @escaping (D) -> C) -> some View {
        navigationDestination(for: data, destination: destination)
    }
}
